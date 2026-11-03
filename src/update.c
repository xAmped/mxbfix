#include "common.h"
#include "update.h"
#include "update_key.h"
#include <bcrypt.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>

#define API_HOST L"api.github.com"
#define API_PATH L"/repos/xAmped/mxbfix/releases/latest"
#define DL_HOST  L"github.com"
#define MAX_PLUGIN (16u * 1024 * 1024)

static volatile LONG s_state = UPDATE_OFF;
static char          s_latest[24];
static HANDLE        s_thread;
static DWORD         s_error;
static volatile LONG s_install = INSTALL_IDLE;
static HANDLE        s_install_thread;
static volatile LONG s_install_stop;
/* Progress for the page. The steps are real; each stays up long enough to read. Those pauses are on this background thread. */
enum { STEP_DOWNLOAD, STEP_VERIFY, STEP_INSTALL };
static const char *const STEP_LABEL[3] = {"Downloading update...", "Checking the signature...", "Installing..."};
static const float STEP_FROM[4] = {0.0f, 0.60f, 0.80f, 1.0f};
static const DWORD STEP_MIN_MS[3] = {1300, 800, 800};
static volatile LONG s_step, s_got, s_total;
static volatile ULONGLONG s_step_start;

/* "v1.2.3" -> 1, 0 if it isn't a plain version. */
static int parse_version(const char *s, int v[3])
{
    if (*s == 'v' || *s == 'V') s++;
    int n = 0;
    return sscanf(s, "%d.%d.%d%n", &v[0], &v[1], &v[2], &n) == 3 && s[n] == 0 && v[0] >= 0 && v[1] >= 0 && v[2] >= 0;
}

/* The tag from GitHub: decide and publish. */
static void conclude(const char *tag)
{
    int have[3], got[3];
    if (!parse_version(MXBFIX_VERSION, have) || !parse_version(tag, got)) {
        log_msg("update check: unexpected version '%s'", tag);
        InterlockedExchange(&s_state, UPDATE_FAILED);
        return;
    }
    snprintf(s_latest, sizeof s_latest, "%d.%d.%d", got[0], got[1], got[2]);
    int newer = 0;
    for (int i = 0; i < 3 && !newer; i++) {
        if (got[i] > have[i]) newer = 1;
        else if (got[i] < have[i]) break;
    }
    log_msg("update check: latest is %s, this is %s%s", s_latest, MXBFIX_VERSION, newer ? ": update available" : "");
    InterlockedExchange(&s_state, newer ? UPDATE_AVAILABLE : UPDATE_CURRENT);
}

