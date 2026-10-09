/*
 * wlanapi 垫片 —— 让依赖 Windows Wi-Fi 栈的程序在 Wine 里能跑起来。
 *
 * 为什么需要
 * ----------
 * Wine 的 wlanapi.dll 只有函数名没有实现（空壳），调用即 abort。
 * 华为电脑管家的 HiConnectivityService.exe 要加载 RealtekWiFi.dll，
 * 后者的 DllMain 会调 WlanOpenHandle 之类的接口，一失败就拒绝加载，
 * 于是服务起不来 —— 表现是"登录 / 我的设备 / 多屏协同 点了没反应"。
 *
 * 设计取向
 * --------
 * 这是 **v0.1 探路版**：目标不是真去操作无线网卡，而是
 *   1. 让 WlanOpenHandle / WlanEnumInterfaces 这些能成功返回
 *   2. 能力查询返回"看起来合理"的值，而不是"不支持"
 *   3. 把所有调用记进日志，好用日志驱动的方式迭代出还缺什么
 *
 * 之后再把 SoftAP / Wi-Fi Direct 真正接到 Linux 原生
 * （hostapd / wpa_supplicant P2P / ip addr）。
 *
 * 部署
 * ----
 * 编译出的 wlanapi.dll 放进程序目录（Windows 的 DLL 搜索顺序里程序目录优先），
 * 并设 WINEDLLOVERRIDES="wlanapi=n" 强制用 native。
 *
 * 构建：见同目录 build.sh
 */

#include <windows.h>
#include <wlanapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>

/* ---------------------------------------------------------------
 * mingw 的 wlanapi.h 里**完全没有 Hosted Network 段**（实测 0 处声明），
 * 但多屏协同恰恰全靠它（电脑开 SoftAP、手机连上来）。这里自带最小定义。
 * 用 _DEFINED 哨兵包起来，将来头文件补上了也不会冲突。
 * --------------------------------------------------------------- */

#ifndef WLANAPI_SHIM_HOSTED_NETWORK_TYPES
#define WLANAPI_SHIM_HOSTED_NETWORK_TYPES

typedef enum _WLAN_HOSTED_NETWORK_STATE {
    wlan_hosted_network_unavailable = 0,
    wlan_hosted_network_idle,
    wlan_hosted_network_active
} WLAN_HOSTED_NETWORK_STATE, *PWLAN_HOSTED_NETWORK_STATE;

typedef enum _WLAN_HOSTED_NETWORK_PEER_AUTH_STATE {
    wlan_hosted_network_peer_state_invalid = 0,
    wlan_hosted_network_peer_state_authenticated
} WLAN_HOSTED_NETWORK_PEER_AUTH_STATE;

typedef struct _WLAN_HOSTED_NETWORK_PEER_STATE {
    DOT11_MAC_ADDRESS                  PeerMacAddress;
    WLAN_HOSTED_NETWORK_PEER_AUTH_STATE PeerAuthState;
} WLAN_HOSTED_NETWORK_PEER_STATE, *PWLAN_HOSTED_NETWORK_PEER_STATE;

typedef struct _WLAN_HOSTED_NETWORK_STATUS {
    WLAN_HOSTED_NETWORK_STATE       wlanHostedNetworkState;
    GUID                            IPDeviceID;
    DOT11_MAC_ADDRESS               wlanHostedNetworkBSSID;
    DOT11_PHY_TYPE                  dot11PhyType;
    ULONG                           ulChannelFrequency;
    DWORD                           dwNumberOfPeers;
    WLAN_HOSTED_NETWORK_PEER_STATE  PeerList[1];
} WLAN_HOSTED_NETWORK_STATUS, *PWLAN_HOSTED_NETWORK_STATUS;

