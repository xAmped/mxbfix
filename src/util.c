/*
 * Paths, game identity, safe memory reads, string conversion and logging.
 */
#include "common.h"
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

wchar_t  g_save_dir[MAX_PATH];
wchar_t  g_base_dir[MAX_PATH];
wchar_t  g_game_dir[MAX_PATH];
wchar_t  g_user_profile[MAX_PATH];
DWORD    g_game_thread_id;
uint8_t *g_game_base;
uint32_t g_game_size;
uint32_t g_game_build;

static HANDLE           s_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION s_log_lock;
static int              s_log_ready;

int env_get(const char *name, char *buf, unsigned cap)
{
    DWORD n = GetEnvironmentVariableA(name, buf, cap);
    if (n == 0 || n >= cap) { if (cap) buf[0] = 0; return 0; }
    return 1;
}

/* dir + "\\" + name into out. If dir is empty or the result doesn't fit, out becomes "" and 0 is returned,
 * so a file operation on it fails instead of touching a relative or truncated (possibly colliding) path.
 * out may be the same buffer as dir. */
int path_join(wchar_t *out, size_t cap, const wchar_t *dir, const wchar_t *name)
{
    size_t n = wcslen(dir), m = wcslen(name);
    int sep = n && dir[n - 1] != L'\\';
    if (!n || cap == 0 || n + sep + m >= cap) {
        if (cap) out[0] = 0;
        return 0;
    }
    wmemmove(out, dir, n);
    if (sep) out[n++] = L'\\';
    wmemcpy(out + n, name, m);
    out[n + m] = 0;
    return 1;
}

/* Create a directory and its parents. */
void ensure_dir(const wchar_t *path)
{
    wchar_t tmp[MAX_PATH];
    size_t n = wcslen(path);
    if (n >= MAX_PATH) return;
    wmemcpy(tmp, path, n + 1);
    for (size_t i = 3; i <= n; i++) {
        if (tmp[i] == L'\\' || tmp[i] == 0) {
            wchar_t c = tmp[i];
            tmp[i] = 0;
            CreateDirectoryW(tmp, NULL);
            tmp[i] = c;
        }
    }
}

void util_init_paths(const char *save_path)
{
    wchar_t save[MAX_PATH] = {0};
    if (save_path) MultiByteToWideChar(CP_ACP, 0, save_path, -1, save, MAX_PATH);
    if (!save[0]) GetTempPathW(MAX_PATH, save);           /* defensive: game always passes one */
    path_join(g_save_dir, MAX_PATH, save, L"");
    path_join(g_base_dir, MAX_PATH, save, L"mxbfix\\");
    ensure_dir(g_base_dir);

    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    while (n && exe[n - 1] != L'\\') n--;
    exe[n] = 0;
    wcscpy(g_game_dir, exe);

    DWORD up = GetEnvironmentVariableW(L"USERPROFILE", g_user_profile, MAX_PATH);
    if (up == 0 || up >= MAX_PATH) g_user_profile[0] = 0;     /* missing or too long: no profile to scrub */
}

void util_init_game_identity(void)
{
    g_game_base = (uint8_t *)GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_game_base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_game_base + dos->e_lfanew);
    g_game_size  = nt->OptionalHeader.SizeOfImage;
    g_game_build = nt->FileHeader.TimeDateStamp;
}

int safe_read(const void *addr, void *out, size_t n)
{
    SIZE_T got = 0;
    if (!addr) return 0;
    return ReadProcessMemory(GetCurrentProcess(), addr, out, n, &got) && got == n;
}

void acp_to_utf8(const char *src, size_t src_max, char *dst, size_t cap)
{
    wchar_t w[512];
    size_t len = 0;
    while (len < src_max && src[len]) len++;
    int wn = MultiByteToWideChar(CP_ACP, 0, src, (int)len, w, 511);
    int n = WideCharToMultiByte(CP_UTF8, 0, w, wn, dst, (int)cap - 1, NULL, NULL);
    if (n < 0) n = 0;
    dst[n] = 0;
}

void utc_now_iso(char *out)
{
    SYSTEMTIME t;
    GetSystemTime(&t);
    wsprintfA(out, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

void log_init(void)
{
    wchar_t path[MAX_PATH], old[MAX_PATH];
    path_join(path, MAX_PATH, g_base_dir, L"mxbfix.log");
    path_join(old, MAX_PATH, g_base_dir, L"mxbfix.prev.log");
    MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);   /* keep one previous log */
    InitializeCriticalSection(&s_log_lock);
    s_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    s_log_ready = 1;
}

void log_msg(const char *fmt, ...)
{
    if (!s_log_ready || s_log == INVALID_HANDLE_VALUE) return;
    char line[2048];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = wsprintfA(line, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - n - 3, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    if (m > (int)(sizeof line - n - 3)) m = (int)(sizeof line - n - 3);
    n += m;
    line[n++] = '\r';
    line[n++] = '\n';
    DWORD w;
    EnterCriticalSection(&s_log_lock);
    WriteFile(s_log, line, (DWORD)n, &w, NULL);
    LeaveCriticalSection(&s_log_lock);
}

void log_close(void)
{
    if (!s_log_ready) return;
    EnterCriticalSection(&s_log_lock);
    if (s_log != INVALID_HANDLE_VALUE) CloseHandle(s_log);
    s_log = INVALID_HANDLE_VALUE;
    s_log_ready = 0;
    LeaveCriticalSection(&s_log_lock);
}
