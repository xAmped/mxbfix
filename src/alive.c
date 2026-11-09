#include "alive.h"
#include <stdio.h>
#include <string.h>

#define ALIVE_EVERY_MS 2000
#define RECORD_LEN     160

static HANDLE           s_file = INVALID_HANDLE_VALUE;
static HANDLE           s_thread, s_stop;
static wchar_t          s_path[MAX_PATH];
static volatile LONG64  s_game_ft;        /* FILETIME of the game thread's last callback */
static HWND volatile    s_hwnd;           /* the game's window, once seen in front */
static ULONGLONG        s_hwnd_checked;
static uint64_t         s_created;

static uint64_t now_ft(void)
{
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

static int alive_dir(wchar_t *out)
{
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n || n >= MAX_PATH) return 0;
    if (!path_join(out, MAX_PATH, tmp, L"mxbfix\\")) return 0;
    CreateDirectoryW(out, NULL);
    return 1;
}

static int alive_path(wchar_t *out, unsigned long pid)
{
    wchar_t dir[MAX_PATH], name[40];
    if (!alive_dir(dir)) return 0;
    wsprintfW(name, L"alive-%lu.txt", pid);
    return path_join(out, MAX_PATH, dir, name);
}

/* A window that belongs to the game thread itself (not a dialog or a helper window on another thread). */
static int game_window(HWND w)
{
    DWORD pid = 0, tid = w ? GetWindowThreadProcessId(w, &pid) : 0;
    return w && pid == GetCurrentProcessId() && tid == g_game_thread_id;
}

void alive_beat(void)
{
    InterlockedExchange64(&s_game_ft, (LONG64)now_ft());
    ULONGLONG t = GetTickCount64();
    if (s_hwnd && t - s_hwnd_checked < 5000) return;
    s_hwnd_checked = t;
    HWND w = GetForegroundWindow();
    static int s_said;
    if (game_window(w)) {
        if (!s_hwnd && !s_said++) log_msg("freeze record: watching the game window");
        s_hwnd = w;
    } else if (w && !s_said) {
        DWORD pid = 0, tid = GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId()) {             /* ours, but not the game thread's: say so once (unexpected) */
            s_said = 1;
            log_msg("freeze record: the game's window in front belongs to thread %lu, not the game thread %lu; not watched", tid,
                    (unsigned long)g_game_thread_id);
        }
    }
}

/* 1 hung, 0 responding, -1 not known (no game window seen yet, or it's gone or no longer the game's). */
static int hung_now(void)
{
    HWND h = s_hwnd;
    if (!h || !IsWindow(h) || !game_window(h)) return -1;
    return IsHungAppWindow(h) ? 1 : 0;
}

static DWORD WINAPI alive_thread(LPVOID unused)
{
    (void)unused;
    uint64_t hung_since = 0;
    do {
        uint64_t now = now_ft();
        int hung = hung_now();
        if (hung != 1) hung_since = 0;
        else if (!hung_since) hung_since = now;
        char rec[RECORD_LEN + 1];
        int n = snprintf(rec, sizeof rec, "mxbfix-alive 1 pid=%lu created=%016llx process=%016llx game=%016llx hung_since=%016llx",
                         GetCurrentProcessId(), (unsigned long long)s_created, (unsigned long long)now,
                         (unsigned long long)s_game_ft, (unsigned long long)hung_since);
        if (n < 0 || n >= RECORD_LEN) continue;
        memset(rec + n, ' ', RECORD_LEN - 2 - n);         /* fixed length: one small write over the same bytes */
        rec[RECORD_LEN - 2] = '\r';
        rec[RECORD_LEN - 1] = '\n';
        OVERLAPPED at;
        memset(&at, 0, sizeof at);
        DWORD w;
        WriteFile(s_file, rec, RECORD_LEN, &w, &at);
    } while (WaitForSingleObject(s_stop, ALIVE_EVERY_MS) == WAIT_TIMEOUT);
    return 0;
}