typedef enum _WLAN_HOSTED_NETWORK_REASON {
    wlan_hosted_network_reason_success = 0,
    wlan_hosted_network_reason_unsupported,
    wlan_hosted_network_reason_bad_parameters,
    wlan_hosted_network_reason_service_shutting_down,
    wlan_hosted_network_reason_insufficient_resources,
    wlan_hosted_network_reason_elevation_required,
    wlan_hosted_network_reason_read_only,
    wlan_hosted_network_reason_persistence_failed,
    wlan_hosted_network_reason_crypt_error,
    wlan_hosted_network_reason_impersonation,
    wlan_hosted_network_reason_stop_before_start,
    wlan_hosted_network_reason_interface_available,
    wlan_hosted_network_reason_interface_unavailable,
    wlan_hosted_network_reason_miniport_stopped,
    wlan_hosted_network_reason_miniport_started,
    wlan_hosted_network_reason_incompatible_connection_started,
    wlan_hosted_network_reason_incompatible_connection_stopped,
    wlan_hosted_network_reason_user_action,
    wlan_hosted_network_reason_client_abort,
    wlan_hosted_network_reason_ap_start_failed,
    wlan_hosted_network_reason_peer_arrived,
    wlan_hosted_network_reason_peer_departed,
    wlan_hosted_network_reason_peer_timeout,
    wlan_hosted_network_reason_gp_denied,
    wlan_hosted_network_reason_service_unavailable,
    wlan_hosted_network_reason_device_change,
    wlan_hosted_network_reason_properties_change,
    wlan_hosted_network_reason_virtual_station_blocking_use,
    wlan_hosted_network_reason_service_available_on_virtual_station
} WLAN_HOSTED_NETWORK_REASON, *PWLAN_HOSTED_NETWORK_REASON;

typedef enum _WLAN_HOSTED_NETWORK_OPCODE {
    wlan_hosted_network_opcode_connection_settings = 0,
    wlan_hosted_network_opcode_security_settings,
    wlan_hosted_network_opcode_station_profile,
    wlan_hosted_network_opcode_enable
} WLAN_HOSTED_NETWORK_OPCODE, *PWLAN_HOSTED_NETWORK_OPCODE;

#endif /* WLANAPI_SHIM_HOSTED_NETWORK_TYPES */

/* ---------------------------------------------------------------- 日志 */

/* 日志路径可用环境变量 WLANAPI_SHIM_LOG 覆盖；默认写到 C:\wlanapi_shim.log */
static FILE *g_log = NULL;

static void log_open(void)
{
    if (g_log) return;
    const char *path = getenv("WLANAPI_SHIM_LOG");
    if (!path || !*path) path = "C:\\wlanapi_shim.log";
    g_log = fopen(path, "a");
}

