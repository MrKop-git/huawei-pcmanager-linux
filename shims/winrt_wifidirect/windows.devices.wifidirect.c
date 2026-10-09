/*
 * Windows.Devices.WiFiDirect —— WinRT 类的 Wine 垫片
 *
 * 为什么需要
 * ----------
 * 电脑管家的 HiConnectivityService.exe 用 WinRT 让手机能发现这台电脑：
 *
 *     Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher   （BLE 广播）
 *     Windows.Devices.WiFiDirect.WiFiDirectAdvertisementPublisher               （Wi-Fi Direct 广播）
 *
 * Wine 有前者（windows.devices.bluetooth.dll），**没有后者**（根本没有
 * windows.devices.wifidirect）。combase 报 `semi-stub` → RoGetActivationFactory 失败
 * → 上层拿到空工厂 → 解引用 -1 → 0xC0000005 崩溃。
 *
 * 垫片机制（不需要改 Wine）
 * -------------------------
 * Wine 用注册表解析 WinRT 类：
 *
 *     HKLM\Software\Microsoft\WindowsRuntime\ActivatableClassId\<类名>
 *         DllPath = <我们的 dll 路径>
 *
 * combase 会加载该 DLL 并调它的 `DllGetActivationFactory`。
 * 见同目录 install.sh。
 *
 * 调用链与 Linux 后端的对应
 * -------------------------
 *     激活 Publisher
 *       → get_Advertisement → get_LegacySettings
 *       → put_Ssid / put_Passphrase        ← ★ 热点名与口令的真正来源
 *       → Start()                          ← ★ 起热点 → 转发给 wlanapi-shimd
 *
 * 接口定义来源
 * ------------
 * GUID 与方法顺序取自 Windows SDK 的 `windows.devices.wifidirect.h`：
 *
 *   IWiFiDirectAdvertisementPublisher {B35A2D1A-9B1F-45D9-925A-694D66DF68EF}
 *       get_Advertisement get_Status add_StatusChanged remove_StatusChanged Start Stop
 *   IWiFiDirectAdvertisement          {AB511A2D-2A06-49A1-A584-61435C7905A6}
 *       get_/put_InformationElements  get_/put_ListenStateDiscoverability
 *       get_/put_IsAutonomousGroupOwnerEnabled  get_LegacySettings
 *   IWiFiDirectLegacySettings         {A64FDBBA-F2FD-4567-A91B-F5C2F5321057}
 *       get_/put_IsEnabled  get_/put_Ssid  get_/put_Passphrase
 *
 * ★ vtable 顺序绝不能改 —— 顺序错了不是编译错误，是运行期静默调错函数。
 */

#include <windows.h>
#include <inspectable.h>
#include <hstring.h>
#include <winstring.h>      /* WindowsCreateString / WindowsGetStringRawBuffer 的 C 声明在这 */
#include <roapi.h>
#include <activation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* WinRT 的事件注册 token（{ __int64 value; }）。
   mingw 的 eventtoken.h 不一定在，自己兜一个。 */
#ifndef __eventtoken_h__
#ifndef __EVENTTOKEN_H__
typedef struct EventRegistrationToken { __int64 value; } EventRegistrationToken;
#endif
#endif

/* ------------------------------------------------------------------ IID */

