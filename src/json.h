/*
 * Minimal JSON writer into a caller-provided buffer.
 *
 * It never allocates and never calls the C runtime's formatting functions, so it
 * is safe to use from the crash handler, where the heap may be corrupt.
 * Keys are written by passing a key name; pass NULL inside arrays.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

typedef struct {
    char  *buf;
    size_t len, cap;
    int    overflow;
    int    depth;
    int    first[48];
} jw_t;

void jw_init(jw_t *w, char *buf, size_t cap);
void jw_obj(jw_t *w, const char *key);        /* begin object */
void jw_arr(jw_t *w, const char *key);        /* begin array */
void jw_end_obj(jw_t *w);
void jw_end_arr(jw_t *w);
void jw_str(jw_t *w, const char *key, const char *utf8);
void jw_strn(jw_t *w, const char *key, const char *utf8, size_t max);
void jw_wstr(jw_t *w, const char *key, const wchar_t *s);
void jw_path(jw_t *w, const char *key, const wchar_t *path);   /* scrubs the user profile */
void jw_u64(jw_t *w, const char *key, uint64_t v);
void jw_i64(jw_t *w, const char *key, int64_t v);
void jw_hex(jw_t *w, const char *key, uint64_t v);           /* "0x1a2b" as a string */
void jw_bool(jw_t *w, const char *key, int v);
void jw_null(jw_t *w, const char *key);
void jw_f(jw_t *w, const char *key, double v, int decimals);  /* NaN/Inf -> string "nan"/"inf" */
void jw_bytes(jw_t *w, const char *key, const uint8_t *p, size_t n);  /* hex string */

/* Raw text helpers usable without the JSON structure. */
size_t fmt_hex(char *out, uint64_t v);     /* "0x..." , returns length */
size_t fmt_u64(char *out, uint64_t v);
