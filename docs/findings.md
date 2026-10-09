# 调研与实测记录

本文是逐条实测出来的结论，不是推测。每条都附**判定依据**（日志原文 / 二进制证据），方便后续复核。

---

## 0. 总体架构：多屏协同的 PC 侧到底做什么

从破解项目的文档与日志里拼出来的链路：

```
1. 伪造机型        程序读 SMBIOS 判断是否华为电脑 → 需要 hook
2. 建网络          SoftAP / Wi-Fi Direct GO 虚拟适配器
3. 配 IP           AddIPAddress 给虚拟网卡配 192.168.137.1/24
4. 发现            蓝牙（靠近发现）或扫码
5. 链路            视频/输入走第 2 步建的网络
```

证据（`huawei-pcmanager-14x-compat` 的 `docs/PCMANAGER_14X_NOTES.md`）：

> 修复网络残留 — 清理错误残留的 `192.168.137.1`，避免 SoftAP 建链失败
> `AddIPAddress failed with error: 5010 The object already exists.`

> 补丁会写入 `version.dll`（hook SMBIOS）与 `murocapi.dll`（Intel Wi-Fi 私有接口 shim）

`192.168.137.1` 是 Windows ICS / 移动热点的默认地址 —— 说明链路是"**电脑开热点、手机连上来**"，**不是** Wi-Fi Direct P2P。这对 Linux 是好消息：`hostapd` 能做。

---

## 1. 安装器：NSIS + 自研皮肤插件

`+loaddll` 日志里的真实身份：

```
C:\users\...\Temp\nsvXXXX.tmp\
    ├── HwSkinButton.dll      华为自研皮肤按钮插件（导出 setskin）
    ├── nsisSlideshow.dll     背景幻灯片（导出 show / stop）
    ├── nsDialogs.dll  nsExec.dll  System.dll  UserInfo.dll
    └── img/                  皮肤资源（bg_title.bmp / btn_install.png / pic_00..04.png / Slides*.dat）
```

安装器主程序：`PCManager_Setup_*.exe` —— **外层是 32 位 PE**（自解压壳），内部载荷是 64 位。

### 绘制链路（objdump 完整导入表）

```
HwSkinButton.dll
    gdiplus.dll : GdipCreateBitmapFromFile → GdipCreateHBITMAPFromBitmap
    MSIMG32.dll : AlphaBlend
    GDI32.dll   : BitBlt  CreateCompatibleDC/Bitmap  SelectObject  SetBkMode
    USER32.dll  : FillRect  RedrawWindow  MapWindowPoints

nsisSlideshow.dll
    ole32.dll   : CoCreateInstance   ← CLSID_WICImagingFactory（WIC 加载 PNG）
    MSIMG32.dll : AlphaBlend
    GDI32.dll   : BitBlt  TextOutW  CreateFontIndirectW
    USER32.dll  : SetWindowLongW  CallWindowProcW（子类化）  GetWindowRect  ScreenToClient
```

**两个插件都没有 `SetWindowRgn` / `SetLayeredWindowAttributes` / `UpdateLayeredWindow` / `Dwm*`** —— 排除了分层窗口和 DWM 合成这两类猜测。

---

## 2. 逐个打掉的坑

### 2.1 `msvcp140.dll._Chmod` 未实现 → 安装器崩溃

```
wine: Call to unimplemented function msvcp140.dll._Chmod, aborting
```

Wine 自带的 `msvcp140.dll` 是残缺实现。
**解法**：`winetricks -q vcrun2022`（装微软原生 VC++ 运行库，`msvcp140.dll` 从 ~200KB builtin 换成 840KB 原生）。

### 2.2 界面中文全是方框

`drive_c/windows/Fonts/` **计数为 0** —— 前缀里一个字体都没有。
**解法**：把系统 Noto CJK 灌进前缀，并注册字体替换：

```bash
cp /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc "$WINEPREFIX/drive_c/windows/Fonts/"
# 再给 Microsoft YaHei / SimSun / SimHei 等注册 FontSubstitutes → Noto Sans CJK SC
```

### 2.3 皮肤背景不画

Wine builtin `gdiplus.dll` 不完整。
**解法**：`winetricks -q gdiplus`（从 Win7 SP1 KB976932 里提取原生 `gdiplus.dll`）。
效果：背景渐变 / logo / 标题都画出来了。

### 2.4 许可协议区一片空白（关键）

`+ole` 日志给出决定性证据：

```
trace:ole:CoCreateInstance {4590f811-1d3a-11d0-891f-00aa004b2e24} ...   ← CLSID_WICImagingFactory（背景图，正常）
trace:ole:CoCreateInstance {3050f3d6-98b5-11cf-bb82-00aa00bdce0b} ...   ← CLSID_HTMLDocument（协议页）
err:ole:apartment_getclassobject DllGetClassObject returned error 0x80040111 for dll mshtml.dll
err:ole:create_server class {3050f3d6-98b5-11cf-bb82-00aa00bdce0b} not registered
```

**Wine 的 `mshtml`（Gecko 后端）不提供 IE 的文档类 `CLSID_HTMLDocument`。** 这是功能缺失，没有开关能绕。
**解法**：`winetricks -q ie8`（换原生 Trident）。
效果：那段白块渲染出来了。

> 注：`winetricks -q ie8` 全部依赖从 `web.archive.org` 下载。若该域名走直连不通，需先让它走代理，否则报 `GnuTLS: TLS 链接非正常地终止了`。

### 2.5 主程序闪退 → `NotifyServiceStatusChangeA`

```
wine: Call from ... to unimplemented function ADVAPI32.dll.NotifyServiceStatusChangeA, aborting
```

