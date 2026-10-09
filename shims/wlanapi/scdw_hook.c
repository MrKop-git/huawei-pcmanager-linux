/*
 * IAT 钩子：把服务 exe 的 StartServiceCtrlDispatcherW 换成我们自己的实现。
 *
 * 为什么
 * ------
 * `HwDistributedMainService.exe` 由 SCM 拉起后，在 Wine 的
 * `StartServiceCtrlDispatcherW` 里 page fault（读地址 -1）。
 * 已排除：Wine 普遍缺陷（最小服务正常）、WOW64 标志、IE8 的 DLL 覆盖、
 * 缺的 WinRT 类、我们自己的其他垫片。它就是和 Wine 的服务初始化路径不合。
 *
 * 而这个服务**没有**脱离 SCM 独立运行的模式（WRE 反编译确认：/startup 也是走 SCM）。
 *
 * 做法
 * ----
 * 服务 exe 通过 `HiConnectivitySDK → wlanapi` 依赖链一定会加载我们这份
 * `wlanapi.dll`，**在它的 main() 之前**。于是我们在这里改它的 IAT：
 *
 *   StartServiceCtrlDispatcherW      -> 我们的：直接遍历服务表调 ServiceMain
 *   RegisterServiceCtrlHandlerExW/W  -> 我们的：给个假但稳定的句柄
 *   SetServiceStatus                 -> 我们的：记状态，返回 TRUE
 *
 * 也就是**自己当那个 SCM**。服务程序要的只是"能注册、能上报状态"的假象，
 * 它并不真的依赖 SCM 做什么。
 *
 * 边界
 * ----
 * 只在点名的主模块里安装（见 kTargets），避免影响同前缀下的其他服务。
 */

#include <windows.h>
#include <winsvc.h>
#include <stdarg.h>

/* ---------------------------------------------------------------- 日志
 *
 * ★ 故意**不用 CRT 的 printf** —— 这条日志会从 DllMain、钩子、以及被
 *   我们接管的服务初始化路径里调用，全都发生在 CRT 状态可能不干净的时机。
 *   实测过一次崩溃栈：[0..2] ucrtbase、[3][4] 我们自己 —— 像 printf 机制炸。
 *   这里只用 Win32（CreateFile/WriteFile）+ 手写格式化，零 CRT 依赖。
 */

static HANDLE g_log;
static char   g_line[1024];

static void put_str(char *dst, int *n, const char *s)
{
    for (; s && *s && *n < (int)sizeof(g_line) - 1; ++s) dst[(*n)++] = *s;
}
static void put_wstr(char *dst, int *n, const WCHAR *s)
{
    for (; s && *s && *n < (int)sizeof(g_line) - 1; ++s)
        dst[(*n)++] = (*s < 0x80) ? (char)*s : '?';
}
static void put_ulong(char *dst, int *n, unsigned long v, int base)
{
    char t[32]; int i = 0;
    if (v == 0) t[i++] = '0';
    while (v) { unsigned d = v % base; t[i++] = (char)(d < 10 ? '0'+d : 'a'+d-10); v /= base; }
    while (i > 0 && *n < (int)sizeof(g_line) - 1) dst[(*n)++] = t[--i];
}

static void lg(const char *fmt, ...)
{
    if (!g_log) {
        g_log = CreateFileA("C:\\scdw_hook.log",
                            FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_log == INVALID_HANDLE_VALUE) { g_log = NULL; return; }
    }

    int n = 0;
    va_list ap; va_start(ap, fmt);
    for (const char *f = fmt; *f && n < (int)sizeof(g_line) - 1; ++f) {
        if (*f != '%') { g_line[n++] = *f; continue; }
        ++f;
        int lng = 0;
        while (*f == 'l') { ++lng; ++f; }
        switch (*f) {
        case 's': {
            if (lng) put_wstr(g_line, &n, va_arg(ap, const WCHAR*));
            else      put_str (g_line, &n, va_arg(ap, const char*));
            break; }
        case 'd': {
            long v = lng ? va_arg(ap, long) : (long)va_arg(ap, int);
            if (v < 0) { g_line[n++] = '-'; v = -v; }
            put_ulong(g_line, &n, (unsigned long)v, 10); break; }
        case 'u': put_ulong(g_line, &n, lng ? va_arg(ap, unsigned long)
                                             : (unsigned long)va_arg(ap, unsigned), 10); break;
        case 'x': put_ulong(g_line, &n, lng ? va_arg(ap, unsigned long)
                                             : (unsigned long)va_arg(ap, unsigned), 16); break;
        case 'p': g_line[n++] = '0'; g_line[n++] = 'x';
                  put_ulong(g_line, &n, (unsigned long)(ULONG_PTR)va_arg(ap, void*), 16); break;
        case '%': g_line[n++] = '%'; break;
        default:  g_line[n++] = *f; break;
        }
    }
    va_end(ap);

    if (n < (int)sizeof(g_line) - 1) g_line[n++] = '\n';
    g_line[n] = 0;

    /* ★ 两条腿都走：写文件（给用户看）+ OutputDebugString（Wine 会把它打进
     *   和它自己消息同一条流，顺序真实、不会丢）。
     *   踩过：只写文件时，多进程 append 会让人误把"没写进去"当成"崩在这里"。 */
    OutputDebugStringA(g_line);
    if (g_log) { DWORD w = 0; WriteFile(g_log, g_line, (DWORD)n, &w, NULL); }
}