static void logf_(const char *fmt, ...)
{
    log_open();
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

#define LOG(...) logf_(__VA_ARGS__)

/* ------------------------------------------------------------ 假句柄 */

/* 句柄只是个不透明的非 NULL 值；Wine 里用不着真对象 */
#define FAKE_HANDLE ((HANDLE)(ULONG_PTR)0x574C414EU) /* 'WLAN' */

/* 一个固定的假接口 GUID，便于日志里辨认 */
static const GUID FAKE_GUID = {
    0x11111111, 0x2222, 0x3333,
    { 0x44, 0x44, 0x55, 0x55, 0x66, 0x66, 0x77, 0x77 }
};

/* 用我们自己的分配器，保证 WlanFreeMemory 能配对释放 */
static PVOID shim_alloc(size_t n)
{
    return calloc(1, n);
}

/* -------------------------------------------------------- 基础三件套 */

DWORD WINAPI WlanOpenHandle(DWORD dwClientVersion, PVOID pReserved,
                            PDWORD pdwNegotiatedVersion, PHANDLE phClientHandle)
{
    LOG("[shim] WlanOpenHandle(clientVersion=%lu)", (unsigned long)dwClientVersion);

    if (!phClientHandle) return ERROR_INVALID_PARAMETER;

    /* 协商成 VISTA(2)；版本太低会拿不到 HostedNetwork 那批接口 */
    if (pdwNegotiatedVersion) *pdwNegotiatedVersion = 2;
    *phClientHandle = FAKE_HANDLE;

    return ERROR_SUCCESS;
}

DWORD WINAPI WlanCloseHandle(HANDLE hClientHandle, PVOID pReserved)
{
    LOG("[shim] WlanCloseHandle(%p)", hClientHandle);
    return ERROR_SUCCESS;
}

VOID WINAPI WlanFreeMemory(PVOID pMemory)
{
    LOG("[shim] WlanFreeMemory(%p)", pMemory);
    free(pMemory);
}

/* ------------------------------------------------------------ 枚举接口 */

/* 造一个假接口。探路阶段"有一个无线网卡"比"一个都没有"更有用 ——
 * 多数程序在拿到 0 个接口时会直接放弃后续流程。 */
DWORD WINAPI WlanEnumInterfaces(HANDLE hClientHandle, PVOID pReserved,
                                PWLAN_INTERFACE_INFO_LIST *ppInterfaceList)
{
    LOG("[shim] WlanEnumInterfaces()");

    if (!ppInterfaceList) return ERROR_INVALID_PARAMETER;

    /* ★ 坑：wlanapi.h 里 WLAN_INTERFACE_INFO_LIST.InterfaceInfo 是**柔性数组** `[]`，
     *   所以 sizeof 只有 8 字节（两个 DWORD）。必须自己加上项数，
     *   否则就是写越界踩坏堆。
     *   （同一头文件里 WLAN_AVAILABLE_NETWORK_LIST 等用的是 `[1]`，不一致，别照抄。） */
    SIZE_T sz = sizeof(WLAN_INTERFACE_INFO_LIST) + sizeof(WLAN_INTERFACE_INFO);
    PWLAN_INTERFACE_INFO_LIST list = shim_alloc(sz);
    if (!list) return ERROR_NOT_ENOUGH_MEMORY;

    list->dwNumberOfItems = 1;
    list->dwIndex         = 0;
    list->InterfaceInfo[0].InterfaceGuid = FAKE_GUID;
    wcsncpy(list->InterfaceInfo[0].strInterfaceDescription,
            L"Wireless Network Connection (wlanapi shim)", 255);
    list->InterfaceInfo[0].isState = wlan_interface_state_connected;

    *ppInterfaceList = list;
    LOG("[shim]   -> 1 个假接口");
    return ERROR_SUCCESS;
}

/* ------------------------------------------------------------ 查询接口 */

/* 按 opcode 返回合适大小的结构。返回错尺寸的数据会直接把调用方写坏，
 * 所以这里对拿不准的 opcode 一律诚实返回 NOT_SUPPORTED 并记日志 —— 探路阶段
 * 我们正是靠这条日志知道还差什么。 */
DWORD WINAPI WlanQueryInterface(HANDLE hClientHandle, const GUID *pInterfaceGuid,
                                WLAN_INTF_OPCODE OpCode, PVOID pReserved,
                                PDWORD pdwDataSize, PVOID *ppData,
                                PWLAN_OPCODE_VALUE_TYPE pOpcodeValueType)
{
    if (pdwDataSize) *pdwDataSize = 0;
    if (ppData)      *ppData = NULL;
    if (pOpcodeValueType) *pOpcodeValueType = wlan_opcode_value_type_query_only;

    if (!ppData || !pdwDataSize) return ERROR_INVALID_PARAMETER;

    PVOID  buf = NULL;
    SIZE_T len = 0;

    switch (OpCode) {
    case wlan_intf_opcode_interface_state: {
        len = sizeof(WLAN_INTERFACE_STATE);
        buf = shim_alloc(len);
        if (buf) *(WLAN_INTERFACE_STATE *)buf = wlan_interface_state_connected;
        break;
    }
    case wlan_intf_opcode_autoconf_enabled:
    case wlan_intf_opcode_background_scan_enabled:
    case wlan_intf_opcode_media_streaming_mode:
    case wlan_intf_opcode_hosted_network_capable:   /* ★ 多屏协同关心这个 */
    case wlan_intf_opcode_management_frame_protection_capable: {
        len = sizeof(BOOL);
        buf = shim_alloc(len);
        /* hosted_network_capable 报 TRUE —— 我们要让它觉得能做 SoftAP */
        if (buf) *(BOOL *)buf = TRUE;
        break;
    }
    case wlan_intf_opcode_channel_number:
    case wlan_intf_opcode_rssi:
    case wlan_intf_opcode_current_operation_mode: {
        len = sizeof(ULONG);
        buf = shim_alloc(len);
        break;
    }
    case wlan_intf_opcode_bss_type: {
        len = sizeof(DOT11_BSS_TYPE);
        buf = shim_alloc(len);
        if (buf) *(DOT11_BSS_TYPE *)buf = dot11_BSS_type_infrastructure;
        break;
    }
    case wlan_intf_opcode_current_connection: {
        len = sizeof(WLAN_CONNECTION_ATTRIBUTES);
        buf = shim_alloc(len);
        if (buf) {
            PWLAN_CONNECTION_ATTRIBUTES a = buf;
            a->isState = wlan_interface_state_disconnected;
            a->wlanConnectionMode = wlan_connection_mode_profile;
            a->strProfileName[0] = 0;
        }
        break;
    }
    case wlan_intf_opcode_radio_state: {
        len = sizeof(WLAN_RADIO_STATE);
        buf = shim_alloc(len);
        if (buf) {
            PWLAN_RADIO_STATE r = buf;
            r->dwNumberOfPhys = 1;
            r->PhyRadioState[0].dwPhyIndex = 0;
            r->PhyRadioState[0].dot11SoftwareRadioState = dot11_radio_state_on;
            r->PhyRadioState[0].dot11HardwareRadioState = dot11_radio_state_on;
        }
        break;
    }
    default:
        LOG("[shim] WlanQueryInterface(opcode=0x%08lx) -> NOT_SUPPORTED  <-- 按需补实现",
            (unsigned long)OpCode);
        return ERROR_NOT_SUPPORTED;
    }

    if (!buf) return ERROR_NOT_ENOUGH_MEMORY;

    *pdwDataSize = (DWORD)len;
    *ppData      = buf;
    LOG("[shim] WlanQueryInterface(opcode=0x%08lx) -> ok (%lu 字节)",
        (unsigned long)OpCode, (unsigned long)len);
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanSetInterface(HANDLE hClientHandle, const GUID *pInterfaceGuid,
                              WLAN_INTF_OPCODE OpCode, DWORD dwDataSize,
                              PVOID pData, PVOID pReserved)
{
    LOG("[shim] WlanSetInterface(opcode=0x%08lx, %lu 字节) -> 假装成功",
        (unsigned long)OpCode, (unsigned long)dwDataSize);
    /* 探路阶段：只要别让它因为"设置失败"而放弃就行 */
    return ERROR_SUCCESS;
}

/* -------------------------------------------------------- 扫描 / 连接 */

DWORD WINAPI WlanScan(HANDLE hClientHandle, const GUID *pInterfaceGuid,
                      const PDOT11_SSID pDot11Ssid, const PWLAN_RAW_DATA pIeData,
                      PVOID pReserved)
{
    LOG("[shim] WlanScan()");
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanGetAvailableNetworkList(HANDLE hClientHandle,
                                         const GUID *pInterfaceGuid,
                                         DWORD dwFlags, PVOID pReserved,
                                         PWLAN_AVAILABLE_NETWORK_LIST *ppAvailableNetworkList)
{
    LOG("[shim] WlanGetAvailableNetworkList() -> 空列表");
    if (!ppAvailableNetworkList) return ERROR_INVALID_PARAMETER;
    SIZE_T sz = sizeof(WLAN_AVAILABLE_NETWORK_LIST);
    PWLAN_AVAILABLE_NETWORK_LIST l = shim_alloc(sz);
    if (!l) return ERROR_NOT_ENOUGH_MEMORY;
    l->dwNumberOfItems = 0;
    l->dwIndex = 0;
    *ppAvailableNetworkList = l;
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanGetNetworkBssList(HANDLE hClientHandle, const GUID *pInterfaceGuid,
                                   const PDOT11_SSID pDot11Ssid, DOT11_BSS_TYPE dot11BssType,
                                   BOOL bSecurityEnabled, PVOID pReserved,
                                   PWLAN_BSS_LIST *ppWlanBssList)
{
    LOG("[shim] WlanGetNetworkBssList() -> 空列表");
    if (!ppWlanBssList) return ERROR_INVALID_PARAMETER;
    PWLAN_BSS_LIST l = shim_alloc(sizeof(WLAN_BSS_LIST));
    if (!l) return ERROR_NOT_ENOUGH_MEMORY;
    l->dwTotalSize = (DWORD)sizeof(WLAN_BSS_LIST);
    l->dwNumberOfItems = 0;
    *ppWlanBssList = l;
    return ERROR_SUCCESS;
}

/* -------------------------------------------------------- Profile */

DWORD WINAPI WlanGetProfileList(HANDLE hClientHandle, const GUID *pInterfaceGuid,
                                PVOID pReserved, PWLAN_PROFILE_INFO_LIST *ppProfileList)
{
    LOG("[shim] WlanGetProfileList() -> 空列表");
    if (!ppProfileList) return ERROR_INVALID_PARAMETER;
    PWLAN_PROFILE_INFO_LIST l = shim_alloc(sizeof(WLAN_PROFILE_INFO_LIST));
    if (!l) return ERROR_NOT_ENOUGH_MEMORY;
    l->dwNumberOfItems = 0;
    l->dwIndex = 0;
    *ppProfileList = l;
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanGetProfile(HANDLE hClientHandle, const GUID *pInterfaceGuid,
                            LPCWSTR strProfileName, PVOID pReserved,
                            LPWSTR *pstrProfileXml, DWORD *pdwFlags,
                            PDWORD pdwGrantedAccess)
{
    LOG("[shim] WlanGetProfile() -> 没有这个 profile");
    return ERROR_NOT_FOUND;
}

DWORD WINAPI WlanSetProfile(HANDLE hClientHandle, const GUID *pInterfaceGuid,
                            DWORD dwFlags, LPCWSTR strProfileXml,
                            LPCWSTR strAllUserProfileSecurity, BOOL bOverwrite,
                            PVOID pReserved, PDWORD pdwReasonCode)
{
    LOG("[shim] WlanSetProfile() -> 拒绝（探路阶段不落盘任何 profile）");
    if (pdwReasonCode) *pdwReasonCode = 0;
    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI WlanDeleteProfile(HANDLE hClientHandle, const GUID *pInterfaceGuid,
                               LPCWSTR strProfileName, PVOID pReserved)
{
    LOG("[shim] WlanDeleteProfile()");
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanSetProfilePosition(HANDLE hClientHandle, const GUID *pInterfaceGuid,
                                    LPCWSTR strProfileName, DWORD dwPosition, PVOID pReserved)
{
    LOG("[shim] WlanSetProfilePosition()");
    return ERROR_SUCCESS;
}

/* -------------------------------------------------------- 连接 / 断开 */

DWORD WINAPI WlanConnect(HANDLE hClientHandle, const GUID *pInterfaceGuid,
                         const PWLAN_CONNECTION_PARAMETERS pConnectionParameters,
                         PVOID pReserved)
{
    LOG("[shim] WlanConnect() -> NOT_SUPPORTED（真接网要靠 hostapd/wpa_supplicant）");
    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI WlanDisconnect(HANDLE hClientHandle, const GUID *pInterfaceGuid, PVOID pReserved)
{
    LOG("[shim] WlanDisconnect()");
    return ERROR_SUCCESS;
}

/* ------------------------------------------------- Hosted Network(SoftAP) */

/* 多屏协同的链路就建在这上面：电脑开热点、手机连上来。
 * 探路阶段先让"初始化设置"成功，把门推开看后面还有什么。 */

DWORD WINAPI WlanHostedNetworkInitSettings(HANDLE hClientHandle, PVOID pReserved,
                                           PWLAN_HOSTED_NETWORK_REASON pFailReason)
{
    LOG("[shim] WlanHostedNetworkInitSettings() -> 假装失败（暂无 SoftAP 后端）");
    if (pFailReason) *pFailReason = wlan_hosted_network_reason_unsupported;
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanHostedNetworkQueryStatus(HANDLE hClientHandle,
                                          PWLAN_HOSTED_NETWORK_STATUS *ppWlanHostedNetworkStatus,
                                          PVOID pReserved)
{
    if (!ppWlanHostedNetworkStatus) return ERROR_INVALID_PARAMETER;
    PWLAN_HOSTED_NETWORK_STATUS s = shim_alloc(sizeof(WLAN_HOSTED_NETWORK_STATUS));
    if (!s) return ERROR_NOT_ENOUGH_MEMORY;
    s->wlanHostedNetworkState = wlan_hosted_network_unavailable;
    s->dwNumberOfPeers = 0;   /* 其余由 calloc 清零 */
    *ppWlanHostedNetworkStatus = s;
    LOG("[shim] WlanHostedNetworkQueryStatus() -> unavailable");
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanHostedNetworkQueryProperty(HANDLE hClientHandle, WLAN_HOSTED_NETWORK_OPCODE OpCode,
                                            PDWORD pdwDataSize, PVOID *ppvData,
                                            PWLAN_OPCODE_VALUE_TYPE pWlanOpcodeValueType,
                                            PVOID pReserved)
{
    LOG("[shim] WlanHostedNetworkQueryProperty(opcode=%d)", (int)OpCode);
    if (!pdwDataSize || !ppvData) return ERROR_INVALID_PARAMETER;
    *pdwDataSize = sizeof(ULONG);
    *ppvData = shim_alloc(sizeof(ULONG));
    if (!*ppvData) return ERROR_NOT_ENOUGH_MEMORY;
    if (pWlanOpcodeValueType) *pWlanOpcodeValueType = wlan_opcode_value_type_query_only;
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanHostedNetworkSetProperty(HANDLE hClientHandle, WLAN_HOSTED_NETWORK_OPCODE OpCode,
                                          DWORD dwDataSize, PVOID pvData,
                                          PWLAN_HOSTED_NETWORK_REASON pFailReason, PVOID pReserved)
{
    LOG("[shim] WlanHostedNetworkSetProperty(opcode=%d, %lu 字节) -> 假装成功",
        (int)OpCode, (unsigned long)dwDataSize);
    if (pFailReason) *pFailReason = wlan_hosted_network_reason_success;
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanHostedNetworkQuerySecondaryKey(HANDLE hClientHandle, PDWORD pdwKeyLength,
                                                PUCHAR pucKeyData, PBOOL pbIsPassPhrase,
                                                PBOOL pbPersistent, PWLAN_HOSTED_NETWORK_REASON pFailReason,
                                                PVOID pReserved)
{
    LOG("[shim] WlanHostedNetworkQuerySecondaryKey()");
    if (pdwKeyLength) *pdwKeyLength = 0;
    if (pbIsPassPhrase) *pbIsPassPhrase = TRUE;
    if (pbPersistent) *pbPersistent = FALSE;
    if (pFailReason) *pFailReason = wlan_hosted_network_reason_success;
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanHostedNetworkSetSecondaryKey(HANDLE hClientHandle, DWORD dwKeyLength,
                                              PUCHAR pucKeyData, BOOL bIsPassPhrase, BOOL bPersistent,
                                              PWLAN_HOSTED_NETWORK_REASON pFailReason, PVOID pReserved)
{
    LOG("[shim] WlanHostedNetworkSetSecondaryKey(%lu 字节) -> 假装成功", (unsigned long)dwKeyLength);
    if (pFailReason) *pFailReason = wlan_hosted_network_reason_success;
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanHostedNetworkForceStart(HANDLE hClientHandle,
                                         PWLAN_HOSTED_NETWORK_REASON pFailReason, PVOID pReserved)
{
    LOG("[shim] WlanHostedNetworkForceStart() -> NOT_SUPPORTED（待接 hostapd）");
    if (pFailReason) *pFailReason = wlan_hosted_network_reason_unsupported;
    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI WlanHostedNetworkForceStop(HANDLE hClientHandle,
                                        PWLAN_HOSTED_NETWORK_REASON pFailReason, PVOID pReserved)
{
    LOG("[shim] WlanHostedNetworkForceStop()");
    if (pFailReason) *pFailReason = wlan_hosted_network_reason_success;
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanHostedNetworkStopUsing(HANDLE hClientHandle, PVOID pReserved)
{
    LOG("[shim] WlanHostedNetworkStopUsing()");
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanHostedNetworkRefreshSecuritySettings(HANDLE hClientHandle,
                                                      PWLAN_HOSTED_NETWORK_REASON pFailReason,
                                                      PVOID pReserved)
{
    LOG("[shim] WlanHostedNetworkRefreshSecuritySettings()");
    if (pFailReason) *pFailReason = wlan_hosted_network_reason_success;
    return ERROR_SUCCESS;
}

/* ------------------------------------------------------------ 杂项 */

DWORD WINAPI WlanRegisterNotification(HANDLE hClientHandle, DWORD dwNotifSource,
                                      BOOL bIgnoreDuplicate, WLAN_NOTIFICATION_CALLBACK funcCallback,
                                      PVOID pCallbackContext, PVOID pReserved, PDWORD pdwPrevNotifSource)
{
    LOG("[shim] WlanRegisterNotification(source=0x%08lx) -> 假装注册成功（不会真回调）",
        (unsigned long)dwNotifSource);
    if (pdwPrevNotifSource) *pdwPrevNotifSource = 0;
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanReasonCodeToString(DWORD dwReasonCode, DWORD dwBufferSize,
                                    PWCHAR pStringBuffer, PVOID pReserved)
{
    if (pStringBuffer && dwBufferSize) {
        _snwprintf(pStringBuffer, dwBufferSize / sizeof(WCHAR),
                   L"wlanapi-shim reason 0x%08lx", (unsigned long)dwReasonCode);
        pStringBuffer[(dwBufferSize / sizeof(WCHAR)) - 1] = 0;
    }
    LOG("[shim] WlanReasonCodeToString(0x%08lx)", (unsigned long)dwReasonCode);
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanSetSecuritySettings(HANDLE hClientHandle, WLAN_SECURABLE_OBJECT SecurableObject,
                                     LPCWSTR strModifiedSDDL)
{
    LOG("[shim] WlanSetSecuritySettings(object=%d) -> 假装成功  <-- 之前崩的就是这个",
        (int)SecurableObject);
    return ERROR_SUCCESS;
}

/* 厂商私有的 ioctl 通道。Realtek 的 IHV_* 会走这里，
 * 探路阶段返回 NOT_SUPPORTED，让上层自己决定降级。 */
DWORD WINAPI WlanIhvControl(HANDLE hClientHandle, const GUID *pInterfaceGuid,
                            WLAN_IHV_CONTROL_TYPE Type, DWORD dwInBufferSize, PVOID pInBuffer,
                            DWORD dwOutBufferSize, PVOID pOutBuffer, PDWORD pdwBytesReturned)
{
    LOG("[shim] WlanIhvControl(type=%d, in=%lu, out=%lu) -> NOT_SUPPORTED",
        (int)Type, (unsigned long)dwInBufferSize, (unsigned long)dwOutBufferSize);
    if (pdwBytesReturned) *pdwBytesReturned = 0;
    return ERROR_NOT_SUPPORTED;
}

/* ------------------------------------------------------------ DllMain */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hinstDLL; (void)lpvReserved;
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        LOG("==== wlanapi 垫片加载 (pid=%lu) ====", (unsigned long)GetCurrentProcessId());
        break;
    case DLL_PROCESS_DETACH:
        LOG("==== wlanapi 垫片卸载 ====");
        if (g_log) { fclose(g_log); g_log = NULL; }
        break;
    }
    return TRUE;   /* ★ 永远返回 TRUE —— 让调用方至少能加载起来 */
}
