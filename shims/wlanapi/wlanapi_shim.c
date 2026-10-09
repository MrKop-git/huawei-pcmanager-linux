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
#include <errno.h>

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

/* 上层用 SetProperty(opcode=connection_settings) 把 SSID 告诉我们 —— 这就是
 * 手机要连的那个热点名。 */
typedef struct _WLAN_HOSTED_NETWORK_CONNECTION_SETTINGS {
    DOT11_SSID dot11Ssid;
    DWORD      dwMaxNumberOfPeers;
} WLAN_HOSTED_NETWORK_CONNECTION_SETTINGS, *PWLAN_HOSTED_NETWORK_CONNECTION_SETTINGS;

typedef struct _WLAN_HOSTED_NETWORK_SECURITY_SETTINGS {
    DOT11_AUTH_ALGORITHM dot11AuthAlgo;
    DOT11_CIPHER_ALGORITHM dot11CipherAlgo;
} WLAN_HOSTED_NETWORK_SECURITY_SETTINGS, *PWLAN_HOSTED_NETWORK_SECURITY_SETTINGS;

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

/* =================================================================
 * Linux 侧后端（wlanapi-shimd）客户端
 *
 * 通信走文件，不走 IPC —— 好处是 Linux 那半可以完全独立测试，
 * 不用每次都把整个 Windows 进程树拉起来（Wine 调试一轮要几十秒）。
 *
 *   C:\wlanapi_shim\req.json     我们写 → shimd 读
 *   C:\wlanapi_shim\status.json  shimd 写 → 我们读
 * ================================================================= */

#define SHIM_COMM_DIR   "C:\\wlanapi_shim"
#define SHIM_REQ_PATH   SHIM_COMM_DIR "\\req.json"
#define SHIM_STS_PATH   SHIM_COMM_DIR "\\status.json"

/* 上层通过 WlanHostedNetworkSetProperty / SetSecondaryKey 告诉我们的参数，
 * 攒着，到 ForceStart 时一起发给后端。 */
static char g_ssid[64] = {0};
static char g_key[128] = {0};
static int  g_have_ssid = 0, g_have_key = 0;

/* 只保留可打印 ASCII；其余替换成 '_'（SSID/口令要写进 hostapd 配置） */
static void sanitize_ascii(const char *src, char *dst, size_t cap)
{
    size_t i = 0;
    if (!cap) return;
    for (; src && *src && i + 1 < cap; ++src) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') dst[i++] = '_';        /* 别破坏 JSON */
        else if (c >= 0x20 && c < 0x7F) dst[i++] = (char)c;
        else dst[i++] = '_';
    }
    dst[i] = 0;
}

/* 把宽字符 SSID / 口令转成 UTF-8（hostapd 的 ssid= 接受 UTF-8） */
static void wide_to_ascii(const WCHAR *w, char *dst, size_t cap)
{
    size_t i = 0;
    if (!cap) return;
    for (; w && *w && i + 1 < cap; ++w) {
        WCHAR c = *w;
        dst[i++] = (c >= 0x20 && c < 0x7F) ? (char)c : '_';
    }
    dst[i] = 0;
}

static unsigned g_seq = 0;

/* 写请求。action: start / stop / status / capability */
static int backend_req(const char *action)
{
    CreateDirectoryA(SHIM_COMM_DIR, NULL);   /* 已存在就失败，无所谓 */
    FILE *f = fopen(SHIM_REQ_PATH, "wb");
    if (!f) {
        LOG("[shim] !! 写不了 %s（errno=%d）—— shimd 装了吗？", SHIM_REQ_PATH, errno);
        return 0;
    }
    ++g_seq;
    fprintf(f, "{\"seq\":%u,\"action\":\"%s\",\"ssid\":\"%s\",\"key\":\"%s\"}\n",
            g_seq, action, g_ssid, g_key);
    fclose(f);
    LOG("[shim] -> req seq=%u action=%s ssid=\"%s\" keyLen=%zu",
        g_seq, action, g_ssid, strlen(g_key));
    return 1;
}

