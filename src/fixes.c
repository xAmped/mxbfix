#include "fixes.h"
#include "known.h"
#include <wchar.h>
#include <stdlib.h>
#include <string.h>

/* ---- registry: add new fixes here ---- */
extern fix_t fix_gate_vertex_pointer;
extern fix_t fix_trainer_series_lookup;
extern fix_t fix_trainer_repair;
extern fix_t fix_ground_nan_guard;
extern fix_t fix_trainer_cleanup_reset;


static fix_t *ALL[] = {
    &fix_gate_vertex_pointer,
    &fix_trainer_series_lookup,
    &fix_trainer_repair,
    &fix_ground_nan_guard,
    &fix_trainer_cleanup_reset,
};
#define NFIX ((int)(sizeof ALL / sizeof ALL[0]))

int    fixes_count(void) { return NFIX; }
fix_t *fixes_get(int i) { return (i >= 0 && i < NFIX) ? ALL[i] : NULL; }

uint64_t game_rva(uint64_t addr)
{
    uint64_t base = (uint64_t)g_game_base;
    return (addr >= base && addr < base + g_game_size) ? addr - base : ~0ull;
}

int game_code_matches(uint32_t rva, const char *pattern)
{
    uint8_t code[64];
    size_t n = 0;
    for (const char *p = pattern; *p; p++) if (*p != ' ') n++;
    n /= 2;
    if (n > sizeof code || rva + n > g_game_size) return 0;
    if (!safe_read(g_game_base + rva, code, n)) return 0;
    return bytes_match(code, n, pattern);
}


int fix_apply_patch(const char *id, uint8_t *p, const uint8_t *orig, const uint8_t *patch, size_t len)
{
    uint8_t cur[256];
    if (!p || len > sizeof cur || !safe_read(p, cur, len)) return 0;
    if (!memcmp(cur, patch, len)) {                  /* already patched */
        return 1;
    }
    if (memcmp(cur, orig, len)) {
        log_msg("fix %s: game code differs from what the fix was written for; not applied", id);
        return 0;
    }
    DWORD old, tmp;
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old)) {
        log_msg("fix %s: could not make the code writable (error %lu); not applied", id, GetLastError());
        return 0;
    }
    memcpy(p, patch, len);
    BOOL restored = VirtualProtect(p, len, old, &tmp);
    BOOL flushed = FlushInstructionCache(GetCurrentProcess(), p, len);
    log_msg("fix %s: patched %u bytes at %p%s%s", id, (unsigned)len, (void *)p,
            restored ? "" : " (warning: could not restore memory protection)",
            flushed ? "" : " (warning: instruction cache flush failed)");
    return 1;                                                    /* the bytes are patched either way */
}


int fix_apply_patches(const char *id, uint8_t *base, const fix_span_t *sp, int n)
{
    enum { MAXS = 16 };
    DWORD old[MAXS];
    if (!base || n < 1 || n > MAXS) return 0;
    int applied = 0;
    for (int i = 0; i < n; i++) {                                /* 1. verify everything first */
        uint8_t cur[64];
        if (sp[i].len > sizeof cur || !safe_read(base + sp[i].rva, cur, sp[i].len)) return 0;
        if (!memcmp(cur, sp[i].patch, sp[i].len)) applied++;
        else if (memcmp(cur, sp[i].orig, sp[i].len)) {
            log_msg("fix %s: game code differs at +0x%x (%s); nothing patched", id, sp[i].rva, sp[i].what);
            return 0;
        }
    }
    if (applied == n) {                                          /* already in place */
        return 1;
    }
    if (applied) {
        log_msg("fix %s: only part of the change is in place; nothing patched", id);
        return 0;
    }
    for (int i = 0; i < n; i++) {                                /* 2. make every span writable */
        if (!VirtualProtect(base + sp[i].rva, sp[i].len, PAGE_EXECUTE_READWRITE, &old[i])) {
            DWORD err = GetLastError(), tmp;
            /* Undo in reverse: spans can share a page, and a later span's "old" protection is the earlier
             * span's writable one; only unwinding backwards ends at the true original. */
            for (int k = i - 1; k >= 0; k--) VirtualProtect(base + sp[k].rva, sp[k].len, old[k], &tmp);
            log_msg("fix %s: could not make the code writable at +0x%x (error %lu); nothing patched", id, sp[i].rva, err);
            return 0;
        }
    }
    for (int i = 0; i < n; i++) {                                /* 3. write everything */
        memcpy(base + sp[i].rva, sp[i].patch, sp[i].len);
    }
    BOOL restored[MAXS];
    for (int i = n - 1; i >= 0; i--) {                           /* 4. restore protection, in reverse (shared pages) */
        DWORD tmp;
        restored[i] = VirtualProtect(base + sp[i].rva, sp[i].len, old[i], &tmp);
    }
    for (int i = 0; i < n; i++) {                                /* 5. flush, log */
        BOOL flushed = FlushInstructionCache(GetCurrentProcess(), base + sp[i].rva, sp[i].len);
        log_msg("fix %s: patched %u bytes at %p%s%s", id, sp[i].len, (void *)(base + sp[i].rva),
                restored[i] ? "" : " (warning: could not restore memory protection)",
                flushed ? "" : " (warning: instruction cache flush failed)");
    }
    return 1;
}

void fixes_init(void)
{
    wchar_t ini[MAX_PATH];
    path_join(ini, MAX_PATH, g_base_dir, L"stats.ini");
    for (int i = 0; i < NFIX; i++) {
        fix_t *f = ALL[i];
        wchar_t key[64];
        MultiByteToWideChar(CP_UTF8, 0, f->id, -1, key, 64);
        f->saves_total_base = GetPrivateProfileIntW(L"saves", key, 0, ini);
        f->enabled = cfg_int("fixes", f->id, 1);
        f->active = 0;
        if (!f->enabled) { log_msg("fix %s: disabled in mxbfix.ini", f->id); continue; }
        f->active = f->install ? f->install(f) : (f->on_exception != NULL);
        log_msg("fix %s: %s", f->id, f->active ? "active" : "not active (game code did not match or not needed)");
    }
}

LONG fixes_dispatch(EXCEPTION_POINTERS *ep)
{
    for (int i = 0; i < NFIX; i++) {
        fix_t *f = ALL[i];
        if (!f->active || !f->on_exception) continue;
        if (f->on_exception(f, ep)) {
            InterlockedIncrement(&f->saves);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void fixes_poll(void)
{
    for (int i = 0; i < NFIX; i++) {
        fix_t *f = ALL[i];
        LONG s = f->saves;
        if (s == f->saves_logged) continue;
        LONG delta = s - f->saves_logged;
        f->saves_logged = s;
        log_msg("fix %s prevented %ld crash(es) (session total %ld)", f->id, delta, s);

        /* lifetime totals in stats.ini, reported with every crash report */
        wchar_t ini[MAX_PATH], key[64], val[24];
        path_join(ini, MAX_PATH, g_base_dir, L"stats.ini");
        MultiByteToWideChar(CP_UTF8, 0, f->id, -1, key, 64);
        UINT total = GetPrivateProfileIntW(L"saves", key, 0, ini) + (UINT)delta;
        wsprintfW(val, L"%u", total);
        WritePrivateProfileStringW(L"saves", key, val, ini);
    }
}