/* ------------------------------------------------------- 我们的实现 */

static SERVICE_STATUS_HANDLE FAKE_HANDLE = (SERVICE_STATUS_HANDLE)(ULONG_PTR)0x5343;

static SERVICE_STATUS_HANDLE WINAPI my_RegisterServiceCtrlHandlerExW(
        LPCWSTR name, LPHANDLER_FUNCTION_EX handler, LPVOID ctx)
{
    lg("[scdw] RegisterServiceCtrlHandlerExW(\"%ls\", handler=%p) -> 假句柄", name, handler);
    (void)ctx;
    return FAKE_HANDLE;
}

static SERVICE_STATUS_HANDLE WINAPI my_RegisterServiceCtrlHandlerW(
        LPCWSTR name, LPHANDLER_FUNCTION handler)
{
    lg("[scdw] RegisterServiceCtrlHandlerW(\"%ls\", handler=%p) -> 假句柄", name, handler);
    return FAKE_HANDLE;
}

static BOOL WINAPI my_SetServiceStatus(SERVICE_STATUS_HANDLE h, LPSERVICE_STATUS st)
{
    if (st) lg("[scdw] SetServiceStatus(state=%lu, exit=%lu, ckpt=%lu)",
               (unsigned long)st->dwCurrentState, (unsigned long)st->dwWin32ExitCode,
               (unsigned long)st->dwCheckPoint);
    (void)h;

    /* ★ 谁调的？失败分支里会 SetServiceStatus(STOPPED)，把返回地址抓出来
     *   就能定位是 ServiceMain 的哪条分支（我们没法读它自己的日志）。 */
    {
        typedef USHORT (WINAPI *pRtlCaptureStackBackTrace)(ULONG, ULONG, PVOID*, PULONG);
        static pRtlCaptureStackBackTrace cap;
        if (!cap) {
            HMODULE nt = GetModuleHandleA("ntdll.dll");
            if (nt) cap = (pRtlCaptureStackBackTrace)(void*)GetProcAddress(nt, "RtlCaptureStackBackTrace");
        }
        if (cap) {
            PVOID fr[8] = {0};
            USHORT n = cap(1, 8, fr, NULL);      /* 跳过第 0 帧（就是这里） */
            for (USHORT i = 0; i < n; ++i)
                lg("[scdw]     ↖ 调用者[%u] = 0x%p", (unsigned)i, fr[i]);
        }
    }
    return TRUE;
}

/* 自己当 SCM：遍历服务表，逐个把 ServiceMain 跑起来。
 * 真 SCDW 会阻塞到服务结束 —— 我们也一样（ServiceMain 自己会转循环）。 */
static BOOL WINAPI my_StartServiceCtrlDispatcherW(LPSERVICE_TABLE_ENTRYW table)
{
    lg("[scdw] StartServiceCtrlDispatcherW 被钩子接管（不再进 Wine 的 sechost）");

    if (!table) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }

    /* ★★★ 关键：**不能"读到 NULL 为止"**。
     *
     * 实测这个 exe 的服务表**没有 NULL 终止符** —— 表项后面紧跟着的是
     * .rdata 里别的字符串。按 NULL 遍历会读到 0x53205d6e69616d5b
     * （小端解出来是 ASCII 的 "[main] S"）当成指针去解引用，读到地址 -1 → 崩。
     *
     * Wine 的 StartServiceCtrlDispatcherW 正是这么崩的 —— 而且它崩的
     * 那个 rcx 值和我们这里读到的垃圾完全一致。
     *
     * 所以改成**校验名字指针是不是落在主模块的映射范围内**，
     * 不是就当表结束。 */
    HMODULE me = GetModuleHandleW(NULL);
    ULONG_PTR lo = (ULONG_PTR)me, hi = lo;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)lo;
    if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS*)((BYTE*)lo + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE)
            hi = lo + nt->OptionalHeader.SizeOfImage;
    }
    lg("[scdw] 主模块范围 0x%p - 0x%p", (void*)lo, (void*)hi);

    int n = 0;
    lg("[scdw] 第一遍：遍历服务表（带指针校验）");
    for (LPSERVICE_TABLE_ENTRYW e = table; ; ++e) {
        ULONG_PTR np = (ULONG_PTR)e->lpServiceName;
        ULONG_PTR pp = (ULONG_PTR)e->lpServiceProc;
        if (!np || !pp) { lg("[scdw]   遇到 NULL，表结束"); break; }
        if (np < lo || np >= hi) {
            lg("[scdw]   名字指针 0x%p 不在模块范围内 -> 判定为表结束"
               "（这个 exe 的表没有 NULL 终止符）", (void*)np);
            break;
        }
        lg("[scdw]   表项[%d] 服务名=\"%ls\" ServiceMain=0x%p",
           n, e->lpServiceName, (void*)pp);
        ++n;
        if (n > 32) { lg("[scdw]   !! 表项超过 32，防止跑飞，停下"); break; }
    }
    lg("[scdw] 第一遍结束，n=%d", n);
    if (n == 0) { SetLastError(ERROR_FAILED_SERVICE_CONTROLLER_CONNECT); return FALSE; }

    /* ServiceMain 的签名是 (DWORD argc, LPWSTR *argv)。
     * 真 SCDW 传 argc=1、argv[0]=服务名。我们照做（虽然 WRE 反编译显示
     * 这个 exe 的 ServiceMain 根本不读这两个参数）。 */
    lg("[scdw] 第二遍：开始调 ServiceMain");
    for (int i = 0; i < n; ++i) {
        LPSERVICE_TABLE_ENTRYW e = &table[i];
        LPWSTR argv[2];
        argv[0] = e->lpServiceName;
        argv[1] = NULL;
        lg("[scdw]   调用 ServiceMain(\"%ls\")", e->lpServiceName);
        e->lpServiceProc(1, argv);          /* 会阻塞到服务结束 */
        lg("[scdw]   ServiceMain(\"%ls\") 返回", e->lpServiceName);
    }
    lg("[scdw] 全部 ServiceMain 结束");
    return TRUE;
}

