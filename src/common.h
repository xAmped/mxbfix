/*
 * Shared declarations for mxbfix.
 */
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define MXBFIX_VERSION "0.1.5"

/* ---- global paths and game identity (util.c) ---- */
extern wchar_t  g_save_dir[MAX_PATH];     /* the game's save path, Documents\PiBoSo\MX Bikes\ (trailing backslash) */
extern wchar_t  g_base_dir[MAX_PATH];     /* <save path>\mxbfix\  (trailing backslash) */
extern wchar_t  g_game_dir[MAX_PATH];     /* folder holding mxbikes.exe (trailing backslash) */
extern wchar_t  g_user_profile[MAX_PATH]; /* used to scrub user names out of paths */
extern DWORD    g_game_thread_id;         /* thread that calls plugin callbacks */
extern uint8_t *g_game_base;              /* mxbikes.exe image base in memory */
extern uint32_t g_game_size;              /* SizeOfImage */
extern uint32_t g_game_build;             /* PE TimeDateStamp: identifies the game build */

void util_init_paths(const char *save_path);
void util_init_game_identity(void);
/* Read an environment variable into buf; 1 only if it exists and fits (so buf is terminated). */
int  env_get(const char *name, char *buf, unsigned cap);
int  path_join(wchar_t *out, size_t cap, const wchar_t *dir, const wchar_t *name);   /* 0 (and "") if it doesn't fit */
void ensure_dir(const wchar_t *path);

/* Copy n bytes from addr without faulting. Returns 1 on success, 0 if unreadable. */
int safe_read(const void *addr, void *out, size_t n);

/* ANSI (game strings) -> UTF-8, always NUL terminated. */
void acp_to_utf8(const char *src, size_t src_max, char *dst, size_t cap);

/* UTC "YYYY-MM-DDTHH:MM:SSZ" into out (>= 21 bytes). */
void utc_now_iso(char *out);

/* ---- logging (util.c). Normal code paths only, never from the crash handler. ---- */
void log_init(void);
void log_msg(const char *fmt, ...);
void log_close(void);

/* ---- config (config.c): <base>\mxbfix.ini ---- */
void cfg_init(void);
int  cfg_int(const char *section, const char *key, int def);
