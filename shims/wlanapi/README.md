# wlanapi 垫片

让依赖 Windows Wi-Fi 栈的程序在 Wine 里能跑起来。

## 解决什么问题

Wine 的 `wlanapi.dll` **只有函数名没有实现**（46 KB / 39 个导出，全是空壳），
调用即 `wine: unimplemented function ... aborting`。

对华为电脑管家，这条链是：

```
登录华为账号 / 我的设备 / 开启多屏协同
    ↓  （点了没反应）
HiConnectivityService.exe
    ↓  加载
RealtekWiFi.dll
    ↓  DllMain 里调 WlanOpenHandle / WlanSetSecuritySettings ...
    ↓  失败 → DllMain 返回 FALSE → 整个服务加载失败
```

`RealtekWiFi.dll` 是 Realtek 的 Wi-Fi Direct / IHV 封装层，导入：

```
Wlanapi.dll   wlanui.dll   CreateFileA/W   ole32   OLEAUT32   RPCRT4
导出 IHV_Oid / IHV_WlanGetHostedNetworkCapable /
     IHV_WlanRtlGetWiFiDirectLinkCurrentRole / ...（一堆 Wi-Fi Direct 能力查询）
```

**垫片让这些调用不再崩，服务就能起来，UI 的调用链随之打通。**

## 部署（不需要重编 Wine）

Windows 的 DLL 搜索顺序里**程序目录优先**，所以：

```bash
# 1. 编译
./build.sh

# 2. 放进前缀（两个位置都放，见下面"为什么"）
WINEPREFIX=~/pcm-wine
cp wlanapi-x64.dll "$WINEPREFIX/drive_c/windows/system32/wlanapi.dll"
#   华为的 exe 分散在多个子目录，只放程序目录会导致
#   别的目录里的 exe 找不到（报 c0000135 STATUS_DLL_NOT_FOUND）

# 3. 强制用 native
export WINEDLLOVERRIDES="mscoree=;wlanapi=n"
```

日志默认写到 `C:\wlanapi_shim.log`（可用环境变量 `WLANAPI_SHIM_LOG` 覆盖）。

## 当前实现程度：v0.1 探路版

目标不是真去操作网卡，而是**把门推开、看清门后还有什么**：

| 已做 | 说明 |
|---|---|
| `WlanOpenHandle` / `WlanCloseHandle` / `WlanFreeMemory` | 成功返回，伪造句柄 |
| `WlanEnumInterfaces` | 返回 **1 个假接口**（0 个接口会让上层直接放弃后续流程） |
| `WlanQueryInterface` | 常见 opcode 返回正确尺寸的结构；**认不出来的一律诚实返回 `NOT_SUPPORTED` 并记日志**——探路阶段正是靠这条日志知道还差什么 |
| `WlanHostedNetwork*` | 设置类调用假装成功；`ForceStart` 诚实返回 `NOT_SUPPORTED` |
| 未知调用 | 全部进日志 |

**没有做的**：真正建 SoftAP / Wi-Fi Direct、真正发包、`WlanIhvControl`（厂商私有 ioctl）。

## 路线图

下一步是把网络真正接到 Linux 原生——垫片只负责让上层以为成功：

| Windows API | Linux 原生 |
|---|---|
| SoftAP / Wi-Fi Direct GO | `hostapd` / `wpa_supplicant` P2P |
| `AddIPAddress` → `192.168.137.1/24` | `ip addr add` |
| 蓝牙发现 | BlueZ（Wine 的 `bluetoothapis` 同样是空壳） |
| `WlanIhvControl`（厂商私有） | 参考 `huawei-pcmanager-14x-compat` 的 `murocapi` Rust shim |

## 踩过的坑

**`WLAN_INTERFACE_INFO_LIST` 的 `InterfaceInfo` 是柔性数组 `[]`** ——
`sizeof` 只有 **8 字节**（两个 DWORD），必须自己加上项数：

```c
SIZE_T sz = sizeof(WLAN_INTERFACE_INFO_LIST) + n * sizeof(WLAN_INTERFACE_INFO);
```

不然就是写越界踩坏堆。同一个头文件里 `WLAN_AVAILABLE_NETWORK_LIST` 等用的却是 `[1]`，
**不一致，别照抄**。

**mingw 的 `wlanapi.h` 完全没有 Hosted Network 段**（实测 0 处声明），
所以本垫片自带了那一段的类型定义（`_DEFINED` 哨兵包着，将来头文件补上也不冲突）。

**`WlanSetSecuritySettings` 的真实签名**是 `(HANDLE, WLAN_SECURABLE_OBJECT, LPCWSTR)`，
不是 `(HANDLE, GUID*, WLAN_SECURITY_ATTRIBUTES*)`。照 MSDN 抄容易错，以头文件为准。

## 法务

垫片本身是**独立实现**（MIT），不包含微软或华为的任何代码。
它只做互操作：让用户在自己机器上使用自己已购设备的功能。
