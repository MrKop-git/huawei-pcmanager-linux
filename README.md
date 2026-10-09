# huawei-pcmanager-linux

在 Linux 上跑华为电脑管家（PC Manager），以打通**多屏协同**。

> **状态：早期探索中。** 安装器能跑通、主程序能启动，目前卡在 Wi-Fi 层（见下）。

## 为什么要做这个

华为电脑管家官方只有 Windows 版，多屏协同的接收端被绑定在华为电脑硬件上。
Linux 上没有官方或成熟的替代方案——现有几条路都不通：

| 路线 | 结论 |
|---|---|
| 蓝牙 HID 伪装键鼠 | 只有盲输入、无画面 |
| Miracast + UIBC | 华为官方明确 Miracast 不支持键鼠反控（只 Cast+ 支持） |
| 移植多屏协同协议 | 卡在设备认证的硬件身份上 |
| **Wine 跑官方客户端** | **本项目** |

## 目前到哪了

```
✅ 安装器在 Wine 里跑通（含 GUI）
✅ 电脑管家主程序能启动
❌ 卡在 wlanapi.dll.WlanSetSecuritySettings（Wine 未实现）
```

### 已经打掉的坑

| 现象 | 根因 | 解法 |
|---|---|---|
| 安装器"程序错误"崩溃 | Wine 缺 MSVC 运行时（`msvcp140.dll._Chmod`） | `winetricks -q vcrun2022` |
| 界面中文全是方框 | Wine 前缀里一个字体都没有 | 灌 Noto CJK + 注册字体替换 |
| 皮肤背景不画 | Wine builtin `gdiplus.dll` 不完整 | `winetricks -q gdiplus`（换微软原生） |
| 许可协议区一片空白 | Wine 的 `mshtml` 不实现 `CLSID_HTMLDocument` | `winetricks -q ie8`（换原生 Trident） |
| 主程序闪退 | `advapi32.NotifyServiceStatusChangeA` 未实现 | **二进制补丁 A→W**（见 `patches/`） |
| Wi-Fi 相关调用崩溃 | `wlanapi.dll` 在 Wine 里是空壳 | **未解决** ← 当前战场 |

细节见 [`docs/findings.md`](docs/findings.md)。

## 当前战场：`wlanapi.dll`

华为电脑管家创建多屏协同链路时要做：

```
建 SoftAP / Wi-Fi Direct GO  →  给虚拟网卡配 192.168.137.1/24  →  手机连上来
```

Wine 的 `wlanapi.dll` 只有函数名没有实现，调用即 `aborting`。
已知第一个崩的 API：**`WlanSetSecuritySettings`**。

**计划中的解法**：写一个 `wlanapi` 垫片，把 Windows 的 Wi-Fi API 映射到 Linux 原生：

| Windows API | Linux 原生 |
|---|---|
| SoftAP / Wi-Fi Direct GO | `hostapd` / `wpa_supplicant` P2P |
| `AddIPAddress` (`192.168.137.1/24`) | `ip addr add` |
| 蓝牙发现 | BlueZ |

垫片只需要让电脑管家以为它成功了——网络由 Linux 侧真建起来。

## 硬件前提

Wine 侧对无线网卡有要求：

- 网卡要支持 **P2P**（`iw list` 里能看见 `P2P-GO` / `P2P-client`）
- Intel 无线网卡另有私有接口（`murocapi`）需要额外处理

## 安装

见 [`docs/install.md`](docs/install.md)。

## 免责声明

- 本项目**不包含**华为的任何二进制文件；使用者需自行从华为官方获取安装包。
- 项目目标是**互操作**：让用户在自己的 Linux 机器上使用自己已购买设备的功能。
- 华为电脑管家官方仅支持华为 Windows 计算机。在你的机器上运行它可能违反其服务条款，请自行判断。
- 涉及的任何二进制补丁都只作用于**本地副本**，仅用于绕过 Wine 的功能缺失，不改变授权校验逻辑。

## 参与

有问题欢迎开 issue，附上：

```
wine --version
iw list | grep -A2 P2P
WINEDEBUG=+wlanapi,+loaddll wine PCManager.exe   # 的日志
```

## 许可

MIT（本项目自身的代码与文档）
