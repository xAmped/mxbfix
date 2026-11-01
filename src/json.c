#include "common.h"
#include "json.h"

static void put(jw_t *w, const char *s, size_t n)
{
    if (w->len + n + 1 > w->cap) { w->overflow = 1; return; }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = 0;
}

static void putc_(jw_t *w, char c) { put(w, &c, 1); }
static void puts_(jw_t *w, const char *s) { put(w, s, strlen(s)); }

size_t fmt_u64(char *out, uint64_t v)
{
    char tmp[24];
    size_t n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

size_t fmt_hex(char *out, uint64_t v)
{
    static const char hx[] = "0123456789abcdef";
    char tmp[17];
    size_t n = 0;
    do { tmp[n++] = hx[v & 15]; v >>= 4; } while (v);
    out[0] = '0';
    out[1] = 'x';
    for (size_t i = 0; i < n; i++) out[2 + i] = tmp[n - 1 - i];
    out[2 + n] = 0;
    return n + 2;
}

static void sep(jw_t *w, const char *key)
{
    if (w->depth > 0) {
        if (!w->first[w->depth]) putc_(w, ',');
        w->first[w->depth] = 0;
    }
    if (key) {
        putc_(w, '"');
        puts_(w, key);
        put(w, "\":", 2);
    }
}

void jw_init(jw_t *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = 0;
    w->depth = 0;
    w->first[0] = 1;
    if (cap) buf[0] = 0;
}

static void open_(jw_t *w, const char *key, char c)
{
    sep(w, key);
    putc_(w, c);
    if (w->depth < 47) w->depth++;
    w->first[w->depth] = 1;
}

void jw_obj(jw_t *w, const char *key) { open_(w, key, '{'); }
void jw_arr(jw_t *w, const char *key) { open_(w, key, '['); }
void jw_end_obj(jw_t *w) { if (w->depth > 0) w->depth--; putc_(w, '}'); }
void jw_end_arr(jw_t *w) { if (w->depth > 0) w->depth--; putc_(w, ']'); }

static void esc(jw_t *w, const char *s, size_t max)
{
    static const char hx[] = "0123456789abcdef";
    putc_(w, '"');
    for (size_t i = 0; i < max && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { putc_(w, '\\'); putc_(w, (char)c); }
        else if (c == '\n') put(w, "\\n", 2);
        else if (c == '\r') put(w, "\\r", 2);
        else if (c == '\t') put(w, "\\t", 2);
        else if (c < 0x20) {
            char u[6] = {'\\', 'u', '0', '0', hx[c >> 4], hx[c & 15]};
            put(w, u, 6);
        } else putc_(w, (char)c);
    }
    putc_(w, '"');
}

void jw_str(jw_t *w, const char *key, const char *s)
{
    sep(w, key);
    if (!s) { puts_(w, "null"); return; }
    esc(w, s, (size_t)-1);
}

void jw_strn(jw_t *w, const char *key, const char *s, size_t max)
{
    sep(w, key);
    esc(w, s, max);
}

void jw_wstr(jw_t *w, const char *key, const wchar_t *s)
{
    char u[MAX_PATH * 3];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, u, sizeof u, NULL, NULL);
    if (n <= 0) u[0] = 0;
    jw_str(w, key, u);
}

/* ASCII-only case-insensitive prefix test. Deliberately not _wcsnicmp: that is locale-aware and can take CRT
 * locks or allocate, which the crash handler must never do. */
static wchar_t lower_ascii(wchar_t c) { return (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c; }
static int prefix_ieq(const wchar_t *s, const wchar_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!s[i] || lower_ascii(s[i]) != lower_ascii(p[i])) return 0;
    return 1;
}

/* Paths in reports: the current profile becomes %USERPROFILE% (also after a \\?\ prefix, and for profiles
 * outside \Users\), and any other "\Users\<name>\" component becomes "\Users\<user>\". No heap, no CRT
 * locale functions: fixed buffers only. */