/* 从 status.json 里抠出我们要的字段。是个小程序内部协议，不值得上 JSON 解析器。 */
static void json_str(const char *buf, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    char pat[64];
    _snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return;
    p = strchr(p + strlen(pat), ':');
    if (!p) return;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '"') return;
    ++p;
    size_t i = 0;
    for (; *p && *p != '"' && i + 1 < cap; ++p)
        out[i++] = (*p == '\\' && p[1]) ? *++p : *p;
    out[i] = 0;
}

static int json_bool(const char *buf, const char *key)
{
    char pat[64];
    _snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (!strncmp(p, "true", 4))  return 1;
    if (!strncmp(p, "false", 5)) return 0;
    return -1;
}

/* 读后端状态。返回 1 表示读到了。 */
static int backend_status(char *state, size_t scap, char *detail, size_t dcap, int *ok)
{
    if (state && scap)  state[0] = 0;
    if (detail && dcap) detail[0] = 0;
    if (ok) *ok = -1;

    FILE *f = fopen(SHIM_STS_PATH, "rb");
    if (!f) return 0;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;

    if (state)  json_str(buf, "state",  state,  scap);
    if (detail) json_str(buf, "detail", detail, dcap);
    if (ok)     *ok = json_bool(buf, "ok");
    return 1;
}

/* 等后端把这次请求的结果写回来（最多 ms 毫秒）。
 * shimd 是轮询的，0.5s 一次，所以给 3 秒余量。 */