/* --------------------------------------------------------- IAT 改写 */

/* 在 module 的导入表里找 dllName!funcName 的槽，改成 replacement。
 * 返回原函数指针（可能为 NULL）。 */
static void *hook_one(HMODULE mod, const char *dllName, const char *funcName, void *replacement)
{
    BYTE *base = (BYTE*)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;

    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;

    IMAGE_DATA_DIRECTORY *dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return NULL;

    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress);
    for (; imp->Name; ++imp) {
        const char *name = (const char*)(base + imp->Name);
        if (lstrcmpiA(name, dllName) != 0) continue;

        IMAGE_THUNK_DATA *oft = imp->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk) : NULL;
        IMAGE_THUNK_DATA *ft  = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        if (!oft) continue;

        for (int i = 0; oft[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL(oft[i].u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft[i].u1.AddressOfData);
            if (lstrcmpA((const char*)ibn->Name, funcName) != 0) continue;

            void *old = (void*)ft[i].u1.Function;
            DWORD oldprot = 0;
            if (!VirtualProtect(&ft[i].u1.Function, sizeof(void*), PAGE_READWRITE, &oldprot)) {
                lg("[scdw] !! VirtualProtect 失败 @ %s!%s", dllName, funcName);
                return NULL;
            }
            ft[i].u1.Function = (ULONG_PTR)replacement;
            VirtualProtect(&ft[i].u1.Function, sizeof(void*), oldprot, &oldprot);
            lg("[scdw] 已钩 %s!%s : %p -> %p", dllName, funcName, old, replacement);
            return old;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------- 安装 */

static const WCHAR *kTargets[] = {
    L"HwDistributedMainService.exe",
    L"HiConnectivityService.exe",
    NULL
};

void scdw_hook_install(void)
{
    /* 只在点名的主模块里动手，免得影响同前缀下的其他服务 */
    WCHAR me[MAX_PATH] = {0};
    if (!GetModuleFileNameW(NULL, me, MAX_PATH)) return;
    const WCHAR *base = me;                       /* 取文件名部分（不依赖 CRT 的 wcsrchr） */
    for (const WCHAR *p = me; *p; ++p)
        if (*p == (WCHAR)'\\') base = p + 1;

    int hit = 0;
    for (int i = 0; kTargets[i]; ++i)
        if (lstrcmpiW(base, kTargets[i]) == 0) { hit = 1; break; }
    if (!hit) return;

    HMODULE exe = GetModuleHandleW(NULL);
    lg("==== 在 %ls 里安装 SCDW 钩子 (module %p) ====", base, (void*)exe);

    hook_one(exe, "advapi32.dll", "StartServiceCtrlDispatcherW", (void*)my_StartServiceCtrlDispatcherW);
    hook_one(exe, "advapi32.dll", "RegisterServiceCtrlHandlerExW", (void*)my_RegisterServiceCtrlHandlerExW);
    hook_one(exe, "advapi32.dll", "RegisterServiceCtrlHandlerW",  (void*)my_RegisterServiceCtrlHandlerW);
    hook_one(exe, "advapi32.dll", "SetServiceStatus",             (void*)my_SetServiceStatus);

    /* 注意：KERNEL32 里也有 RegisterServiceCtrlHandlerExW 的转发，一并看看 */
    hook_one(exe, "KERNEL32.dll", "RegisterServiceCtrlHandlerExW", (void*)my_RegisterServiceCtrlHandlerExW);
    lg("==== 钩子安装结束 ====");
}