void jw_path(jw_t *w, const char *key, const wchar_t *path)
{
    wchar_t tmp[MAX_PATH + 48];           /* loop stops at MAX_PATH + 20; one replacement adds 13, plus NUL */
    size_t pl = wcslen(g_user_profile), n = 0;
    const wchar_t *src = path;
    while (pl && g_user_profile[pl - 1] == L'\\') pl--;
    if (prefix_ieq(src, L"\\\\?\\UNC\\", 8) && pl > 2 && prefix_ieq(g_user_profile, L"\\\\", 2) &&
        prefix_ieq(src + 8, g_user_profile + 2, pl - 2) && (src[8 + pl - 2] == L'\\' || src[8 + pl - 2] == 0)) {
        wmemcpy(tmp, L"%USERPROFILE%", 13);                     /* \\?\UNC\server\share\... = \\server\share\... */
        n = 13;
        src += 8 + pl - 2;
    } else {
        if (prefix_ieq(src, L"\\\\?\\", 4) && !prefix_ieq(src + 4, L"UNC\\", 4)) src += 4;   /* \\?\C:\... */
        if (pl && prefix_ieq(src, g_user_profile, pl) && (src[pl] == L'\\' || src[pl] == 0)) {
            wmemcpy(tmp, L"%USERPROFILE%", 13);
            n = 13;
            src += pl;
        }
    }
    while (*src && n < MAX_PATH + 20) {
        if (prefix_ieq(src, L"\\Users\\", 7)) {
            wmemcpy(tmp + n, L"\\Users\\<user>", 13);
            n += 13;
            src += 7;
            while (*src && *src != L'\\') src++;    /* skip the account name */
            continue;
        }
        tmp[n++] = *src++;
    }
    tmp[n] = 0;
    jw_wstr(w, key, tmp);
}

void jw_u64(jw_t *w, const char *key, uint64_t v)
{
    char t[24];
    sep(w, key);
    put(w, t, fmt_u64(t, v));
}

void jw_i64(jw_t *w, const char *key, int64_t v)
{
    char t[24];
    sep(w, key);
    if (v < 0) { putc_(w, '-'); v = -v; }
    put(w, t, fmt_u64(t, (uint64_t)v));
}

void jw_hex(jw_t *w, const char *key, uint64_t v)
{
    char t[24];
    sep(w, key);
    putc_(w, '"');
    put(w, t, fmt_hex(t, v));
    putc_(w, '"');
}

void jw_bool(jw_t *w, const char *key, int v) { sep(w, key); puts_(w, v ? "true" : "false"); }
void jw_null(jw_t *w, const char *key) { sep(w, key); puts_(w, "null"); }

void jw_f(jw_t *w, const char *key, double v, int dec)
{
    sep(w, key);
    if (v != v) { puts_(w, "\"nan\""); return; }
    if (v > 1e15 || v < -1e15) { puts_(w, v > 0 ? "\"inf\"" : "\"-inf\""); return; }
    if (v < 0) { putc_(w, '-'); v = -v; }
    uint64_t scale = 1;
    for (int i = 0; i < dec; i++) scale *= 10;
    uint64_t fixed = (uint64_t)(v * (double)scale + 0.5);
    char t[24];
    put(w, t, fmt_u64(t, fixed / scale));
    if (dec > 0) {
        putc_(w, '.');
        uint64_t frac = fixed % scale;
        char d[24];
        size_t n = fmt_u64(d, frac);
        for (size_t i = n; i < (size_t)dec; i++) putc_(w, '0');
        put(w, d, n);
    }
}

void jw_bytes(jw_t *w, const char *key, const uint8_t *p, size_t n)
{
    static const char hx[] = "0123456789abcdef";
    sep(w, key);
    putc_(w, '"');
    for (size_t i = 0; i < n; i++) {
        char b[2] = {hx[p[i] >> 4], hx[p[i] & 15]};
        put(w, b, 2);
    }
    putc_(w, '"');
}
