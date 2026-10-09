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
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- 日志 */

static FILE *g_log;
static void lg(const char *fmt, ...)
{
    if (!g_log) {
        const char *p = getenv("SCDW_HOOK_LOG");
        g_log = fopen(p && *p ? p : "C:\\scdw_hook.log", "a");
        if (!g_log) return;
    }
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
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
    return TRUE;
}

/* 自己当 SCM：遍历服务表，逐个把 ServiceMain 跑起来。
 * 真 SCDW 会阻塞到服务结束 —— 我们也一样（ServiceMain 自己会转循环）。 */
static BOOL WINAPI my_StartServiceCtrlDispatcherW(LPSERVICE_TABLE_ENTRYW table)
{
    lg("[scdw] StartServiceCtrlDispatcherW 被钩子接管（不再进 Wine 的 sechost）");

    if (!table) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }

    int n = 0;
    for (LPSERVICE_TABLE_ENTRYW e = table; e->lpServiceName; ++e) {
        lg("[scdw]   表项[%d] 服务名=\"%ls\" ServiceMain=%p",
           n, e->lpServiceName, (void*)e->lpServiceProc);
        ++n;
    }
    if (n == 0) { SetLastError(ERROR_FAILED_SERVICE_CONTROLLER_CONNECT); return FALSE; }

    /* ServiceMain 的签名是 (DWORD argc, LPWSTR *argv)。
     * 真 SCDW 传 argc=1、argv[0]=服务名。我们照做（虽然 WRE 反编译显示
     * 这个 exe 的 ServiceMain 根本不读这两个参数）。 */
    for (LPSERVICE_TABLE_ENTRYW e = table; e->lpServiceName; ++e) {
        if (!e->lpServiceProc) continue;
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
        if (_stricmp(name, dllName) != 0) continue;

        IMAGE_THUNK_DATA *oft = imp->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk) : NULL;
        IMAGE_THUNK_DATA *ft  = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        if (!oft) continue;

        for (int i = 0; oft[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL(oft[i].u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft[i].u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, funcName) != 0) continue;

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
    const WCHAR *base = wcsrchr(me, L'\\');
    base = base ? base + 1 : me;

    int hit = 0;
    for (int i = 0; kTargets[i]; ++i)
        if (_wcsicmp(base, kTargets[i]) == 0) { hit = 1; break; }
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