void alive_start(void)
{
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) s_created = ((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime;
    if (!alive_path(s_path, GetCurrentProcessId())) return;
    s_file = CreateFileW(s_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (s_file == INVALID_HANDLE_VALUE) { log_msg("freeze record: could not create it (error %lu)", GetLastError()); return; }
    s_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (s_stop) s_thread = CreateThread(NULL, 0, alive_thread, NULL, 0, NULL);
    if (!s_thread) { CloseHandle(s_file); s_file = INVALID_HANDLE_VALUE; DeleteFileW(s_path); }
}

void alive_stop(void)
{
    if (s_thread) {
        SetEvent(s_stop);
        if (WaitForSingleObject(s_thread, 3000) == WAIT_OBJECT_0) { CloseHandle(s_thread); s_thread = NULL; }
    }
    if (s_thread) return;                                 /* still writing: leave the file; the next start ignores it */
    if (s_stop) { CloseHandle(s_stop); s_stop = NULL; }
    if (s_file != INVALID_HANDLE_VALUE) { CloseHandle(s_file); s_file = INVALID_HANDLE_VALUE; }
    if (s_path[0]) DeleteFileW(s_path);
}

/* " key=" followed by exactly 16 hex digits and a space (the writer always pads to 16): anything else is a damaged record. */
static int field(const char *rec, const char *key, uint64_t *v)
{
    const char *p = strstr(rec, key);
    if (!p || p == rec || p[-1] != ' ') return 0;
    p += strlen(key);
    uint64_t x = 0;
    for (int n = 0; n < 16; n++, p++) {
        int d = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 : -1;
        if (d < 0) return 0;
        x = x * 16 + (uint64_t)d;
    }
    if (*p != ' ') return 0;
    *v = x;
    return 1;
}

int alive_read_previous(unsigned long pid, uint64_t created, alive_info_t *out)
{
    memset(out, 0, sizeof *out);
    wchar_t path[MAX_PATH];
    if (!pid || !alive_path(path, pid)) return 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char rec[RECORD_LEN + 1];
    DWORD got = 0;
    BOOL ok = ReadFile(h, rec, RECORD_LEN, &got, NULL);
    CloseHandle(h);
    DeleteFileW(path);
    if (!ok || got != RECORD_LEN || rec[RECORD_LEN - 2] != '\r' || rec[RECORD_LEN - 1] != '\n') return 0;   /* whole records only */
    rec[got] = 0;
    if (strlen(rec) != RECORD_LEN) return 0;                                                               /* no zeros inside */
    char want[40];
    snprintf(want, sizeof want, "mxbfix-alive 1 pid=%lu ", pid);
    uint64_t c = 0, proc = 0, game = 0, hs = 0;
    if (strncmp(rec, want, strlen(want)) || !field(rec, "created=", &c) || !field(rec, "process=", &proc) ||
        !field(rec, "game=", &game) || !field(rec, "hung_since=", &hs))
        return 0;
    /* The same process: its creation time (1 s for rounding), so a reused pid can't borrow another run's record. */
    uint64_t diff = c > created ? c - created : created - c;
    if (!created || diff > 10000000ull || !proc) return 0;
    /* Every time inside that process's life: started <= game, hung_since <= last seen, and no more than a week long. */
    const uint64_t WEEK = 7ull * 24 * 3600 * 10000000ull;
    if (proc < c || proc - c > WEEK || (game && (game < c || game > proc)) || (hs && (hs < c || hs > proc))) return 0;
    out->found = 1;
    out->process_ft = proc;
    out->game_quiet_s = game && game <= proc ? (int64_t)((proc - game) / 10000000ull) : -1;
    out->hung = hs != 0 && hs <= proc;
    out->hung_s = out->hung ? (int64_t)((proc - hs) / 10000000ull) : 0;
    return 1;
}