/* Pull "tag_name":"..." out of the response. It's near the start of GitHub's JSON. */
static int find_tag(const char *json, char *out, size_t cap)
{
    const char *p = strstr(json, "\"tag_name\"");
    if (!p) return 0;
    p = strchr(p + 10, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (p[n] && p[n] != '"' && n + 1 < cap) { out[n] = p[n]; n++; }
    if (p[n] != '"') return 0;
    out[n] = 0;
    return 1;
}

static int fetch_latest(char *tag, size_t cap)
{
    static char body[32768];
    int ok = 0;
    wchar_t ua[48];
    wsprintfW(ua, L"mxbfix/%hs", MXBFIX_VERSION);
    HINTERNET ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return 0;
    WinHttpSetTimeouts(ses, 8000, 8000, 8000, 8000);
    HINTERNET con = WinHttpConnect(ses, API_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", API_PATH, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE) : NULL;
    if (req && WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                            &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            DWORD total = 0, got = 0;
            while (total < sizeof body - 1 && WinHttpReadData(req, body + total, (DWORD)(sizeof body - 1 - total), &got) && got)
                total += got;
            body[total] = 0;
            ok = find_tag(body, tag, cap);
        } else {
            log_msg("update check: GitHub answered %lu", status);
        }
    }
    if (!ok) s_error = GetLastError();
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

static DWORD WINAPI worker(LPVOID unused)
{
    (void)unused;
    char tag[32];
    if (fetch_latest(tag, sizeof tag)) conclude(tag);
    else {
        log_msg("update check: couldn't get the latest version from GitHub (error %lu)", s_error);
        InterlockedExchange(&s_state, UPDATE_FAILED);
    }
    return 0;
}

void update_start(void)
{
    if (s_thread || !cfg_int("updates", "check", 1)) return;
    InterlockedExchange(&s_state, UPDATE_CHECKING);
    s_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!s_thread) InterlockedExchange(&s_state, UPDATE_FAILED);
}

void update_stop(void)
{
    if (s_install_thread) {
        /* A download stops at its next read; a swap in progress finishes, it's a few quick file moves. */
        s_install_stop = 1;
        if (WaitForSingleObject(s_install_thread, 3000) == WAIT_TIMEOUT) {
            HMODULE pin;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)(void *)update_stop, &pin);
        }
        CloseHandle(s_install_thread);
        s_install_thread = NULL;
    }
    if (!s_thread) return;
    /* A request in progress ends by itself (it has timeouts). Don't wait long: pin the DLL instead. */
    if (WaitForSingleObject(s_thread, 2000) == WAIT_TIMEOUT) {
        HMODULE self;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)(void *)update_stop, &self);
        log_msg("update check still running at shutdown; plugin pinned in memory until exit");
    }
    CloseHandle(s_thread);
    s_thread = NULL;
}

/* ---- install ---- */

static DWORD fetch_url(const wchar_t *path, unsigned char *out, DWORD cap, int track)
{
    DWORD total = 0;
    wchar_t ua[48];
    wsprintfW(ua, L"mxbfix/%hs", MXBFIX_VERSION);
    HINTERNET ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return 0;
    WinHttpSetTimeouts(ses, 10000, 10000, 15000, 30000);
    HINTERNET con = WinHttpConnect(ses, DL_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, NULL)) {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            DWORD got = 0, want = 0;
            size = sizeof want;
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &want, &size, WINHTTP_NO_HEADER_INDEX) && track)
                InterlockedExchange(&s_total, (LONG)want);
            while (!s_install_stop && total < cap && WinHttpReadData(req, out + total, cap - total, &got) && got) {
                total += got;
                if (track) InterlockedExchange(&s_got, (LONG)total);
            }
            if (total >= cap) total = 0;                 /* too big: not ours */
        } else {
            log_msg("update install: GitHub answered %lu", status);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return total;
}

/* A file from the release. */
static DWORD get_asset(const char *name, unsigned char *out, DWORD cap)
{
    wchar_t url[160];
    wsprintfW(url, L"/xAmped/mxbfix/releases/download/v%hs/%hs", s_latest, name);
    return fetch_url(url, out, cap, !strcmp(name, "mxbfix.dlo"));
}

/* DER "SEQUENCE { INTEGER r, INTEGER s }" to the 64 raw bytes Windows expects. */
static int der_to_raw(const unsigned char *d, DWORD n, unsigned char raw[64])
{
    DWORD i = 0;
    if (n < 8 || d[i++] != 0x30) return 0;
    if (d[i] & 0x80) { if (d[i] != 0x81) return 0; i++; }
    i++;
    for (int part = 0; part < 2; part++) {
        if (i + 2 > n || d[i++] != 0x02) return 0;
        DWORD len = d[i++];
        if (i + len > n || len == 0) return 0;
        const unsigned char *v = d + i;
        DWORD vl = len;
        while (vl > 32 && *v == 0) { v++; vl--; }
        if (vl > 32) return 0;
        memset(raw + part * 32, 0, 32);
        memcpy(raw + part * 32 + (32 - vl), v, vl);
        i += len;
    }
    return 1;
}

