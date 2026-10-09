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

/* 读一个 HSTRING 参数 */
static void hstr_to_ascii(HSTRING h, char *dst, size_t cap)
{
    UINT32 n = 0;
    const WCHAR *raw = h ? WindowsGetStringRawBuffer(h, &n) : NULL;
    dst[0] = 0;
    if (raw) wide_to_ascii(raw, dst, cap);
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

static ULONG rt_AddRef(const void *self) { return (ULONG)InterlockedIncrement((LONG*)&((const LONG*)self)[1]); }
static ULONG rt_Release(const void *self, void (*dtor)(void*))
{
    LONG n = InterlockedDecrement((LONG*)&((const LONG*)self)[1]);
    if (n == 0 && dtor) dtor((void*)self);
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
    /* IInspectable */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Publisher*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(Publisher*);
    ULONG   (STDMETHODCALLTYPE *Release)(Publisher*);
    HRESULT (STDMETHODCALLTYPE *GetIids)(Publisher*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(Publisher*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(Publisher*, TrustLevel*);
    /* IActivationFactory */
    HRESULT (STDMETHODCALLTYPE *ActivateInstance)(Publisher*, void**);
    /* IWiFiDirectAdvertisementPublisher */
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
    if (IsEqualGUID(iid, &kIID_IWifiDirectAdvertisementPublisher)
     || IsEqualGUID(iid, &kIID_IActivationFactory)) {
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

static HRESULT PB_ActivateInstance(Publisher *s, void **out)
{
    if (!out) return E_POINTER;
    PB_AddRef(s);          /* 复用同一个对象作为实例 —— 语义上够用 */
    *out = s;
    lg("[wifidirect] ActivateInstance");
    return S_OK;
}

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
    PB_ActivateInstance,
    PB_get_Advertisement, PB_get_Status,
    PB_add_StatusChanged, PB_remove_StatusChanged,
    PB_Start, PB_Stop,
};

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

    if (!raw || _wcsicmp(raw, kClassName) != 0) {
        lg("[wifidirect]   -> 不是我的类，返回 CLASS_E_CLASSNOTAVAILABLE");
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    Publisher *p = (Publisher*)calloc(1, sizeof(*p));
    if (!p) return E_OUTOFMEMORY;
    p->lpVtbl = &kPBVtbl;
    p->ref = 1;
    p->adv = Advertisement_new();
    p->status = 0;                        /* Stopped */

    *factory = (IActivationFactory*)p;
    lg("[wifidirect]   -> 工厂已创建");
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