static int backend_wait(char *state, size_t scap, char *detail, size_t dcap, int *ok, int ms)
{
    unsigned want = g_seq;
    for (int waited = 0; waited <= ms; waited += 200) {
        if (backend_status(state, scap, detail, dcap, ok)) {
            /* 简单起见：只要能读到就算这轮结果（shimd 每次都会覆盖写） */
            return 1;
        }
        Sleep(200);
    }
    if (state && scap)  strcpy(state, "unavailable");
    if (detail && dcap) strcpy(detail, "后端没响应（wlanapi-shimd 没在跑？）");
    if (ok) *ok = 0;
    LOG("[shim] !! 等后端超时（seq=%u, %d ms）", want, ms);
    return 0;
}

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
    /* 问后端网卡到底支不支持开热点。支持就报成功，让上层继续往下走。 */
    char state[64], detail[256];
    int ok = -1;
    backend_req("capability");
    backend_wait(state, sizeof(state), detail, sizeof(detail), &ok, 3000);

    if (ok == 1) {
        LOG("[shim] WlanHostedNetworkInitSettings() -> OK (%s)", detail);
        if (pFailReason) *pFailReason = wlan_hosted_network_reason_success;
    } else {
        LOG("[shim] WlanHostedNetworkInitSettings() -> 后端说不行: %s", detail);
        if (pFailReason) *pFailReason = wlan_hosted_network_reason_unsupported;
    }
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanHostedNetworkQueryStatus(HANDLE hClientHandle,
                                          PWLAN_HOSTED_NETWORK_STATUS *ppWlanHostedNetworkStatus,
                                          PVOID pReserved)
{
    if (!ppWlanHostedNetworkStatus) return ERROR_INVALID_PARAMETER;
    PWLAN_HOSTED_NETWORK_STATUS s = shim_alloc(sizeof(WLAN_HOSTED_NETWORK_STATUS));
    if (!s) return ERROR_NOT_ENOUGH_MEMORY;

    /* 状态以后端为准 —— 别撒谎，上层靠它判断链路是否真的建立了 */
    char state[64] = {0}, detail[256] = {0};
    int ok = -1;
    backend_status(state, sizeof(state), detail, sizeof(detail), &ok);

    if (!strcmp(state, "active"))
        s->wlanHostedNetworkState = wlan_hosted_network_active;
    else if (!strcmp(state, "idle"))
        s->wlanHostedNetworkState = wlan_hosted_network_idle;
    else
        s->wlanHostedNetworkState = wlan_hosted_network_unavailable;

    s->dwNumberOfPeers = 0;   /* TODO: 从 hostapd 的客户端列表里数真实上来的手机 */
    *ppWlanHostedNetworkStatus = s;
    LOG("[shim] WlanHostedNetworkQueryStatus() -> %s (%s)", state[0] ? state : "无后端", detail);
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
    /* ★ 这里拿到手机要连的热点名（SSID）—— 整个链路的关键参数之一 */
    if (OpCode == wlan_hosted_network_opcode_connection_settings
        && pvData && dwDataSize >= sizeof(WLAN_HOSTED_NETWORK_CONNECTION_SETTINGS)) {
        const WLAN_HOSTED_NETWORK_CONNECTION_SETTINGS *cs = pvData;
        ULONG n = cs->dot11Ssid.uSSIDLength;
        if (n > sizeof(cs->dot11Ssid.ucSSID)) n = sizeof(cs->dot11Ssid.ucSSID);

        char raw[40] = {0};
        memcpy(raw, cs->dot11Ssid.ucSSID, n);
        sanitize_ascii(raw, g_ssid, sizeof(g_ssid));
        g_have_ssid = (g_ssid[0] != 0);

        LOG("[shim] WlanHostedNetworkSetProperty(connection_settings): SSID=\"%s\" (%lu 字节) "
            "maxPeers=%lu", g_ssid, (unsigned long)n,
            (unsigned long)cs->dwMaxNumberOfPeers);
    } else {
        LOG("[shim] WlanHostedNetworkSetProperty(opcode=%d, %lu 字节)",
            (int)OpCode, (unsigned long)dwDataSize);
    }
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
    /* ★ 这里拿到热点口令 —— 另一个关键参数。攒着，ForceStart 时一起发给后端。 */
    if (pucKeyData && dwKeyLength) {
        char raw[256] = {0};
        DWORD n = dwKeyLength < sizeof(raw) - 1 ? dwKeyLength : (DWORD)(sizeof(raw) - 1);
        memcpy(raw, pucKeyData, n);
        if (bIsPassPhrase) sanitize_ascii(raw, g_key, sizeof(g_key));
        else               wide_to_ascii((const WCHAR *)raw, g_key, sizeof(g_key));
        g_have_key = (g_key[0] != 0);
        LOG("[shim] WlanHostedNetworkSetSecondaryKey: 收到口令 %lu 字节 (isPassPhrase=%d) -> 已存",
            (unsigned long)dwKeyLength, (int)bIsPassPhrase);
    }
    if (pFailReason) *pFailReason = wlan_hosted_network_reason_success;
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanHostedNetworkForceStart(HANDLE hClientHandle,
                                         PWLAN_HOSTED_NETWORK_REASON pFailReason, PVOID pReserved)
{
    /* ★ 真正的动作在这里：让 Linux 侧起 hostapd + 配 IP */
    LOG("[shim] WlanHostedNetworkForceStart(): ssid=\"%s\" keySet=%d",
        g_ssid, g_have_key);

    if (!g_have_ssid) strcpy(g_ssid, "HUAWEI-PC");   /* 上层没给就兜个默认 */
    if (!g_have_key) {
        LOG("[shim] !! 没收到口令，后端会拒绝");
    }

    char state[64], detail[256];
    int ok = -1;
    backend_req("start");
    backend_wait(state, sizeof(state), detail, sizeof(detail), &ok, 6000);

    if (ok == 1 && !strcmp(state, "active")) {
        if (pFailReason) *pFailReason = wlan_hosted_network_reason_success;
        LOG("[shim] ForceStart -> 成功: %s", detail);
        return ERROR_SUCCESS;
    }

    if (pFailReason) *pFailReason = wlan_hosted_network_reason_ap_start_failed;
    LOG("[shim] ForceStart -> 失败: %s", detail);
    return ERROR_NOT_SUPPORTED;   /* 如实报失败，别骗上层 */
}

DWORD WINAPI WlanHostedNetworkForceStop(HANDLE hClientHandle,
                                        PWLAN_HOSTED_NETWORK_REASON pFailReason, PVOID pReserved)
{
    char state[64], detail[256];
    int ok = -1;
    backend_req("stop");
    backend_wait(state, sizeof(state), detail, sizeof(detail), &ok, 5000);
    LOG("[shim] WlanHostedNetworkForceStop() -> %s (%s)", state, detail);
    if (pFailReason) *pFailReason = wlan_hosted_network_reason_success;
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanHostedNetworkStopUsing(HANDLE hClientHandle, PVOID pReserved)
{
    backend_req("stop");
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