static int sha256(const void *data, DWORD n, unsigned char out[32])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    int ok = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return 0;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        ok = BCryptHashData(h, (PUCHAR)data, n, 0) == 0 && BCryptFinishHash(h, out, 32, 0) == 0;
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* The signature must cover "mxbfix-update|<the version we asked for>|<sha256 of this file>". */
static int signature_ok(const unsigned char *file, DWORD n, const unsigned char *sig, DWORD sig_len)
{
    unsigned char digest[32], raw[64], msg_hash[32];
    char hex[65], msg[128];
    struct { BCRYPT_ECCKEY_BLOB h; unsigned char xy[64]; } blob;
    int ok = 0;
    if (!sha256(file, n, digest) || !der_to_raw(sig, sig_len, raw)) return 0;
    for (int i = 0; i < 32; i++) wsprintfA(hex + 2 * i, "%02x", digest[i]);
    int m = snprintf(msg, sizeof msg, "mxbfix-update|%s|%s", s_latest, hex);
    if (m <= 0 || m >= (int)sizeof msg || !sha256(msg, (DWORD)m, msg_hash)) return 0;
    blob.h.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.h.cbKey = 32;
    memcpy(blob.xy, UPDATE_PUBLIC_KEY, 64);
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0) == 0) {
        if (BCryptImportKeyPair(alg, NULL, BCRYPT_ECCPUBLIC_BLOB, &key, (PUCHAR)&blob, sizeof blob, 0) == 0) {
            ok = BCryptVerifySignature(key, NULL, msg_hash, 32, raw, 64, 0) == 0;
            BCryptDestroyKey(key);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return ok;
}

static int file_equals(const wchar_t *path, const unsigned char *data, DWORD n)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int same = GetFileSize(h, NULL) == n;
    static unsigned char buf[65536];
    DWORD at = 0, got = 0;
    while (same && at < n && ReadFile(h, buf, sizeof buf, &got, NULL) && got) {
        same = at + got <= n && !memcmp(buf, data + at, got);
        at += got;
    }
    CloseHandle(h);
    return same && at == n;
}

/* A loaded file can't be replaced, only renamed. So a crash at any point below leaves at least one working plugin: the new one goes
 * in under a second name first (the game loads every .dlo, and a second copy just refuses to start), then the current one moves
 * out, then the new one takes the usual name. Two game instances take turns. */
