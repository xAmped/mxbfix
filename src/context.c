#include "context.h"
#include <psapi.h>
#include <wchar.h>

session_ctx_t g_ctx;
env_info_t    g_env;

/* ---- event ring: last 64 plugin callbacks, written only on the game thread ---- */
#define RING 64
static event_rec_t   s_ring[RING];
static volatile LONG s_ring_next;

void ctx_event(const char *name, int a, int b)
{
    LONG i = s_ring_next;
    event_rec_t *e = &s_ring[i % RING];
    e->tick = GetTickCount64();
    e->name = name;
    e->a = a;
    e->b = b;
    s_ring_next = i + 1;
}

int ctx_events_snapshot(event_rec_t *out, int max)
{
    LONG end = s_ring_next;
    LONG start = end > RING ? end - RING : 0;
    if (end - start > max) start = end - max;
    int n = 0;
    for (LONG i = start; i < end; i++) out[n++] = s_ring[i % RING];
    return n;
}

/* ---- riders in the event (RaceAddEntry / RaceRemoveEntry) ---- */
#define MAX_RIDERS 256
static int s_riders[MAX_RIDERS];
static int s_nriders;

void ctx_reset_riders(void) { s_nriders = 0; g_ctx.riders = 0; }

void ctx_rider_add(int num, int unactive)
{
    for (int i = 0; i < s_nriders; i++)
        if (s_riders[i] == num) {
            if (unactive) { s_riders[i] = s_riders[--s_nriders]; g_ctx.riders = s_nriders; }
            return;
        }
    if (!unactive && s_nriders < MAX_RIDERS) s_riders[s_nriders++] = num;
    g_ctx.riders = s_nriders;
}

void ctx_rider_remove(int num) { ctx_rider_add(num, 1); }

/* ---- environment ---- */
static void w2u(const wchar_t *w, char *out, int cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
    if (n <= 0) out[0] = 0;
}

static int file_version(const wchar_t *path, unsigned v[4])
{
    DWORD h = 0, sz = GetFileVersionInfoSizeW(path, &h);
    if (!sz || sz > 1 << 20) return 0;
    void *buf = HeapAlloc(GetProcessHeap(), 0, sz);
    if (!buf) return 0;
    int ok = 0;
    VS_FIXEDFILEINFO *ffi;
    UINT len;
    if (GetFileVersionInfoW(path, 0, sz, buf) && VerQueryValueW(buf, L"\\", (void **)&ffi, &len) && len) {
        v[0] = HIWORD(ffi->dwFileVersionMS); v[1] = LOWORD(ffi->dwFileVersionMS);
        v[2] = HIWORD(ffi->dwFileVersionLS); v[3] = LOWORD(ffi->dwFileVersionLS);
        ok = 1;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return ok;
}

static int product_name(const wchar_t *path, wchar_t *out, int cap)
{
    DWORD h = 0, sz = GetFileVersionInfoSizeW(path, &h);
    out[0] = 0;
    if (!sz || sz > 1 << 20) return 0;
    void *buf = HeapAlloc(GetProcessHeap(), 0, sz);
    if (!buf) return 0;
    struct { WORD lang, cp; } *tr;
    UINT len;
    if (GetFileVersionInfoW(path, 0, sz, buf) && VerQueryValueW(buf, L"\\VarFileInfo\\Translation", (void **)&tr, &len) && len >= 4) {
        wchar_t q[64];
        wchar_t *val;
        UINT vl;
        wsprintfW(q, L"\\StringFileInfo\\%04x%04x\\ProductName", tr->lang, tr->cp);
        if (VerQueryValueW(buf, q, (void **)&val, &vl) && vl) lstrcpynW(out, val, cap);
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return out[0] != 0;
}

void env_collect_startup(void)
{
    typedef LONG (WINAPI *RtlGetVersion_t)(RTL_OSVERSIONINFOW *);
    RtlGetVersion_t rgv = (RtlGetVersion_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    RTL_OSVERSIONINFOW vi = {sizeof vi};
    if (rgv && rgv(&vi) == 0)
        wsprintfA(g_env.os, "Windows %lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);

    wchar_t cpu[96];
    DWORD cb = sizeof cpu;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, NULL, cpu, &cb) == ERROR_SUCCESS) {
        wchar_t *p = cpu;
        while (*p == L' ') p++;
        size_t n = wcslen(p);
        while (n && p[n - 1] == L' ') p[--n] = 0;
        w2u(p, g_env.cpu, sizeof g_env.cpu);
    }

    MEMORYSTATUSEX ms = {sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) g_env.ram_mb = ms.ullTotalPhys >> 20;

    DISPLAY_DEVICEW dd = {sizeof dd};
    for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++) {
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) { w2u(dd.DeviceString, g_env.gpu, sizeof g_env.gpu); break; }
        dd.cb = sizeof dd;
    }
}

void env_collect_late(void)
{
    HMODULE mods[512];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    int n = (int)(need / sizeof(HMODULE));
    if (n > 512) n = 512;
    size_t gdl = wcslen(g_game_dir);
    for (int i = 0; i < n; i++) {
        wchar_t path[MAX_PATH];
        if (!GetModuleFileNameW(mods[i], path, MAX_PATH)) continue;
        const wchar_t *name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;

        /* GPU OpenGL driver */
        if (!_wcsicmp(name, L"nvoglv64.dll") || !_wcsicmp(name, L"atio6axx.dll") || !_wcsnicmp(name, L"ig", 2)) {
            if (_wcsnicmp(name, L"ig", 2) == 0 && !wcsstr(name, L"icd64")) continue;
            unsigned v[4];
            w2u(name, g_env.gpu_driver_module, sizeof g_env.gpu_driver_module);
            if (file_version(path, v)) {
                if (!_wcsicmp(name, L"nvoglv64.dll")) {
                    /* NVIDIA: file 32.0.16.1692 -> driver 616.92 (last five digits) */
                    unsigned d = (v[2] % 10) * 10000 + v[3];
                    wsprintfA(g_env.gpu_driver_version, "%u.%02u (file %u.%u.%u.%u)", d / 100, d % 100, v[0], v[1], v[2], v[3]);
                } else {
                    wsprintfA(g_env.gpu_driver_version, "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
                }
            }
        }

        /* ReShade installed as a proxy DLL in the game folder */
        if (gdl && _wcsnicmp(path, g_game_dir, gdl) == 0 && !wcschr(path + gdl, L'\\')) {
            wchar_t prod[64];
            if (product_name(path, prod, 64) && wcsstr(prod, L"ReShade")) {
                unsigned v[4];
                g_env.reshade = 1;
                if (file_version(path, v)) wsprintfA(g_env.reshade_version, "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
            }
        }
    }
}
