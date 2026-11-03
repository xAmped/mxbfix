/*
 * Fix framework.
 *
 * A fix is one file in src/fixes/ that defines a fix_t and is listed in
 * fixes.c. A fix can do either or both of:
 *   - install():      patch something once at startup (e.g. redirect an allocation)
 *   - on_exception(): recognise one specific fault and repair the CPU context so
 *                     the game can carry on (called from a first-chance vectored
 *                     exception handler, before any other handler sees the fault)
 *
 * Rules:
 *   - Only act on a fault you can identify exactly: module, game build or code
 *     bytes, and the register state you expect. Otherwise return 0.
 *   - on_exception runs inside an exception: no heap, no locks, no logging.
 *     Bump self->saves and let fixes_poll() log it later.
 */
#pragma once
#include "common.h"

typedef struct fix fix_t;
struct fix {
    const char *id;                 /* stable id; also the [fixes] key in mxbfix.ini */
    const char *title;              /* one line, player-facing */
    const char *crash_id;           /* the known crash (known.h id) this fix addresses */
    int  (*install)(fix_t *self);   /* optional; return 1 if the fix is active */
    int  (*on_exception)(fix_t *self, EXCEPTION_POINTERS *ep);  /* optional; 1 = recovered */
    /* runtime state */
    int           enabled;          /* from config, default 1 */
    int           active;           /* installed and ready */
    volatile LONG saves;            /* crashes prevented this session */
    LONG          saves_logged;
    UINT          saves_total_base; /* lifetime total from stats.ini at startup (crash-safe to read) */
};

void   fixes_init(void);
LONG   fixes_dispatch(EXCEPTION_POINTERS *ep);   /* called by the vectored handler */
void   fixes_poll(void);                         /* game thread: log new saves, persist stats */
int    fixes_count(void);
void     ground_watch_saves(unsigned *total, unsigned *session);  /* fixes/fix_ground_nan_guard.c: crash values handled */
unsigned trainer_repairs_total(void);                             /* fixes/fix_trainer_repair.c: files repaired, lifetime */
fix_t *fixes_get(int i);

/* Helpers for fixes */
uint64_t game_rva(uint64_t addr);              /* address -> offset in mxbikes.exe, or ~0 */
int      game_code_matches(uint32_t rva, const char *pattern);

/* Apply a same-length code patch: accept already-patched bytes, refuse anything that isn't exactly
 * `orig`, then write `patch` with VirtualProtect + FlushInstructionCache. Logs the outcome under the
 * fix id. Returns 1 if the patch is in place. Game thread, at startup, before the code can run. */
int fix_apply_patch(const char *id, uint8_t *at, const uint8_t *orig, const uint8_t *patch, size_t len);

/* Several spans as one change, all or nothing: every span must read as `orig` (or all as `patch`: already
 * applied) before anything is touched; every span is made writable before any byte is written, and if one
 * can't be, the others are restored and nothing changes. Logs under the fix id. 1 = all spans patched. */
typedef struct {
    uint32_t       rva;
    uint32_t       len;          /* at most 64 */
    const char    *what;
    const uint8_t *orig, *patch;
} fix_span_t;
int fix_apply_patches(const char *id, uint8_t *base, const fix_span_t *spans, int n);

/* Original bytes of everything patched this session: puts them back into a copy of game memory. */
void fix_restore_originals(uint8_t *copy, uint64_t copy_base, size_t copy_len);

/* fixes/fix_trainer_series_lookup.c: log whether the loaded trainer's series name is junk */
void trainer_fix_note(const char *when);

/* fixes/fix_ground_nan_guard.c: passive log of rider slots whose position goes bad (game thread) */
void ground_watch_poll(void);
void ground_watch_summary(void);

/* fixes/fix_trainer_cleanup_reset.c: at Event Deinit (just before the trainer cleanup), log what it will do */
void trainer_cleanup_note(void);