static int swap_in(const unsigned char *file, DWORD n)
{
    HMODULE self;
    wchar_t cur[MAX_PATH], tmp[MAX_PATH], second[MAX_PATH], dir[MAX_PATH], back[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)swap_in, &self)) return 0;
    DWORD len = GetModuleFileNameW(self, cur, MAX_PATH);
    if (!len || len + 16 >= MAX_PATH) return 0;
    wsprintfW(tmp, L"%s.%lu.new", cur, GetCurrentProcessId());
    lstrcpyW(dir, cur);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return 0;
    slash[1] = 0;
    if (!path_join(second, MAX_PATH, dir, L"mxbfix.update.dlo") || !path_join(back, MAX_PATH, dir, L"mxbfix_data")) return 0;
    CreateDirectoryW(back, NULL);
    if (!path_join(back, MAX_PATH, dir, L"mxbfix_data\\mxbfix.previous")) return 0;

    HANDLE lock = CreateMutexW(NULL, FALSE, L"Local\\mxbfix-update");
    if (lock) WaitForSingleObject(lock, 10000);
    int ok = 0;
    if (file_equals(cur, file, n)) { ok = 1; log_msg("update install: already in place"); goto done; }

    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) { log_msg("update install: can't write next to the plugin (error %lu)", GetLastError()); goto done; }
    DWORD w = 0;
    BOOL wrote = WriteFile(h, file, n, &w, NULL) && w == n;
    CloseHandle(h);
    if (!wrote) { DeleteFileW(tmp); log_msg("update install: writing the new plugin failed"); goto done; }
    if (!MoveFileExW(tmp, second, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        log_msg("update install: can't put the new plugin in place (error %lu)", GetLastError());
        DeleteFileW(tmp);
        goto done;
    }
    if (!MoveFileExW(cur, back, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        log_msg("update install: can't move the current plugin (error %lu), keeping it", GetLastError());
        DeleteFileW(second);
        goto done;
    }
    /* From here the new plugin is what the game will load, whatever happens next. */
    ok = 1;
    if (!MoveFileExW(second, cur, MOVEFILE_WRITE_THROUGH))
        log_msg("update install: the new plugin keeps the name mxbfix.update.dlo (error %lu); it works the same", GetLastError());
done:
    if (lock) { ReleaseMutex(lock); CloseHandle(lock); }
    return ok;
}

static void begin_step(LONG step)
{
    s_step_start = GetTickCount64();
    InterlockedExchange(&s_step, step);
}

/* Keep the current step on screen for its minimum time. */
static void finish_step(void)
{
    ULONGLONG want = s_step_start + STEP_MIN_MS[s_step];
    while (!s_install_stop && GetTickCount64() < want) Sleep(20);
}

float update_install_progress(const char **label)
{
    LONG step = s_step;
    if (step < 0 || step > 2) step = 0;
    if (label) *label = STEP_LABEL[step];
    float f = (float)(GetTickCount64() - s_step_start) / (float)STEP_MIN_MS[step];
    if (f > 1.0f) f = 1.0f;
    if (step == STEP_DOWNLOAD && s_total > 0) {
        float real = (float)s_got / (float)s_total;       /* never ahead of the real download */
        if (real < f) f = real;
    }
    return STEP_FROM[step] + (STEP_FROM[step + 1] - STEP_FROM[step]) * f;
}

static DWORD WINAPI install_worker(LPVOID unused)
{
    (void)unused;
    static unsigned char sig[512];
    int ok = 0;
    unsigned char *file = (unsigned char *)VirtualAlloc(NULL, MAX_PLUGIN, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (file) {
        log_msg("update install: getting version %s", s_latest);
        begin_step(STEP_DOWNLOAD);
        DWORD n = get_asset("mxbfix.dlo", file, MAX_PLUGIN);
        DWORD sn = n ? get_asset("mxbfix.sig", sig, sizeof sig) : 0;
        finish_step();
        if (!n || !sn) log_msg("update install: download failed");
        else {
            begin_step(STEP_VERIFY);
            int valid = n >= 2 && file[0] == 'M' && file[1] == 'Z' && signature_ok(file, n, sig, sn);
            finish_step();
            if (!valid) log_msg("update install: the download failed its signature check, not installing");
            else if (!s_install_stop) {
                begin_step(STEP_INSTALL);
                ok = swap_in(file, n);
                finish_step();
                if (ok) log_msg("update install: version %s is in place, active at the next game start", s_latest);
            }
        }
        VirtualFree(file, 0, MEM_RELEASE);
    }
    InterlockedExchange(&s_install, ok ? INSTALL_READY : INSTALL_FAILED);
    return 0;
}

void update_install_start(void)
{
    if (s_state != UPDATE_AVAILABLE || s_install == INSTALL_WORKING || s_install == INSTALL_READY) return;
    s_got = s_total = 0;
    s_step = STEP_DOWNLOAD;
    s_step_start = GetTickCount64();
    InterlockedExchange(&s_install, INSTALL_WORKING);
    if (s_install_thread) { CloseHandle(s_install_thread); s_install_thread = NULL; }
    s_install_thread = CreateThread(NULL, 0, install_worker, NULL, 0, NULL);
    if (!s_install_thread) InterlockedExchange(&s_install, INSTALL_FAILED);
}

install_state_t update_install_state(void) { return (install_state_t)s_install; }

update_state_t update_state(void) { return (update_state_t)s_state; }
const char    *update_latest(void) { return s_latest; }
