#include "common.h"
#include "upload.h"
#include "ui.h"
#include <bcrypt.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RELAY_HOST L"mxbfix-reports.jay-b9d.workers.dev"
#define MAX_REPORT (128 * 1024)
#define MAX_COUNT_SEND 100
#define MAX_PER_START 10

static HANDLE s_thread;
static volatile LONG s_stop;
static char s_install_id[33];

static const char *const COUNT_KEYS[4] = {"gate-vertex-pointer", "trainer-series-lookup", "trainer-cleanup-reset", "ground-nan-guard"};

/* ---- settings: mxbfix.ini for the choice, stats.ini for the running totals ---- */

static int ini_path(wchar_t *out, const wchar_t *name) { return path_join(out, MAX_PATH, g_base_dir, name); }

static void load_install_id(void)
{
    wchar_t ini[MAX_PATH], val[40];
    if (!ini_path(ini, L"mxbfix.ini")) return;
    GetPrivateProfileStringW(L"reports", L"install_id", L"", val, 40, ini);
    if (lstrlenW(val) == 32) {
        WideCharToMultiByte(CP_ACP, 0, val, -1, s_install_id, sizeof s_install_id, NULL, NULL);
        return;
    }
    uint8_t r[16];
    if (BCryptGenRandom(NULL, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return;
    for (int i = 0; i < 16; i++) wsprintfA(s_install_id + 2 * i, "%02x", r[i]);
    MultiByteToWideChar(CP_ACP, 0, s_install_id, -1, val, 40);
    WritePrivateProfileStringW(L"reports", L"install_id", val, ini);
}

/* Lifetime total for one crash type, as the page counts it. */
static unsigned count_total(int i)
{
    wchar_t ini[MAX_PATH], key[48];
    if (!ini_path(ini, L"stats.ini")) return 0;
    if (i == 3) return GetPrivateProfileIntW(L"bad_positions", L"would_crash", 0, ini);
    MultiByteToWideChar(CP_ACP, 0, COUNT_KEYS[i], -1, key, 48);
    return GetPrivateProfileIntW(L"saves", key, 0, ini);
}

static void set_sent_baseline(int i, unsigned v)
{
    wchar_t ini[MAX_PATH], key[48], val[16];
    if (!ini_path(ini, L"stats.ini")) return;
    MultiByteToWideChar(CP_ACP, 0, COUNT_KEYS[i], -1, key, 48);
    wsprintfW(val, L"%u", v);
    WritePrivateProfileStringW(L"sent_counts", key, val, ini);
}

static unsigned now_seconds(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (unsigned)((((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000000ull) - 11644473600ull);
}

void upload_mark_consent(void)
{
    cfg_set_int("reports", "since", (int)now_seconds());
    for (int i = 0; i < 4; i++) set_sent_baseline(i, count_total(i));
}

/* ---- what leaves the PC ---- */

/* Anything that looks like a file path, as JSON text: C:\\... or \\\\server. Last check before a report leaves. */
static int has_path(const char *s, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++) {
        if (s[i] == ':' && s[i + 1] == '\\' && s[i + 2] == '\\' && i && ((s[i - 1] | 32) >= 'a' && (s[i - 1] | 32) <= 'z')) return 1;
        if (s[i] == '"' && s[i + 1] == '\\' && s[i + 2] == '\\' && s[i + 3] == '\\') return 1;
    }
    return 0;
}

/* The report without any "path" member, and with the install ID added. Works on the plugin's own JSON. */
static size_t build_payload(const char *in, size_t n, char *out, size_t cap)
{
    size_t o = 0, i = 0;
    if (n < 2 || in[0] != '{' || cap < n + 64) return 0;
    o = (size_t)sprintf(out, "{\"install_id\":\"%s\",", s_install_id);
    i = 1;
    while (i < n) {
        char c = in[i];
        if (c != '"') { out[o++] = c; i++; continue; }
        size_t e = i + 1;                                  /* find the end of this string */
        while (e < n && in[e] != '"') e += in[e] == '\\' ? 2 : 1;
        if (e >= n) return 0;
        size_t colon = e + 1;
        while (colon < n && (in[colon] == ' ' || in[colon] == '\t' || in[colon] == '\r' || in[colon] == '\n')) colon++;
        size_t q = colon + 1;
        while (q < n && (in[q] == ' ' || in[q] == '\t' || in[q] == '\r' || in[q] == '\n')) q++;
        if (e - i == 5 && !memcmp(in + i, "\"path\"", 6) && q < n && in[colon] == ':' && in[q] == '"') {
            size_t v = q + 1;                              /* skip the value and one comma */
            while (v < n && in[v] != '"') v += in[v] == '\\' ? 2 : 1;
            if (v >= n) return 0;
            i = v + 1;
            if (i < n && in[i] == ',') i++;
            else {
                while (o && (out[o - 1] == ' ' || out[o - 1] == '\t' || out[o - 1] == '\r' || out[o - 1] == '\n')) o--;
                if (o && out[o - 1] == ',') o--;
            }
            continue;
        }
        memcpy(out + o, in + i, e - i + 1);
        o += e - i + 1;
        i = e + 1;
    }
    return o;
}

/* ---- sending ---- */

/* HTTP status, 0 if it couldn't be sent. */
static DWORD post(const wchar_t *path, const char *body, size_t len)
{
    DWORD status = 0, size = sizeof status;
    wchar_t ua[48];
    wsprintfW(ua, L"mxbfix/%hs", MXBFIX_VERSION);
    HINTERNET ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return 0;
    WinHttpSetTimeouts(ses, 8000, 8000, 15000, 15000);
    HINTERNET con = WinHttpConnect(ses, RELAY_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"POST", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (req && WinHttpSendRequest(req, L"Content-Type: application/json\r\n", (DWORD)-1, (LPVOID)body, (DWORD)len, (DWORD)len, 0) &&
        WinHttpReceiveResponse(req, NULL))
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return status;
}

static void send_reports(void)
{
    static char raw[MAX_REPORT], payload[MAX_REPORT + 128];
    wchar_t pat[MAX_PATH], dir[MAX_PATH], sent[MAX_PATH], rejected[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
    WIN32_FIND_DATAW d;
    uint64_t since = (uint64_t)(unsigned)cfg_int("reports", "since", 0);
    int tried = 0, ok = 0;
    if (!since) { cfg_set_int("reports", "since", (int)now_seconds()); return; }   /* first start since the choice: nothing older */
    if (!path_join(dir, MAX_PATH, g_base_dir, L"reports\\pending\\") || !path_join(pat, MAX_PATH, dir, L"*.json") ||
        !path_join(sent, MAX_PATH, g_base_dir, L"reports\\sent\\") || !path_join(rejected, MAX_PATH, g_base_dir, L"reports\\rejected\\")) return;
    ensure_dir(sent);
    ensure_dir(rejected);
    HANDLE h = FindFirstFileW(pat, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        uint64_t t = (((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32 | d.ftLastWriteTime.dwLowDateTime) / 10000000ull) - 11644473600ull;
        if (t < since || d.nFileSizeHigh || d.nFileSizeLow >= MAX_REPORT) continue;
        if (!ui_reports_allowed()) break;                 /* the box can be unticked at any moment */
        if (!path_join(from, MAX_PATH, dir, d.cFileName)) continue;
        HANDLE f = CreateFileW(from, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;
        DWORD got = 0;
        BOOL read = ReadFile(f, raw, sizeof raw, &got, NULL);
        CloseHandle(f);
        size_t len = read ? build_payload(raw, got, payload, sizeof payload) : 0;
        if (!len) continue;
        DWORD status = 0;
        if (has_path(payload, len)) {
            log_msg("crash reports: not sending %ls, it still contains a file path", d.cFileName);
            status = 400;
        } else {
            tried++;
            status = post(L"/v1/report", payload, len);
        }
        /* Accepted, or refused for good: don't send it again. Anything else tries again next start. */
        if (status >= 200 && status < 300 && path_join(to, MAX_PATH, sent, d.cFileName)) {
            MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
            ok++;
        } else if (status == 400 && path_join(to, MAX_PATH, rejected, d.cFileName)) {
            MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
        }
    } while (tried < MAX_PER_START && !s_stop && FindNextFileW(h, &d));
    FindClose(h);
    if (tried) log_msg("crash reports: sent %d of %d", ok, tried);
}

static void send_counts(void)
{
    wchar_t ini[MAX_PATH], key[48], val[16];
    SYSTEMTIME st;
    GetSystemTime(&st);
    int day = st.wYear * 10000 + st.wMonth * 100 + st.wDay;
    if (cfg_int("reports", "counts_day", 0) == day || !ini_path(ini, L"stats.ini")) return;
    char body[320];
    int n = snprintf(body, sizeof body, "{\"schema\":1,\"counts\":{");
    unsigned total[4] = {0, 0, 0, 0};
    int any = 0;
    unsigned had[4];
    for (int i = 0; i < 4; i++) {
        total[i] = count_total(i);
        had[i] = 0;
        MultiByteToWideChar(CP_ACP, 0, COUNT_KEYS[i], -1, key, 48);
        GetPrivateProfileStringW(L"sent_counts", key, L"", val, 16, ini);
        if (!val[0]) { set_sent_baseline(i, total[i]); continue; }    /* first time: count from here on */
        unsigned base = (unsigned)_wtoi(val);
        if (total[i] <= base) continue;
        unsigned send = total[i] - base;
        if (send > MAX_COUNT_SEND) send = MAX_COUNT_SEND;  /* the rest goes in later sends */
        n += snprintf(body + n, sizeof body - (size_t)n, "%s\"%s\":%u", any ? "," : "", COUNT_KEYS[i], send);
        had[i] = base + send;                              /* what the baseline moves to once this is accepted */
        any = 1;
    }
    cfg_set_int("reports", "counts_day", day);
    if (!any) return;
    n += snprintf(body + n, sizeof body - (size_t)n, "}}");
    DWORD status = post(L"/v1/counts", body, (size_t)n);
    if (status >= 200 && status < 300) {
        for (int i = 0; i < 4; i++) if (had[i]) set_sent_baseline(i, had[i]);
        log_msg("crash counts: sent");
    } else {
        cfg_set_int("reports", "counts_day", 0);          /* try again next start */
    }
}

static DWORD WINAPI worker(LPVOID unused)
{
    (void)unused;
    load_install_id();
    if (!s_install_id[0]) return 0;
    send_reports();
    if (!s_stop && ui_reports_allowed()) send_counts();
    return 0;
}

void upload_start(void)
{
    if (!s_thread) s_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
}

void upload_stop(void)
{
    if (!s_thread) return;
    s_stop = 1;     /* A request in progress ends by itself (it has timeouts). Don't wait long: pin the DLL instead. */
    if (WaitForSingleObject(s_thread, 2000) == WAIT_TIMEOUT) {
        HMODULE self;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)(void *)upload_stop, &self);
        log_msg("crash report upload still running at shutdown; plugin pinned in memory until exit");
    }
    CloseHandle(s_thread);
    s_thread = NULL;
}
