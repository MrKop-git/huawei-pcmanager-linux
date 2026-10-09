# 模块地图：电脑管家的分布式栈长什么样

这份文档是**用 WRE 全量扫出来的**，不是猜的。作用是给"该在哪一层做垫片"提供依据。

## 方法

```bash
# 档 0：横向粗筛全量（996 个 PE，~12 秒）
wre recon "<Huawei 目录>" --max-depth 8 --max-tier 0 --json \
    --target huawei-pcm --db hw.db

# 档 1：对短名单加深（函数边界 + 常量引用 + 调用图）
wre recon /tmp/shortlist --max-tier 1 --top 15 --json \
    --target huawei-pcm-t1 --db hw.db
```

> ⚠️ 两个坑：① `campaign` 不是干这个的（那是差分回归），**`recon` 才是**；
> ② 路径里有空格时不能 `$(cat list)` 展开，会拆词——先复制到无空格目录。

## 结论一：PC 侧有完整的分布式栈，不是只有个 UI

`HwDistributedMainService.exe` 的导入表就是一张架构图：

```
HwDistributedMainService.exe   ← 枢纽（多屏协同主服务）
 ├── libdeviceauth_service.dll  → DeviceAuthMainServiceInit   ★ 设备认证（本地实现）
 ├── libdm_server.dll           → DeviceMgrServerInit         设备管理
 ├── libsoftbus_server.dll      → SoftBusServer               ★ 分布式软总线
 ├── libwifi_sdk.dll            → Hid2d*, EnableP2p, ...      ★ Wi-Fi P2P
 ├── libbtframework.dll         → BleScan / BtFrameWork*      蓝牙
 └── libhuks_service.dll        → StartHksService             密钥库
```

**这推翻了一个早期判断。** 之前认为"设备认证卡在华为云 + 硬件身份、得重写半个鸿蒙"——
实际上 **HiChain 就在 PC 侧，是一批普通的 PE64 DLL**（`hichain_fwk.dll` / `HichainProxy.dll`）。

## 结论二：各层的量级差一个数量级

档 1 实测（`导出` 是最关键的指标——它决定垫片的接口面）：

| 模块 | 大小 | 导出 | 函数(确认) | 调用图边 | 加密常量 |
|---|---:|---:|---:|---:|---|
| `libdm_server.dll` | 3.21 MB | 249 | 9518 | 49164 | AES/MD5/SHA1/256/512 |
| `libdeviceauth_service.dll` | 3.05 MB | 275 | 6083 | 40385 | AES/MD5/SHA1/256/512 |
| `libsoftbus_adapter.dll` | 2.16 MB | 104 | 1223 | 20183 | AES/… |
| `libdm_datamgr.dll` | 2.14 MB | 265 | 4629 | 27349 | — |
| `libCastEngine.dll` | 1.71 MB | 79 | 1643 | 18709 | SHA-256 |
| `libbtframework.dll` | 1.65 MB | 74 | 4681 | 22217 | — |
| `nfonehop.dll` | 1.07 MB | 170 | 956 | 10968 | AES/… |
| `HichainProxy.dll` | 919 KB | 283 | 942 | 9615 | AES/… |
| `hichain_fwk.dll` | 909 KB | 83 | 710 | 9902 | AES/… |
| `HiConnectivitySDK.dll` | 431 KB | **210** | 429 | 7829 | — |
| **`libwifi_sdk.dll`** | 618 KB | **30** | 189 | 5378 | **无** |

**HiChain 是真实现**（AES+MD5+SHA 全套 + 24–38 张虚表），不是云端壳。
**`libwifi_sdk.dll` 是最小、最简、无加密的那个。**

## 结论三：切口在 `libwifi_sdk.dll`

全量 grep 出的依赖关系：

```
libwifi_sdk.dll   ← 只有两个消费者
      ① HwDistributedMainService.exe
      ② libsoftbus_server.dll

libbtframework.dll ← 同样这两个
libdeviceauth_service.dll / libdm_server.dll ← 只有 HwDistributedMainService.exe
libsoftbus_adapter.dll ← libsoftbus_client / libsoftbus_server / libsoftbus_utils
libCastEngine.dll ← HosDmsdpProvider.dll / SourceController.dll   （独立的 Cast 线）
```

`libwifi_sdk` 正好夹在「分布式服务 / 软总线」和「Wi-Fi 硬件」之间，**只有 2 个消费者**。

### 为什么不是垫 `wlanapi`

| 层 | 接口面 | 消费者 | 判断 |
|---|---|---|---|
| `wlanapi.dll` | 39 函数 + **行为语义** | 全系统可见 | ❌ 无底洞 |
| `HiConnectivitySDK` | 210 导出（C++ mangled） | 8 个 plugin | ❌ 太大 |
| **`libwifi_sdk.dll`** | **30 个纯 C 导出** | **2 个** | ✅ **切口** |

`libwifi_sdk` 的 30 个导出就是"多屏协同怎么建链路"的语义层：

```
IsWifiActive  GetBaseMacAddress  GetCurrentGroup
EnableP2p     RemoveGroup        StopDiscoverDevices
Hid2dCreateGroupEx   ← 组 P2P 组（当 GO）
Hid2dConnect         ← 连对端
Hid2dConfigIPAddr    ← 配 IP
Hid2dRequestGcIp     ← 请求 GO 的 IP
Hid2dGetRecommendChannelEx / Hid2dGetChannelListFor5GEx   选信道
RegisterP2pConnectionChangedCallback / PeersChanged / StateChanged / ...  通知
                        ↑ "Hid2d" = Hi D2D = Device-to-Device
```

每条都有干净的 Linux 原生对应物（`hostapd` / `wpa_supplicant` P2P / `ip addr` / `iw`）。

## 当前的两块拦路石

| # | 拦路石 | 证据 | 性质 |
|---|---|---|---|
| **1** | `HwDistributedMainService.exe` 在 `StartServiceCtrlDispatcherW` 崩溃 | 崩溃报告调用栈：`ucrtbase ← sechost ← service.exe` | Wine 的服务基础设施缺陷 |
| **2** | Wi-Fi P2P 没有 Linux 后端 | `libwifi_sdk` 的 30 个导出 | 待实现 |

**两块都通，多屏协同才可能建链。**

> 注：早期用 `patches/patch-ansi-to-wide.py` 把 `NotifyServiceStatusChangeA` 改成 `W` 版，
> 只是把"未实现函数直接 abort"换成了"进 Wine 的 sechost 里崩"——**没真正修好**。
> 真正的崩点是 `StartServiceCtrlDispatcherW`。
