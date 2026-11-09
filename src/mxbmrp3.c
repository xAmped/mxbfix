#include "mxbmrp3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FT_PER_SEC 10000000ll
#define FT_UNIX    116444736000000000ull   /* 1970-01-01 as a FILETIME */

static uint64_t ft64(FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

/* module+offset as mxbmrp3 writes it ("mxbikes.exe+0x1a29ae"): plain module name characters, then +0x and hex. */
static int valid_loc(const char *s)
{
    const char *plus = strstr(s, "+0x");
    if (!plus || plus == s || plus - s > 80) return 0;
    for (const char *p = s; p < plus; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("_.-() ", *p))) return 0;
    const char *h = plus + 3;
    int n = 0;
    for (; *h; h++, n++)
        if (!((*h >= '0' && *h <= '9') || (*h >= 'a' && *h <= 'f') || (*h >= 'A' && *h <= 'F'))) return 0;
    return n >= 1 && n <= 16;
}

static int valid_code(const char *s)
{
    if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return 0;
    int n = 0;
    for (const char *h = s + 2; *h; h++, n++)
        if (!((*h >= '0' && *h <= '9') || (*h >= 'a' && *h <= 'f') || (*h >= 'A' && *h <= 'F'))) return 0;
    return n >= 1 && n <= 8;
}

/* Find `key` among the top-level members of a flat JSON object (string, number, true/false/null values only). The
 * whole object is checked first; anything else (nesting, a bad number, missing commas, text after the end) fails, so a
 * damaged or changed file gives nothing rather than something wrong. Strings are returned as written, escapes included
 * (callers' character checks refuse backslashes). Returns 1 for a string, 2 for a number, 0 if absent or malformed. */
static int flat_get(const char *j, const char *key, char *out, size_t cap)
{
    const char *p = j;
    int found = 0;
    size_t klen = strlen(key);
#define WS() while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++
    WS();
    if (*p++ != '{') return 0;
    WS();
    if (*p == '}') { p++; goto tail; }
    for (;;) {
        WS();
        if (*p++ != '"') return 0;
        const char *k = p;
        while (*p && *p != '"') { if (*p == '\\') { if (!p[1]) return 0; p++; } p++; }
        if (*p != '"') return 0;
        int match = (size_t)(p - k) == klen && !memcmp(k, key, klen);
        p++;
        WS();
        if (*p++ != ':') return 0;
        WS();
        const char *v = p;
        int kind;
        if (*p == '"') {
            p++;
            v = p;
            while (*p && *p != '"') { if (*p == '\\') { if (!p[1]) return 0; p++; } p++; }
            if (*p != '"') return 0;
            kind = 1;
        } else if (*p >= '0' && *p <= '9') {
            while (*p >= '0' && *p <= '9') p++;
            if (p - v > 19) return 0;                    /* fits in uint64 without wrapping */
            kind = 2;
        } else if (!strncmp(p, "true", 4) || !strncmp(p, "null", 4)) { p += 4; kind = 3; }
        else if (!strncmp(p, "false", 5)) { p += 5; kind = 3; }
        else return 0;
        if (match) {
            if (found) return 0;                         /* the same key twice: ambiguous */
            size_t n = (size_t)(p - v);
            if (kind == 3 || n + 1 > cap) return 0;
            memcpy(out, v, n);
            out[n] = 0;
            found = kind;
        }
        if (kind == 1) p++;                              /* closing quote */
        WS();
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
        return 0;
    }
tail:
    WS();
#undef WS
    return *p ? 0 : found;
}

static int json_str(const char *j, const char *key, char *out, size_t cap) { return flat_get(j, key, out, cap) == 1; }

static int json_u64(const char *j, const char *key, uint64_t *out)
{
    char num[24];
    if (flat_get(j, key, num, sizeof num) != 2) return 0;
    uint64_t v = 0;
    for (const char *p = num; *p; p++) v = v * 10 + (uint64_t)(*p - '0');
    *out = v;
    return 1;
}

/* "signature":"..." from one of our own reports (nested, so not flat): the first top-level occurrence our writer puts
 * right after the header. */
static int report_signature(const char *j, char *out, size_t cap)
{
    const char *p = strstr(j, "\"signature\":\"");
    if (!p) return 0;
    p += 13;
    size_t n = 0;
    for (; *p && *p != '"'; p++) {
        if (*p == '\\' || n + 1 >= cap) return 0;
        out[n++] = *p;
    }
    out[n] = 0;
    return *p == '"' && n > 0;
}

static char *read_small(const wchar_t *path, size_t max, size_t *len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    char *buf = NULL;
    DWORD got = 0;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && (uint64_t)sz.QuadPart <= max && (buf = malloc((size_t)sz.QuadPart + 1)) &&
        ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) && got == (DWORD)sz.QuadPart) {
        buf[got] = 0;
        *len = got;
    } else {
        free(buf);
        buf = NULL;
    }
    CloseHandle(h);
    return buf;
}