Wine 的 `advapi32` **只导出 `NotifyServiceStatusChangeW`，没有 A（ANSI）变体**。

调用方定位（全目录字符串搜索）：

```
PCManager/libdistribute_service_monitor.dll      ← A 变体
Hiview/libdistribute_service_monitor.dll
PCManager/plugins/SystemDetectPlugin.dll
PCManager/HwMdcCenter.exe
```

**解法**：二进制补丁，把字符串 `NotifyServiceStatusChangeA` 改成 `...W`（**同长度，改 1 字节**）。
工具见 [`../patches/patch-ansi-to-wide.py`](../patches/patch-ansi-to-wide.py)。

效果：主程序启动成功，越过了这个崩溃。

### 2.6 主程序界面"一个字都没有"（★ 有个陷阱）

现象：窗口、图标、卡片图片**全都正常**，但**所有动态文字全缺**（导航栏只有图标、按钮只有图标）。

排查：`WINEDEBUG=+font` 显示程序要的是 `Microsoft YaHei`，Wine 也确实替换到了 `Noto Sans CJK SC`（1259 次），而且 `Noto Sans CJK SC` **确实注册进了族列表**。
→ **字体没问题。**

**真凶是 2.3 里为修安装器皮肤装的那个微软原生 `gdiplus`。** 原生 gdiplus 在 Wine 里画文字会失败。

```bash
wine reg add 'HKCU\Software\Wine\DllOverrides' /v gdiplus /d builtin /f
```

效果：文字立刻全部出现。

> **⚠️ 冲突**：安装器皮肤要 **原生** gdiplus，主程序文字要 **builtin** gdiplus，两者不能同时满足。
> 实操上：**装的时候用原生，装完切回 builtin**。

### 2.7 窗口透明度不对

界面是 **DuiLib** 写的（国产窗口库）。`PCManager.exe` 导入：

```
DuiLib::CPaintManagerUI::SetBlurMode(HWND, AccentState, ...)
DuiLib::CControlUI::SetBlur(int)
SetWindowCompositionAttribute     ← DuiLib.dll / DllControl.dll / UIControl.dll
```

毛玻璃走的是微软**未公开**的 `SetWindowCompositionAttribute` + `AccentState`
（`ACCENT_ENABLE_ACRYLICBLURBEHIND`）。**Wine 没实现它** → 模糊做不出来，
但窗口仍按"背后有模糊层"来画 → 半透明、把桌面透出来。

**解法**：关掉 DuiLib 的 blur。窗口定义在 `res/layout/*.xml`：

```xml
<Window size="1024,640" ... blur="true">   ← 改成 false
```

工具： [`../scripts/patch-duilib-blur.sh`](../scripts/patch-duilib-blur.sh)（12 个 layout 文件，自动备份 `.orig`）。

> 注意：这是**改过的程序文件**，电脑管家自我修复/重装会覆盖，需要重跑。


---

## 3. 当前战场：`wlanapi.dll`

打完上面的补丁，主程序立刻撞上：

```
wine: Call from ... to unimplemented function wlanapi.dll.WlanSetSecuritySettings, aborting
[ERROR] FUNc:OHOS::HiviewDFX::DFSClient::GetDetectCapability :SendIPCMessage fail.
```

**Wine 的 `wlanapi.dll` 只有函数名没有实现**（46KB / 39 个导出，全是壳）。
已知第一个崩的 API：`WlanSetSecuritySettings`。

### 判定方法（可复现）

```bash
# 看 Wine 到底实现了多少
winedump -j export /usr/lib/wine/i386-windows/wlanapi.dll

# 跑程序时看它第一次撞哪个
WINEDEBUG=+wlanapi,+loaddll wine PCManager.exe 2>&1 | grep -i 'unimplemented\|aborting'
```

### 计划：垫片 + Linux 原生后端

思路是**不让 Wine 真去建网**——Linux 侧用原生工具把网络建起来，垫片只负责让电脑管家以为它成功了。

| Windows API（电脑管家调的） | Linux 原生 |
|---|---|
| SoftAP / Wi-Fi Direct GO | `hostapd` / `wpa_supplicant` P2P |
| `AddIPAddress` → `192.168.137.1/24` | `ip addr add` |
| 蓝牙发现 | BlueZ（`bluetoothapis` 在 Wine 里同样是空壳） |
| Intel Wi-Fi 私有接口（`murocapi`） | 参考 `huawei-pcmanager-14x-compat` 的 Rust shim |

---

## 4. 排除掉的路线（别再试）

| 路线 | 为什么不通 |
|---|---|
| 蓝牙 HID 伪装键鼠 | 无官方/仓库方案，AUR 里 `hidclient`/`EmuBTHID`/`bluez-peripheral` 全无，且只有盲输入无画面 |
| Miracast + UIBC | 华为官方明确 Miraast 不支持键鼠反控（只 Cast+ 支持）；MiracleCast 的 UIBC 实测传不过去 |
| 移植鸿蒙电脑的 UI | 鸿蒙里 UI 跑在 WMS/图形/输入服务之上，不是可摘取的层 |
| 鸿蒙电脑 ISO | 华为不发布，系统只预载在指定机型 |
| OpenHarmony x86_64 镜像 | **不存在** —— 实测 `repo.huaweicloud.com/openharmony/os/` 从 3.2 到 7.0 全目录只有 ARM 板镜像 |

---

## 5. 待验证的目标

**这个项目最终要回答的问题**：Wine 里跑起来的电脑管家，能不能和**真实手机**建立多屏协同？

不确定因素：
- 模拟出来的"华为电脑"身份，手机认不认
- 华为账号信任链能否建立
- 自建 SoftAP 后，手机侧是否能正常完成发现与配对

**先决条件是打通 `wlanapi`。**