static const GUID kIID_IActivationFactory = {
    0x00000035, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID kIID_IWifiDirectAdvertisementPublisher = {
    0xB35A2D1A, 0x9B1F, 0x45D9, { 0x92, 0x5A, 0x69, 0x4D, 0x66, 0xDF, 0x68, 0xEF } };
static const GUID kIID_IWifiDirectAdvertisement = {
    0xAB511A2D, 0x2A06, 0x49A1, { 0xA5, 0x84, 0x61, 0x43, 0x5C, 0x79, 0x05, 0xA6 } };
static const GUID kIID_IWifiDirectLegacySettings = {
    0xA64FDBBA, 0xF2FD, 0x4567, { 0xA9, 0x1B, 0xF5, 0xC2, 0xF5, 0x32, 0x10, 0x57 } };
static const GUID kIID_IWifiDirectConnectionListener = {
    0x699C1B0D, 0x8D13, 0x4EE9, { 0xB9, 0xEC, 0x9C, 0x72, 0xF8, 0x25, 0x1F, 0x7D } };
/* 备用（将来要真回调"有设备请求连接"时会用到）：
 *   IWiFiDirectConnectionRequestedEventArgs = {F99D20BE-D38D-484F-8215-E7B65ABF244C} */

/* 本垫片认识的两个 runtime class */
static const WCHAR kClassNameConnListener[] =
    L"Windows.Devices.WiFiDirect.WiFiDirectConnectionListener";

static const WCHAR kClassName[] =
    L"Windows.Devices.WiFiDirect.WiFiDirectAdvertisementPublisher";

/* ----------------------------------------------------------------- 日志 */

static FILE *g_log;

static void lg(const char *fmt, ...)
{
    if (!g_log) {
        const char *p = getenv("WIFIDIRECT_SHIM_LOG");
        g_log = fopen(p && *p ? p : "C:\\wifidirect_shim.log", "a");
        if (!g_log) return;
    }
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

/* ------------------------------------------------- 后端桥（复用文件协议） */

#define COMM_DIR  "C:\\wlanapi_shim"
#define REQ_PATH  COMM_DIR "\\req.json"
#define STS_PATH  COMM_DIR "\\status.json"

static char g_ssid[64],  g_pass[128];
static unsigned g_seq;

static void wide_to_ascii(const WCHAR *w, char *dst, size_t cap)
{
    size_t i = 0;
    if (!cap) return;
    for (; w && *w && i + 1 < cap; ++w)
        dst[i++] = (*w >= 0x20 && *w < 0x7F) ? (char)*w : '_';
    dst[i] = 0;
}

/* 把动作发给 wlanapi-shimd（复用已有的文件协议） */
static int backend(const char *action)
{
    CreateDirectoryA(COMM_DIR, NULL);
    FILE *f = fopen(REQ_PATH, "wb");
    if (!f) { lg("[wifidirect] !! 写不了 %s", REQ_PATH); return 0; }
    fprintf(f, "{\"seq\":%u,\"action\":\"%s\",\"ssid\":\"%s\",\"key\":\"%s\"}\n",
            ++g_seq, action, g_ssid, g_pass);
    fclose(f);
    lg("[wifidirect] -> req seq=%u action=%s ssid=\"%s\" keyLen=%zu",
       g_seq, action, g_ssid, strlen(g_pass));
    return 1;
}

static int wait_state(char *state, size_t cap)
{
    for (int i = 0; i <= 25; ++i) {         /* 最多 5 秒；shimd 轮询 0.5s */
        FILE *f = fopen(STS_PATH, "rb");
        if (f) {
            char buf[1024]; size_t n = fread(buf, 1, sizeof(buf)-1, f); fclose(f);
            buf[n] = 0;
            const char *p = strstr(buf, "\"state\"");
            if (p) {
                p = strchr(p + 7, ':'); if (p) {
                    ++p; while (*p==' '||*p=='\t') ++p;
                    if (*p == '"') { ++p; size_t j=0;
                        while (*p && *p!='"' && j+1<cap) state[j++]=*p++;
                        state[j]=0; return 1; } } }
        }
        Sleep(200);
    }
    if (cap) strcpy(state, "unavailable");
    return 0;
}

/* ------------------------------------------------------------ 通用样板 */

/* WinRT 对象的公共头：vtable 指针 + 引用计数 */
#define RT_HEAD const void *lpVtbl; LONG ref;

/* ★ 坑（踩过）：不要用 ((LONG*)self)[1] 取 ref —— x64 上 lpVtbl 占 8 字节，
 *   ref 在偏移 8 而不是 4。那样等于每次 AddRef/Release 都在改 lpVtbl 的高 4 字节，
 *   虚表指针被写坏，之后一跳就是 page fault（表现为"崩在 combase 里"）。
 *   老老实实用结构体取字段。 */
typedef struct RTHead { const void *lpVtbl; LONG ref; } RTHead;

static ULONG rt_AddRef(void *self) { return (ULONG)InterlockedIncrement(&((RTHead*)self)->ref); }
static ULONG rt_Release(void *self, void (*dtor)(void*))
{
    LONG n = InterlockedDecrement(&((RTHead*)self)->ref);
    if (n == 0 && dtor) dtor(self);
    return (ULONG)n;
}

/* 凡是 QI IInspectable/IUnknown 一律成功；具体接口按各自实现判断 */
static HRESULT rt_QI(const GUID *iid, const GUID *self_iid, void *self, void **out)
{
    if (!out) return E_POINTER;
    static const GUID iid_unk = {0,0,0,{0xC0,0,0,0,0,0,0,0x46}};
    static const GUID iid_ins = {0xAF86E2E0,0xB12D,0x4C6A,{0x9C,0x5A,0xD7,0xAA,0x65,0x10,0x1E,0x90}};
    if (IsEqualGUID(iid, &iid_unk) || IsEqualGUID(iid, &iid_ins) || IsEqualGUID(iid, self_iid)) {
        rt_AddRef(self); *out = self; return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static HRESULT rt_GetIids(const GUID *self_iid, ULONG *n, IID **iids)
{
    IID *a = (IID*)CoTaskMemAlloc(sizeof(IID));
    if (!a) return E_OUTOFMEMORY;
    memcpy(a, self_iid, sizeof(IID));
    *n = 1; *iids = a;
    return S_OK;
}

static HRESULT rt_GetRuntimeClassName(HSTRING *name)
{
    return WindowsCreateString(kClassName, (UINT32)wcslen(kClassName), name);
}

/* 事件注册 token（我们不真的回调，但要给个稳定的非零值） */
static HRESULT rt_token(EventRegistrationToken *t) { if (t) t->value = 1; return t ? S_OK : E_POINTER; }

/* =================================================================
 * IWiFiDirectLegacySettings  {A64FDBBA-...}
 *   get_/put_IsEnabled  get_/put_Ssid  get_/put_Passphrase
 * ★ Ssid / Passphrase 就是热点名与口令 —— 上层写这里，我们存下来
 * ================================================================= */

typedef struct LegacySettings {
    RT_HEAD
    BOOL   enabled;
    WCHAR  ssid[64];
    WCHAR  pass[128];
} LegacySettings;

typedef struct LegacySettingsVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(LegacySettings*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(LegacySettings*);
    ULONG   (STDMETHODCALLTYPE *Release)(LegacySettings*);
    HRESULT (STDMETHODCALLTYPE *GetIids)(LegacySettings*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(LegacySettings*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(LegacySettings*, TrustLevel*);
    /* 以下顺序来自 SDK 头，勿改 */
    HRESULT (STDMETHODCALLTYPE *get_IsEnabled)(LegacySettings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_IsEnabled)(LegacySettings*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_Ssid)(LegacySettings*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE *put_Ssid)(LegacySettings*, HSTRING);
    HRESULT (STDMETHODCALLTYPE *get_Passphrase)(LegacySettings*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE *put_Passphrase)(LegacySettings*, HSTRING);
} LegacySettingsVtbl;

static HRESULT LS_QI(LegacySettings *s, REFIID iid, void **o)
{ return rt_QI(iid, &kIID_IWifiDirectLegacySettings, s, o); }
static ULONG LS_AddRef(LegacySettings *s) { return rt_AddRef(s); }
static void  LS_dtor(void *p) { free(p); }
static ULONG LS_Release(LegacySettings *s) { return rt_Release(s, LS_dtor); }
static HRESULT LS_GetIids(LegacySettings *s, ULONG *n, IID **i) { (void)s; return rt_GetIids(&kIID_IWifiDirectLegacySettings, n, i); }
static HRESULT LS_GetRCN(LegacySettings *s, HSTRING *h) { (void)s; return rt_GetRuntimeClassName(h); }
static HRESULT LS_GetTL(LegacySettings *s, TrustLevel *t) { (void)s; if(t)*t=BaseTrust; return S_OK; }

static HRESULT LS_get_IsEnabled(LegacySettings *s, BOOL *v) { if(!v) return E_POINTER; *v = s->enabled; return S_OK; }
static HRESULT LS_put_IsEnabled(LegacySettings *s, BOOL v) { s->enabled = v; lg("[wifidirect] LegacySettings.IsEnabled = %d", v); return S_OK; }

static HRESULT LS_get_Ssid(LegacySettings *s, HSTRING *v)
{ if(!v) return E_POINTER; return WindowsCreateString(s->ssid, (UINT32)wcslen(s->ssid), v); }
static HRESULT LS_put_Ssid(LegacySettings *s, HSTRING v)
{
    UINT32 n=0; const WCHAR *r = v ? WindowsGetStringRawBuffer(v,&n) : NULL;
    if (r) { wcsncpy(s->ssid, r, 63); s->ssid[63]=0; } else s->ssid[0]=0;
    wide_to_ascii(s->ssid, g_ssid, sizeof(g_ssid));
    lg("[wifidirect] ★ LegacySettings.Ssid = \"%s\"", g_ssid);
    return S_OK;
}
static HRESULT LS_get_Passphrase(LegacySettings *s, HSTRING *v)
{ if(!v) return E_POINTER; return WindowsCreateString(s->pass, (UINT32)wcslen(s->pass), v); }
static HRESULT LS_put_Passphrase(LegacySettings *s, HSTRING v)
{
    UINT32 n=0; const WCHAR *r = v ? WindowsGetStringRawBuffer(v,&n) : NULL;
    if (r) { wcsncpy(s->pass, r, 127); s->pass[127]=0; } else s->pass[0]=0;
    wide_to_ascii(s->pass, g_pass, sizeof(g_pass));
    lg("[wifidirect] ★ LegacySettings.Passphrase 已收到（%zu 字符）", strlen(g_pass));
    return S_OK;
}

static const LegacySettingsVtbl kLSVtbl = {
    LS_QI, LS_AddRef, LS_Release, LS_GetIids, LS_GetRCN, LS_GetTL,
    LS_get_IsEnabled, LS_put_IsEnabled,
    LS_get_Ssid,      LS_put_Ssid,
    LS_get_Passphrase, LS_put_Passphrase,
};

static LegacySettings *LegacySettings_new(void)
{
    LegacySettings *s = (LegacySettings*)calloc(1, sizeof(*s));
    if (s) { s->lpVtbl = &kLSVtbl; s->ref = 1; s->enabled = TRUE;
             wcscpy(s->ssid, L"HUAWEI-PC"); }
    return s;
}

/* =================================================================
 * IWiFiDirectAdvertisement  {AB511A2D-...}
 *   get_/put_InformationElements
 *   get_/put_ListenStateDiscoverability
 *   get_/put_IsAutonomousGroupOwnerEnabled
 *   get_LegacySettings
 * ================================================================= */

typedef struct Advertisement {
    RT_HEAD
    LegacySettings *legacy;
    INT32 discoverability;
    BOOL  autonomousGO;
} Advertisement;

typedef struct AdvertisementVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Advertisement*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(Advertisement*);
    ULONG   (STDMETHODCALLTYPE *Release)(Advertisement*);
    HRESULT (STDMETHODCALLTYPE *GetIids)(Advertisement*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(Advertisement*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(Advertisement*, TrustLevel*);
    HRESULT (STDMETHODCALLTYPE *get_InformationElements)(Advertisement*, void**);
    HRESULT (STDMETHODCALLTYPE *put_InformationElements)(Advertisement*, void*);
    HRESULT (STDMETHODCALLTYPE *get_ListenStateDiscoverability)(Advertisement*, INT32*);
    HRESULT (STDMETHODCALLTYPE *put_ListenStateDiscoverability)(Advertisement*, INT32);
    HRESULT (STDMETHODCALLTYPE *get_IsAutonomousGroupOwnerEnabled)(Advertisement*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_IsAutonomousGroupOwnerEnabled)(Advertisement*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_LegacySettings)(Advertisement*, void**);
} AdvertisementVtbl;

static HRESULT AD_QI(Advertisement *s, REFIID iid, void **o)
{ return rt_QI(iid, &kIID_IWifiDirectAdvertisement, s, o); }
static ULONG AD_AddRef(Advertisement *s) { return rt_AddRef(s); }
static void  AD_dtor(void *p) { Advertisement *a=(Advertisement*)p; if(a->legacy) LS_Release(a->legacy); free(a); }
static ULONG AD_Release(Advertisement *s) { return rt_Release(s, AD_dtor); }
static HRESULT AD_GetIids(Advertisement *s, ULONG *n, IID **i) { (void)s; return rt_GetIids(&kIID_IWifiDirectAdvertisement, n, i); }
static HRESULT AD_GetRCN(Advertisement *s, HSTRING *h) { (void)s; return rt_GetRuntimeClassName(h); }
static HRESULT AD_GetTL(Advertisement *s, TrustLevel *t) { (void)s; if(t)*t=BaseTrust; return S_OK; }

static HRESULT AD_get_IE(Advertisement *s, void **v) { (void)s; if(v)*v=NULL; return S_OK; }   /* 空集合即可 */
static HRESULT AD_put_IE(Advertisement *s, void *v) { (void)s;(void)v; return S_OK; }
static HRESULT AD_get_LSD(Advertisement *s, INT32 *v) { if(!v) return E_POINTER; *v = s->discoverability; return S_OK; }
static HRESULT AD_put_LSD(Advertisement *s, INT32 v) { s->discoverability = v; lg("[wifidirect] ListenStateDiscoverability = %d", v); return S_OK; }
static HRESULT AD_get_GO(Advertisement *s, BOOL *v) { if(!v) return E_POINTER; *v = s->autonomousGO; return S_OK; }
static HRESULT AD_put_GO(Advertisement *s, BOOL v) { s->autonomousGO = v; lg("[wifidirect] IsAutonomousGroupOwnerEnabled = %d", v); return S_OK; }
static HRESULT AD_get_LS(Advertisement *s, void **v)
{
    if(!v) return E_POINTER;
    LS_AddRef(s->legacy);      /* 交出去一份引用 */
    *v = s->legacy;
    return S_OK;
}

static const AdvertisementVtbl kADVtbl = {
    AD_QI, AD_AddRef, AD_Release, AD_GetIids, AD_GetRCN, AD_GetTL,
    AD_get_IE, AD_put_IE,
    AD_get_LSD, AD_put_LSD,
    AD_get_GO, AD_put_GO,
    AD_get_LS,
};

static Advertisement *Advertisement_new(void)
{
    Advertisement *a = (Advertisement*)calloc(1, sizeof(*a));
    if (a) { a->lpVtbl=&kADVtbl; a->ref=1; a->legacy=LegacySettings_new();
             a->discoverability=1; a->autonomousGO=TRUE; }
    return a;
}

/* =================================================================
 * IWiFiDirectAdvertisementPublisher  {B35A2D1A-...}
 *   get_Advertisement  get_Status  add_/remove_StatusChanged  Start  Stop
 *
 * 这个对象同时充当"激活工厂"：ActivateInstance 返回它自己的一份引用
 * （WinRT 的 runtime class 就是这么做的）
 * ================================================================= */

typedef struct Publisher {
    RT_HEAD
    Advertisement *adv;
    INT32 status;                 /* WiFiDirectAdvertisementPublisherStatus */
    BOOL  started;
} Publisher;

typedef struct PublisherVtbl {
    /* IInspectable（6 槽） */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Publisher*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(Publisher*);
    ULONG   (STDMETHODCALLTYPE *Release)(Publisher*);
    HRESULT (STDMETHODCALLTYPE *GetIids)(Publisher*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(Publisher*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(Publisher*, TrustLevel*);
    /* ★ 紧接着就是 IWiFiDirectAdvertisementPublisher 的 6 个方法。
     *   中间**不许**再插任何东西 —— 工厂是另一个对象（见下面的 Factory）。
     *   （踩过：原先把 ActivateInstance 插在这里，导致整个接口错一位。） */
    HRESULT (STDMETHODCALLTYPE *get_Advertisement)(Publisher*, void**);
    HRESULT (STDMETHODCALLTYPE *get_Status)(Publisher*, INT32*);
    HRESULT (STDMETHODCALLTYPE *add_StatusChanged)(Publisher*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_StatusChanged)(Publisher*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *Start)(Publisher*);
    HRESULT (STDMETHODCALLTYPE *Stop)(Publisher*);
} PublisherVtbl;

static ULONG PB_AddRef(Publisher *s);       /* PB_QI 里要用，先声明 */

static HRESULT PB_QI(Publisher *s, REFIID iid, void **o)
{
    if (!o) return E_POINTER;
    /* 只认自己的接口；IActivationFactory 归工厂对象管，不在这里认领 */
    if (IsEqualGUID(iid, &kIID_IWifiDirectAdvertisementPublisher)) {
        PB_AddRef(s); *o = s; return S_OK;
    }
    return rt_QI(iid, &kIID_IWifiDirectAdvertisementPublisher, s, o);
}
static ULONG PB_AddRef(Publisher *s) { return rt_AddRef(s); }
static void  PB_dtor(void *p) { Publisher *s=(Publisher*)p; if(s->adv) AD_Release(s->adv); free(s); }
static ULONG PB_Release(Publisher *s) { return rt_Release(s, PB_dtor); }
static HRESULT PB_GetIids(Publisher *s, ULONG *n, IID **i) { (void)s; return rt_GetIids(&kIID_IWifiDirectAdvertisementPublisher, n, i); }
static HRESULT PB_GetRCN(Publisher *s, HSTRING *h) { (void)s; return rt_GetRuntimeClassName(h); }
static HRESULT PB_GetTL(Publisher *s, TrustLevel *t) { (void)s; if(t)*t=BaseTrust; return S_OK; }

static HRESULT PB_get_Advertisement(Publisher *s, void **v)
{
    if (!v) return E_POINTER;
    AD_AddRef(s->adv);
    *v = s->adv;
    return S_OK;
}
static HRESULT PB_get_Status(Publisher *s, INT32 *v)
{ if(!v) return E_POINTER; *v = s->status; return S_OK; }
static HRESULT PB_add_StatusChanged(Publisher *s, void *h, EventRegistrationToken *t)
{ (void)s;(void)h; lg("[wifidirect] add_StatusChanged"); return rt_token(t); }
static HRESULT PB_remove_StatusChanged(Publisher *s, EventRegistrationToken t)
{ (void)s;(void)t; return S_OK; }

/* ★ 起热点：转给 Linux 后端 */
static HRESULT PB_Start(Publisher *s)
{
    lg("[wifidirect] ★★★ Start()  ssid=\"%s\" keyLen=%zu", g_ssid, strlen(g_pass));
    if (!g_pass[0]) lg("[wifidirect] !! 还没收到口令，后端会拒绝");

    s->status = 1;                       /* Created */
    backend("start");
    char st[64]; wait_state(st, sizeof(st));
    lg("[wifidirect] Start() -> 后端状态 \"%s\"", st);
    if (!strcmp(st, "active")) { s->status = 2; s->started = TRUE; }  /* Started */
    return S_OK;
}

static HRESULT PB_Stop(Publisher *s)
{
    lg("[wifidirect] Stop()");
    backend("stop");
    s->status = 4;                       /* Aborted */
    s->started = FALSE;
    return S_OK;
}

static const PublisherVtbl kPBVtbl = {
    PB_QI, PB_AddRef, PB_Release, PB_GetIids, PB_GetRCN, PB_GetTL,
    PB_get_Advertisement, PB_get_Status,
    PB_add_StatusChanged, PB_remove_StatusChanged,
    PB_Start, PB_Stop,
};

static Publisher *Publisher_new(void)
{
    Publisher *s = (Publisher*)calloc(1, sizeof(*s));
    if (s) { s->lpVtbl = &kPBVtbl; s->ref = 1; s->adv = Advertisement_new(); s->status = 0; }
    return s;
}

/* =================================================================
 * IWiFiDirectConnectionListener  {699C1B0D-...}
 *   add_ConnectionRequested  remove_ConnectionRequested
 *
 * 程序用它监听"手机来连了"。我们注册回调但**不会真的回调** ——
 * 真回调要等后面把"有设备接入"从 Linux 侧（hostapd 的客户端列表 / BlueZ）接上来。
 * ================================================================= */

typedef struct ConnListener {
    RT_HEAD
    int dummy;
} ConnListener;

typedef struct ConnListenerVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ConnListener*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ConnListener*);
    ULONG   (STDMETHODCALLTYPE *Release)(ConnListener*);
    HRESULT (STDMETHODCALLTYPE *GetIids)(ConnListener*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(ConnListener*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(ConnListener*, TrustLevel*);
    HRESULT (STDMETHODCALLTYPE *add_ConnectionRequested)(ConnListener*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_ConnectionRequested)(ConnListener*, EventRegistrationToken);
} ConnListenerVtbl;

static HRESULT CL_QI(ConnListener *s, REFIID iid, void **o)
{
    if (!o) return E_POINTER;
    if (IsEqualGUID(iid, &kIID_IWifiDirectConnectionListener)) { rt_AddRef(s); *o = s; return S_OK; }
    return rt_QI(iid, &kIID_IWifiDirectConnectionListener, s, o);
}
static ULONG CL_AddRef(ConnListener *s) { return rt_AddRef(s); }
static ULONG CL_Release(ConnListener *s) { return rt_Release(s, free); }
static HRESULT CL_GetIids(ConnListener *s, ULONG *n, IID **i)
{ (void)s; return rt_GetIids(&kIID_IWifiDirectConnectionListener, n, i); }
static HRESULT CL_GetRCN(ConnListener *s, HSTRING *h)
{ (void)s; return h ? WindowsCreateString(kClassNameConnListener,
                                          (UINT32)wcslen(kClassNameConnListener), h) : E_POINTER; }
static HRESULT CL_GetTL(ConnListener *s, TrustLevel *t) { (void)s; if(t)*t=BaseTrust; return S_OK; }

static HRESULT CL_add_CR(ConnListener *s, void *h, EventRegistrationToken *t)
{ (void)s;(void)h; lg("[wifidirect] ConnectionListener::add_ConnectionRequested（已记下，暂不回调）"); return rt_token(t); }
static HRESULT CL_remove_CR(ConnListener *s, EventRegistrationToken t) { (void)s;(void)t; return S_OK; }

static const ConnListenerVtbl kCLVtbl = {
    CL_QI, CL_AddRef, CL_Release, CL_GetIids, CL_GetRCN, CL_GetTL,
    CL_add_CR, CL_remove_CR,
};

/* =================================================================
 * 激活工厂（IActivationFactory）
 *
 * ★ 工厂和实例是两个对象：工厂只有 IInspectable + ActivateInstance，
 *   实例才带具体接口的方法。混在一个对象里会让实例的接口整体错位。
 * ================================================================= */

/* =================================================================
 * 通用兜底对象
 *
 * 程序引用的 WinRT 类比 Wine 实现的多得多（光 HiConnectivityService 就还缺
 * 9 个）。一个个实现既慢又没必要 —— 其中有些在本次执行的路径上根本不会被激活。
 *
 * 兜底的语义：**类找得到、实例给得出，但具体接口一律 E_NOINTERFACE**。
 * 这样上层走它自己的错误分支（大多数 WinRT 代码都会查 HRESULT），
 * 而不是整条链断在 combase 的 "Failed to find library"。
 *
 * 这正是"先用运行期日志把真正要紧的挑出来"的手段 —— 谁真的被 QI 了，
 * 日志里看得见，再去把它实现完整。
 * ================================================================= */

typedef struct StubObj { RT_HEAD } StubObj;

typedef struct StubVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(StubObj*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(StubObj*);
    ULONG   (STDMETHODCALLTYPE *Release)(StubObj*);
    HRESULT (STDMETHODCALLTYPE *GetIids)(StubObj*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(StubObj*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(StubObj*, TrustLevel*);
} StubVtbl;

static HRESULT ST_QI(StubObj *s, REFIID iid, void **o)
{
    if (!o) return E_POINTER;
    static const GUID iid_unk = {0,0,0,{0xC0,0,0,0,0,0,0,0x46}};
    static const GUID iid_ins = {0xAF86E2E0,0xB12D,0x4C6A,{0x9C,0x5A,0xD7,0xAA,0x65,0x10,0x1E,0x90}};
    if (IsEqualGUID(iid, &iid_unk) || IsEqualGUID(iid, &iid_ins)) { rt_AddRef(s); *o = s; return S_OK; }
    char nm[64]; snprintf(nm, sizeof(nm), "{%08lX-%04X-%04X-...}",
                          (unsigned long)iid->Data1, iid->Data2, iid->Data3);
    lg("[wifidirect] 兜底对象被 QI 一个未实现的接口 %s -> E_NOINTERFACE", nm);
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG ST_AddRef(StubObj *s) { return rt_AddRef(s); }
static ULONG ST_Release(StubObj *s) { return rt_Release(s, free); }
static HRESULT ST_GetIids(StubObj *s, ULONG *n, IID **i)
{ (void)s; if(n)*n=0; if(i)*i=NULL; return S_OK; }
static HRESULT ST_GetRCN(StubObj *s, HSTRING *h)
{ (void)s; if(!h) return E_POINTER; return WindowsCreateString(kClassName, (UINT32)wcslen(kClassName), h); }
static HRESULT ST_GetTL(StubObj *s, TrustLevel *t) { (void)s; if(t)*t=BaseTrust; return S_OK; }

static const StubVtbl kSTVtbl = {
    ST_QI, ST_AddRef, ST_Release, ST_GetIids, ST_GetRCN, ST_GetTL,
};

enum { KIND_PUBLISHER = 0, KIND_CONNLISTENER = 1, KIND_STUB = 2 };

typedef struct Factory {
    RT_HEAD
    int kind;
} Factory;

typedef struct FactoryVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Factory*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(Factory*);
    ULONG   (STDMETHODCALLTYPE *Release)(Factory*);
    HRESULT (STDMETHODCALLTYPE *GetIids)(Factory*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(Factory*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(Factory*, TrustLevel*);
    HRESULT (STDMETHODCALLTYPE *ActivateInstance)(Factory*, void**);
} FactoryVtbl;

static HRESULT FW_QI(Factory *s, REFIID iid, void **o)
{
    if (!o) return E_POINTER;
    if (IsEqualGUID(iid, &kIID_IActivationFactory)) { rt_AddRef(s); *o = s; return S_OK; }
    static const GUID iid_unk = {0,0,0,{0xC0,0,0,0,0,0,0,0x46}};
    static const GUID iid_ins = {0xAF86E2E0,0xB12D,0x4C6A,{0x9C,0x5A,0xD7,0xAA,0x65,0x10,0x1E,0x90}};
    if (IsEqualGUID(iid, &iid_unk) || IsEqualGUID(iid, &iid_ins)) { rt_AddRef(s); *o = s; return S_OK; }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG FW_AddRef(Factory *s) { return rt_AddRef(s); }
static ULONG FW_Release(Factory *s) { return rt_Release(s, free); }
static HRESULT FW_GetIids(Factory *s, ULONG *n, IID **i)
{ (void)s; return rt_GetIids(&kIID_IActivationFactory, n, i); }
static HRESULT FW_GetRCN(Factory *s, HSTRING *h) { (void)s; return rt_GetRuntimeClassName(h); }
static HRESULT FW_GetTL(Factory *s, TrustLevel *t) { (void)s; if(t)*t=BaseTrust; return S_OK; }

static HRESULT FW_ActivateInstance(Factory *s, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (s->kind == KIND_PUBLISHER) {
        Publisher *p = Publisher_new();
        if (!p) return E_OUTOFMEMORY;
        *out = p;
        lg("[wifidirect] ActivateInstance -> Publisher");
    } else if (s->kind == KIND_CONNLISTENER) {
        ConnListener *c = (ConnListener*)calloc(1, sizeof(*c));
        if (!c) return E_OUTOFMEMORY;
        c->lpVtbl = &kCLVtbl; c->ref = 1;
        *out = c;
        lg("[wifidirect] ActivateInstance -> ConnectionListener");
    } else {
        StubObj *o = (StubObj*)calloc(1, sizeof(*o));
        if (!o) return E_OUTOFMEMORY;
        o->lpVtbl = &kSTVtbl; o->ref = 1;
        *out = o;
        lg("[wifidirect] ActivateInstance -> 兜底对象（未实现该类的行为）");
    }
    return S_OK;
}

static const FactoryVtbl kFWVtbl = {
    FW_QI, FW_AddRef, FW_Release, FW_GetIids, FW_GetRCN, FW_GetTL,
    FW_ActivateInstance,
};

static Factory *Factory_new(int kind)
{
    Factory *f = (Factory*)calloc(1, sizeof(*f));
    if (f) { f->lpVtbl = &kFWVtbl; f->ref = 1; f->kind = kind; }
    return f;
}

/* =================================================================
 * 只给骨架、暂不实现行为的类
 *
 * 来源：tools/collect-gaps.sh 的运行期缺口 + 逐 exe 的 UTF-16 字符串扫
 * （**逐 exe 扫**才干净；扫整个目录会带进 SDK 元数据，实测噪声大两个量级）。
 *
 * 这份清单会随程序"走得更远"而增长 —— 每补一个类，它就露出下一层。
 * 加类只需在这里加一行 + 在 install.sh 的 CLASSES 里加一行。
 * ================================================================= */

/* =================================================================
 * 两个"带自己工厂接口"的类
 *
 * ★ 结构上和前面几个不同：
 *   - NetworkInformation 是**静态类**：工厂对象**不实现 IActivationFactory**，
 *     而是把 INetworkInformationStatics 的 8 个方法直接摆在槽 6 起。
 *     （给静态类问 IActivationFactory 本来就该是 E_NOINTERFACE。）
 *   - PasswordCredential 没有默认构造，工厂只实现 ICredentialFactory（槽 6）。
 *   所以这两个各要自己的工厂虚表，不能复用通用的 Factory。
 *
 * 语义上先给"最小可用"：
 *   - 网络状态类方法一律返回 S_OK + NULL（"没有可用连接"），
 *     上层多半会走降级分支；
 *   - PasswordCredential 是纯数据容器，正常实现（存账号密码用得上）。
 * ================================================================= */

static const GUID kIID_INetworkInformationStatics = {
    0x5074F851, 0x950D, 0x4165, { 0x9C, 0x15, 0x36, 0x56, 0x19, 0x48, 0x1E, 0xEA } };
static const GUID kIID_ICredentialFactory = {
    0x54EF13A1, 0xBF26, 0x47B5, { 0x97, 0xDD, 0xDE, 0x77, 0x9B, 0x7C, 0xAD, 0x58 } };
static const GUID kIID_IPasswordCredential = {
    0x6AB18989, 0xC720, 0x41A7, { 0xA6, 0xC1, 0xFE, 0xAD, 0xB3, 0x63, 0x29, 0xA0 } };

/* ---------------- PasswordCredential（纯数据容器）---------------- */

typedef struct PasswordCred {
    RT_HEAD
    WCHAR resource[256];
    WCHAR user[256];
    WCHAR pass[256];
} PasswordCred;

typedef struct PasswordCredVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(PasswordCred*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(PasswordCred*);
    ULONG   (STDMETHODCALLTYPE *Release)(PasswordCred*);
    HRESULT (STDMETHODCALLTYPE *GetIids)(PasswordCred*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(PasswordCred*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(PasswordCred*, TrustLevel*);
    /* 顺序照 SDK 头，勿改 */
    HRESULT (STDMETHODCALLTYPE *get_Resource)(PasswordCred*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE *put_Resource)(PasswordCred*, HSTRING);
    HRESULT (STDMETHODCALLTYPE *get_UserName)(PasswordCred*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE *put_UserName)(PasswordCred*, HSTRING);
    HRESULT (STDMETHODCALLTYPE *get_Password)(PasswordCred*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE *put_Password)(PasswordCred*, HSTRING);
    HRESULT (STDMETHODCALLTYPE *RetrievePassword)(PasswordCred*);
    HRESULT (STDMETHODCALLTYPE *get_Properties)(PasswordCred*, void**);
} PasswordCredVtbl;

static HRESULT PC_QI(PasswordCred *s, REFIID iid, void **o)
{
    if (!o) return E_POINTER;
    if (IsEqualGUID(iid, &kIID_IPasswordCredential)) { rt_AddRef(s); *o = s; return S_OK; }
    return rt_QI(iid, &kIID_IPasswordCredential, s, o);
}
static ULONG PC_AddRef(PasswordCred *s) { return rt_AddRef(s); }
static ULONG PC_Release(PasswordCred *s) { return rt_Release(s, free); }
static HRESULT PC_GetIids(PasswordCred *s, ULONG *n, IID **i)
{ (void)s; return rt_GetIids(&kIID_IPasswordCredential, n, i); }
static HRESULT PC_GetRCN(PasswordCred *s, HSTRING *h)
{ (void)s; static const WCHAR n[]=L"Windows.Security.Credentials.PasswordCredential";
  return h ? WindowsCreateString(n,(UINT32)wcslen(n),h) : E_POINTER; }
static HRESULT PC_GetTL(PasswordCred *s, TrustLevel *t) { (void)s; if(t)*t=BaseTrust; return S_OK; }

static HRESULT PC_hget(const WCHAR *v, HSTRING *out)
{ if(!out) return E_POINTER; return WindowsCreateString(v,(UINT32)wcslen(v),out); }
static void PC_hput(HSTRING v, WCHAR *dst, size_t cap)
{
    UINT32 n=0; const WCHAR *r = v ? WindowsGetStringRawBuffer(v,&n) : NULL;
    if (r) { wcsncpy(dst,r,cap-1); dst[cap-1]=0; } else dst[0]=0;
}
static HRESULT PC_get_Resource(PasswordCred *s, HSTRING *o) { return PC_hget(s->resource,o); }
static HRESULT PC_put_Resource(PasswordCred *s, HSTRING v) { PC_hput(v,s->resource,256); lg("[wifidirect] PasswordCredential.Resource 已设"); return S_OK; }
static HRESULT PC_get_UserName(PasswordCred *s, HSTRING *o) { return PC_hget(s->user,o); }
static HRESULT PC_put_UserName(PasswordCred *s, HSTRING v) { PC_hput(v,s->user,256); lg("[wifidirect] PasswordCredential.UserName 已设"); return S_OK; }
static HRESULT PC_get_Password(PasswordCred *s, HSTRING *o) { return PC_hget(s->pass,o); }
static HRESULT PC_put_Password(PasswordCred *s, HSTRING v) { PC_hput(v,s->pass,256); lg("[wifidirect] PasswordCredential.Password 已设(%zu 字)", wcslen(s->pass)); return S_OK; }
static HRESULT PC_RetrievePassword(PasswordCred *s) { (void)s; lg("[wifidirect] PasswordCredential.RetrievePassword"); return S_OK; }
static HRESULT PC_get_Properties(PasswordCred *s, void **o) { (void)s; if(o)*o=NULL; return S_OK; }

static const PasswordCredVtbl kPCVtbl = {
    PC_QI, PC_AddRef, PC_Release, PC_GetIids, PC_GetRCN, PC_GetTL,
    PC_get_Resource, PC_put_Resource,
    PC_get_UserName, PC_put_UserName,
    PC_get_Password, PC_put_Password,
    PC_RetrievePassword, PC_get_Properties,
};

/* ---------------- NetworkInformation 的静态工厂 ---------------- */

typedef struct NetInfoFactory { RT_HEAD } NetInfoFactory;

typedef struct NetInfoStaticsVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(NetInfoFactory*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(NetInfoFactory*);
    ULONG   (STDMETHODCALLTYPE *Release)(NetInfoFactory*);
    HRESULT (STDMETHODCALLTYPE *GetIids)(NetInfoFactory*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(NetInfoFactory*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(NetInfoFactory*, TrustLevel*);
    /* INetworkInformationStatics 的 8 个方法，顺序照 SDK 头 */
    HRESULT (STDMETHODCALLTYPE *GetConnectionProfiles)(NetInfoFactory*, void**);
    HRESULT (STDMETHODCALLTYPE *GetInternetConnectionProfile)(NetInfoFactory*, void**);
    HRESULT (STDMETHODCALLTYPE *GetLanIdentifiers)(NetInfoFactory*, void**);
    HRESULT (STDMETHODCALLTYPE *GetHostNames)(NetInfoFactory*, void**);
    HRESULT (STDMETHODCALLTYPE *GetProxyConfigurationAsync)(NetInfoFactory*, void*, void**);
    HRESULT (STDMETHODCALLTYPE *GetSortedEndpointPairs)(NetInfoFactory*, void*, int, void**);
    HRESULT (STDMETHODCALLTYPE *add_NetworkStatusChanged)(NetInfoFactory*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_NetworkStatusChanged)(NetInfoFactory*, EventRegistrationToken);
} NetInfoStaticsVtbl;

static HRESULT NI_QI(NetInfoFactory *s, REFIID iid, void **o)
{
    if (!o) return E_POINTER;
    if (IsEqualGUID(iid, &kIID_INetworkInformationStatics)) { rt_AddRef(s); *o = s; return S_OK; }
    return rt_QI(iid, &kIID_INetworkInformationStatics, s, o);   /* 静态类：不认 IActivationFactory */
}
static ULONG NI_AddRef(NetInfoFactory *s) { return rt_AddRef(s); }
static ULONG NI_Release(NetInfoFactory *s) { return rt_Release(s, free); }
static HRESULT NI_GetIids(NetInfoFactory *s, ULONG *n, IID **i)
{ (void)s; return rt_GetIids(&kIID_INetworkInformationStatics, n, i); }
static HRESULT NI_GetRCN(NetInfoFactory *s, HSTRING *h)
{ (void)s; static const WCHAR n[]=L"Windows.Networking.Connectivity.NetworkInformation";
  return h ? WindowsCreateString(n,(UINT32)wcslen(n),h) : E_POINTER; }
static HRESULT NI_GetTL(NetInfoFactory *s, TrustLevel *t) { (void)s; if(t)*t=BaseTrust; return S_OK; }

/* 一律"没有可用连接"。上层多半会走降级分支 —— 这正是我们要的：
 * 让它继续往下走，别卡在这里。真接 Linux 网络状态是后面的事。 */
static HRESULT NI_GetConnectionProfiles(NetInfoFactory *s, void **o) { (void)s; lg("[wifidirect] NetworkInformation::GetConnectionProfiles -> 空"); if(o)*o=NULL; return S_OK; }
static HRESULT NI_GetInternetConnectionProfile(NetInfoFactory *s, void **o) { (void)s; lg("[wifidirect] NetworkInformation::GetInternetConnectionProfile -> NULL"); if(o)*o=NULL; return S_OK; }
static HRESULT NI_GetLanIdentifiers(NetInfoFactory *s, void **o) { (void)s; if(o)*o=NULL; return S_OK; }
static HRESULT NI_GetHostNames(NetInfoFactory *s, void **o) { (void)s; if(o)*o=NULL; return S_OK; }
static HRESULT NI_GetProxyConfigurationAsync(NetInfoFactory *s, void *a, void **o) { (void)s;(void)a; if(o)*o=NULL; return S_OK; }
static HRESULT NI_GetSortedEndpointPairs(NetInfoFactory *s, void *a, int b, void **o) { (void)s;(void)a;(void)b; if(o)*o=NULL; return S_OK; }
static HRESULT NI_add_NSC(NetInfoFactory *s, void *h, EventRegistrationToken *t)
{ (void)s;(void)h; lg("[wifidirect] NetworkInformation::add_NetworkStatusChanged（记下，暂不回调）"); return rt_token(t); }
static HRESULT NI_remove_NSC(NetInfoFactory *s, EventRegistrationToken t) { (void)s;(void)t; return S_OK; }

static const NetInfoStaticsVtbl kNIVtbl = {
    NI_QI, NI_AddRef, NI_Release, NI_GetIids, NI_GetRCN, NI_GetTL,
    NI_GetConnectionProfiles, NI_GetInternetConnectionProfile,
    NI_GetLanIdentifiers, NI_GetHostNames,
    NI_GetProxyConfigurationAsync, NI_GetSortedEndpointPairs,
    NI_add_NSC, NI_remove_NSC,
};

/* ---------------- PasswordCredential 的工厂（ICredentialFactory）---------------- */

typedef struct CredFactory { RT_HEAD } CredFactory;

typedef struct CredFactoryVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(CredFactory*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(CredFactory*);
    ULONG   (STDMETHODCALLTYPE *Release)(CredFactory*);
    HRESULT (STDMETHODCALLTYPE *GetIids)(CredFactory*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(CredFactory*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(CredFactory*, TrustLevel*);
    /* ICredentialFactory */
    HRESULT (STDMETHODCALLTYPE *CreatePasswordCredential)(CredFactory*, HSTRING, HSTRING, HSTRING, void**);
} CredFactoryVtbl;

static HRESULT CF_QI(CredFactory *s, REFIID iid, void **o)
{
    if (!o) return E_POINTER;
    if (IsEqualGUID(iid, &kIID_ICredentialFactory)) { rt_AddRef(s); *o = s; return S_OK; }
    return rt_QI(iid, &kIID_ICredentialFactory, s, o);
}
static ULONG CF_AddRef(CredFactory *s) { return rt_AddRef(s); }
static ULONG CF_Release(CredFactory *s) { return rt_Release(s, free); }
static HRESULT CF_GetIids(CredFactory *s, ULONG *n, IID **i)
{ (void)s; return rt_GetIids(&kIID_ICredentialFactory, n, i); }
static HRESULT CF_GetRCN(CredFactory *s, HSTRING *h)
{ (void)s; static const WCHAR n[]=L"Windows.Security.Credentials.PasswordCredential";
  return h ? WindowsCreateString(n,(UINT32)wcslen(n),h) : E_POINTER; }
static HRESULT CF_GetTL(CredFactory *s, TrustLevel *t) { (void)s; if(t)*t=BaseTrust; return S_OK; }

static HRESULT CF_Create( CredFactory *s, HSTRING res, HSTRING usr, HSTRING pw, void **o)
{
    (void)s;
    if (!o) return E_POINTER;
    PasswordCred *p = (PasswordCred*)calloc(1, sizeof(*p));
    if (!p) return E_OUTOFMEMORY;
    p->lpVtbl = &kPCVtbl; p->ref = 1;
    PC_hput(res, p->resource, 256);
    PC_hput(usr, p->user, 256);
    PC_hput(pw,  p->pass, 256);
    lg("[wifidirect] CreatePasswordCredential(resource=\"%ls\", user=\"%ls\")", p->resource, p->user);
    *o = p;
    return S_OK;
}

static const CredFactoryVtbl kCFVtbl = {
    CF_QI, CF_AddRef, CF_Release, CF_GetIids, CF_GetRCN, CF_GetTL,
    CF_Create,
};

static const WCHAR *const kStubClasses[] = {
    /* 蓝牙：GATT 服务端 / 广播发布 —— 让手机发现并连上来 */
    L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementPublisher",
    L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEManufacturerData",
    L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattServiceProvider",
    L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattSession",
    L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattLocalCharacteristicParameters",
    L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattLocalCharacteristic",
    L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattServiceProviderAdvertisingParameters",
    /* Wi-Fi Direct 设备对象 */
    L"Windows.Devices.WiFiDirect.WiFiDirectDevice",
    /* 热点管理（真正的实现等下一步） */
    L"Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager",
    /* 凭据存储（PasswordCredential 已实现；Vault 是它的容器） */
    L"Windows.Security.Credentials.PasswordVault",
    /* 数据读写 */
    L"Windows.Storage.Streams.DataReader",
    L"Windows.Storage.Streams.DataWriter",
    NULL
};

static int is_stub_class(const WCHAR *name)
{
    for (int i = 0; kStubClasses[i]; ++i)
        if (_wcsicmp(name, kStubClasses[i]) == 0) return 1;
    return 0;
}

/* =================================================================
 * 导出
 * ================================================================= */

HRESULT WINAPI DllGetActivationFactory(HSTRING className, IActivationFactory **factory)
{
    if (!factory) return E_POINTER;
    *factory = NULL;

    UINT32 n = 0;
    const WCHAR *raw = className ? WindowsGetStringRawBuffer(className, &n) : NULL;
    char name[256] = {0};
    if (raw) wide_to_ascii(raw, name, sizeof(name));
    lg("[wifidirect] DllGetActivationFactory(\"%s\")", name);

    if (!raw) return CLASS_E_CLASSNOTAVAILABLE;

    /* 两个"带自己工厂接口"的类：工厂对象类型与通用 Factory 不同 */
    static const WCHAR kNetInfo[] = L"Windows.Networking.Connectivity.NetworkInformation";
    static const WCHAR kPwdCred[] = L"Windows.Security.Credentials.PasswordCredential";

    if (_wcsicmp(raw, kNetInfo) == 0) {
        NetInfoFactory *n = (NetInfoFactory*)calloc(1, sizeof(*n));
        if (!n) return E_OUTOFMEMORY;
        n->lpVtbl = &kNIVtbl; n->ref = 1;
        *factory = (IActivationFactory*)n;
        lg("[wifidirect]   -> NetworkInformation 静态工厂已创建");
        return S_OK;
    }
    if (_wcsicmp(raw, kPwdCred) == 0) {
        CredFactory *c = (CredFactory*)calloc(1, sizeof(*c));
        if (!c) return E_OUTOFMEMORY;
        c->lpVtbl = &kCFVtbl; c->ref = 1;
        *factory = (IActivationFactory*)c;
        lg("[wifidirect]   -> PasswordCredential 工厂已创建");
        return S_OK;
    }

    int kind;
    if (_wcsicmp(raw, kClassName) == 0)
        kind = KIND_PUBLISHER;
    else if (_wcsicmp(raw, kClassNameConnListener) == 0)
        kind = KIND_CONNLISTENER;
    else if (is_stub_class(raw))
        kind = KIND_STUB;          /* 类找得到，行为留白 —— 见文件里"通用兜底"一节 */
    else {
        lg("[wifidirect]   -> 不是我的类，返回 CLASS_E_CLASSNOTAVAILABLE");
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    Factory *f = Factory_new(kind);
    if (!f) return E_OUTOFMEMORY;
    *factory = (IActivationFactory*)f;
    lg("[wifidirect]   -> 工厂已创建 (kind=%d)", kind);
    return S_OK;
}

HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h; (void)r;
    if (reason == DLL_PROCESS_ATTACH) lg("==== WiFiDirect 垫片加载 pid=%lu ====", GetCurrentProcessId());
    if (reason == DLL_PROCESS_DETACH) lg("==== WiFiDirect 垫片卸载 ====");
    return TRUE;
}