int mxbmrp3_find(unsigned long pid, uint64_t created, mxbmrp3_match_t *m)
{
    memset(m, 0, sizeof *m);
    if (!pid || !created) return 0;
    wchar_t dir[MAX_PATH], pat[MAX_PATH], name[64];
    if (!path_join(dir, MAX_PATH, g_save_dir, L"mxbmrp3\\crashes\\")) return 0;
    wsprintfW(name, L"mxbmrp3_crash_*_%lu.log", pid);
    if (!path_join(pat, MAX_PATH, dir, name)) return 0;
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(pat, &d);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        uint64_t t = ft64(d.ftLastWriteTime);
        if (t < created || (m->found && t < m->log_time) || lstrlenW(d.cFileName) >= (int)(sizeof m->name / sizeof m->name[0])) continue;
        m->found = 1;
        m->log_time = t;
        lstrcpyW(m->name, d.cFileName);
    } while (FindNextFileW(h, &d));
    FindClose(h);
    if (!m->found) return 0;

    wchar_t dump[MAX_PATH];
    if (path_join(dump, MAX_PATH, dir, m->name)) {
        int n = lstrlenW(dump);
        if (n > 4) { lstrcpyW(dump + n - 4, L".dmp"); m->dump = GetFileAttributesW(dump) != INVALID_FILE_ATTRIBUTES; }
    }

    /* The newest crash's details, if they are this crash's: its time (Unix seconds) matches the log's. */
    wchar_t pend[MAX_PATH];
    size_t len = 0;
    char *j = path_join(pend, MAX_PATH, g_save_dir, L"mxbmrp3\\pending_crash.json") ? read_small(pend, 64 * 1024, &len) : NULL;
    uint64_t t = 0;
    if (j && json_u64(j, "time", &t)) {
        int64_t log_unix = (int64_t)((m->log_time - FT_UNIX) / FT_PER_SEC);
        int64_t diff = (int64_t)t - log_unix;
        if (diff >= -10 && diff <= 10 && json_str(j, "fault", m->fault, sizeof m->fault) && valid_loc(m->fault)) {
            m->details = 1;
            if (!json_str(j, "code", m->code, sizeof m->code) || !valid_code(m->code)) m->code[0] = 0;
            char stack[2048];
            if (json_str(j, "stack", stack, sizeof stack)) {
                for (char *tok = stack; *tok && m->nstack < 16;) {
                    char *sp = strchr(tok, ' ');
                    if (sp) *sp = 0;
                    if (*tok && strlen(tok) < sizeof m->stack[0] && valid_loc(tok)) strcpy(m->stack[m->nstack++], tok);
                    if (!sp) break;
                    tok = sp + 1;
                }
            }
        }
    }
    free(j);
    return 1;
}

void mxbmrp3_write(jw_t *w, const mxbmrp3_match_t *m, uint64_t ours, const char *our_signature, int with_stack)
{
    jw_obj(w, "mxbmrp3");
    jw_bool(w, "crash_log", m->found);
    jw_bool(w, "dump", m->dump);
    if (ours) jw_i64(w, "seconds_from_ours", ((int64_t)m->log_time - (int64_t)ours) / FT_PER_SEC);
    if (m->details) {
        jw_str(w, "fault", m->fault);
        if (m->code[0]) jw_str(w, "code", m->code);
        if (our_signature) jw_bool(w, "same_fault", !lstrcmpiA(m->fault, our_signature));
        if (with_stack && m->nstack) {
            jw_arr(w, "stack");
            for (int i = 0; i < m->nstack; i++) jw_str(w, NULL, m->stack[i]);
            jw_end_arr(w);
        }
    }
    jw_end_obj(w);
}

/* Replace a report's contents, keeping its last-write time: the uploader sends only reports written after the player
 * agreed (upload.c), so a rewrite must not make an older report look newer. */
static int write_replace(const wchar_t *path, const char *data, size_t len, FILETIME written)
{
    wchar_t tmp[MAX_PATH];
    if (lstrlenW(path) + 5 >= MAX_PATH) return 0;
    lstrcpyW(tmp, path);
    lstrcatW(tmp, L".tmp");
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, data, (DWORD)len, &wr, NULL) && wr == len && SetFileTime(h, NULL, NULL, &written);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp); return 0; }
    return 1;
}

void mxbmrp3_annotate_reports(unsigned long pid, uint64_t created, const mxbmrp3_match_t *m)
{
    if (!m->found) return;
    wchar_t dir[MAX_PATH], pat[MAX_PATH], name[32], path[MAX_PATH];
    if (!path_join(dir, MAX_PATH, g_base_dir, L"reports\\pending\\")) return;
    wsprintfW(name, L"*-%lu.json", pid);
    if (!path_join(pat, MAX_PATH, dir, name)) return;
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(pat, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        uint64_t ours = ft64(d.ftLastWriteTime);
        if (ours < created || !path_join(path, MAX_PATH, dir, d.cFileName)) continue;
        size_t len = 0;
        char *j = read_small(path, 256 * 1024, &len);
        if (!j) continue;
        size_t end = len;
        while (end && (j[end - 1] == ' ' || j[end - 1] == '\r' || j[end - 1] == '\n' || j[end - 1] == '\t')) end--;
        char sig[128];
        if (end < 2 || j[end - 1] != '}' || strstr(j, "\"mxbmrp3\"") || !report_signature(j, sig, sizeof sig)) { free(j); continue; }
        char add[4096];
        jw_t w;
        jw_init(&w, add, sizeof add);
        jw_obj(&w, NULL);
        mxbmrp3_write(&w, m, ours, sig, 0);
        jw_end_obj(&w);
        char *out = w.overflow || w.len < 2 ? NULL : malloc(end + w.len + 1);
        if (out) {
            /* {...existing...} + ,"mxbmrp3":{...} + } */
            memcpy(out, j, end - 1);
            size_t n = end - 1;
            out[n++] = ',';
            memcpy(out + n, add + 1, w.len - 1);       /* drop our wrapper's '{'; its '}' closes the report */
            n += w.len - 1;
            if (write_replace(path, out, n, d.ftLastWriteTime))
                log_msg("report %ls matches mxbmrp3's crash log %ls%s%s%s", d.cFileName, m->name, m->details ? " (fault " : "",
                        m->details ? m->fault : "", m->details ? ")" : "");
            free(out);
        }
        free(j);
    } while (FindNextFileW(h, &d));
    FindClose(h);
}
