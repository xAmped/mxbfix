/*
 * Fix: trainer-cleanup-reset
 * Crash: mxbikes.exe+0x11d753, cancelling a server join after riding offline (~7% of community crash reports)
 *
 * Bug: unloading an event (+0x71d50, the game's command 44) runs the trainer
 * (ghost) cleanup +0x21500. It runs only while the "trainer loaded" flag
 * +0xF3DB54 is set, destroys the trainer's objects by number (request 0x38b ->
 * +0x11d730, a 10-entry table at +0xE4B380), and never clears that flag or the
 * stored numbers. Online events don't load a trainer, so after riding offline
 * the flag stays set; cancelling a server join unloads again, the cleanup runs
 * a second time, and +0x11d730, which has no "already destroyed?" check, reads
 * address 0x10. Reproduced on demand: ride offline, join a server, Cancel while
 * it loads.
 *
 * Fix, two same-length patches, applied all or nothing:
 *  1. +0x21504 (19 bytes): the cleanup's opening "is a trainer loaded?" test
 *     reads the flag and clears it in one `xchg`. Not loaded: returns 1 exactly
 *     as before. Loaded: cleans up as before, and a second run finds nothing to
 *     do. Every other reader of the flag uses it as "trainer loaded? else skip",
 *     and the trainer save (+0x21010) runs before the cleanup, so saving is
 *     unchanged. This also stops a stale cleanup from destroying a different,
 *     live object after the table reuses an entry.
 *  2. +0x11d74f (30 bytes): safety net in "destroy object n": if the entry is
 *     already empty, skip the two frees (`jrcxz`) and store 0 as before.
 * Checked against the game image: both decode as intended, nothing branches
 * into either span, and register/flag use after each span is unchanged.
 *
 * Can't count prevented crashes (no hook at the crash moment). trainer_cleanup_note(),
 * called at each Event Deinit (which the game sends just before the cleanup),
 * logs when an unpatched game would crash, and when the fix very likely saved one.
 */
#include "../fixes.h"
#include "../dev.h"
#include <stdlib.h>

#define TCR_BUILD 0x6A21833Du

typedef struct {
    uint32_t rva;
    uint8_t len;
    const char *what;
    uint8_t orig[30];
    uint8_t patch[30];
} tcr_site_t;

static const tcr_site_t SITES[] = {
    {0x21504, 19, "trainer cleanup start",
     {0x83, 0x3d, 0x49, 0xc6, 0xf1, 0x00, 0x00,        /* cmp dword ptr [+0xF3DB54], 0 */
      0x75, 0x0a,                                      /* jne +0x21517                 */
      0xb8, 0x01, 0x00, 0x00, 0x00,                    /* mov eax, 1                   */
      0x48, 0x83, 0xc4, 0x28,                          /* add rsp, 0x28                */
      0xc3},                                           /* ret                          */
     {0x31, 0xc0,                                      /* xor eax, eax                 */
      0x87, 0x05, 0x48, 0xc6, 0xf1, 0x00,              /* xchg [+0xF3DB54], eax        */
      0x85, 0xc0,                                      /* test eax, eax                */
      0x75, 0x07,                                      /* jne +0x21517                 */
      0xff, 0xc0,                                      /* inc eax (= 1)                */
      0x48, 0x83, 0xc4, 0x28,                          /* add rsp, 0x28                */
      0xc3}},                                          /* ret                          */
    {0x11D74F, 30, "destroy object (null check)",
     {0x48, 0x8b, 0x0c, 0xdf,                          /* mov rcx, [rdi+rbx*8]         */
      0x48, 0x8b, 0x49, 0x10,                          /* mov rcx, [rcx+0x10] (crash)  */
      0xe8, 0xe4, 0xdd, 0x03, 0x00,                    /* call +0x15b540 (free)        */
      0x48, 0x8b, 0x0c, 0xdf,                          /* mov rcx, [rdi+rbx*8]         */
      0xe8, 0xdb, 0xdd, 0x03, 0x00,                    /* call +0x15b540               */
      0x48, 0xc7, 0x04, 0xdf, 0x00, 0x00, 0x00, 0x00}, /* mov qword [rdi+rbx*8], 0     */
     {0x48, 0x8b, 0x0c, 0xdf,                          /* mov rcx, [rdi+rbx*8]         */
      0xe3, 0x12,                                      /* jrcxz +0x11d767 (empty)      */
      0x48, 0x8b, 0x49, 0x10,                          /* mov rcx, [rcx+0x10]          */
      0xe8, 0xe2, 0xdd, 0x03, 0x00,                    /* call +0x15b540               */
      0x48, 0x8b, 0x0c, 0xdf,                          /* mov rcx, [rdi+rbx*8]         */
      0xe8, 0xd9, 0xdd, 0x03, 0x00,                    /* call +0x15b540               */
      0x48, 0x83, 0x24, 0xdf, 0x00,                    /* and qword [rdi+rbx*8], 0     */
      0x90}},                                          /* nop                          */
};
#define NSITES ((int)(sizeof SITES / sizeof SITES[0]))

/* The game (or the test harness's stand-in in the developer build). Looked up once. */
static uint8_t *image_base(void)
{
    static uint8_t *s_cached;
    static int s_done;
    if (s_done) return s_cached;
    s_done = 1;
    s_cached = test_target("GAME_IMAGE");
    if (!s_cached && g_game_build == TCR_BUILD)
        s_cached = g_game_base;
    return s_cached;
}

static int install(fix_t *self)
{
    fix_span_t spans[NSITES];
    for (int i = 0; i < NSITES; i++)
        spans[i] = (fix_span_t){SITES[i].rva, SITES[i].len, SITES[i].what, SITES[i].orig, SITES[i].patch};
    return fix_apply_patches(self->id, image_base(), spans, NSITES);   /* all or nothing */
}

fix_t fix_trainer_cleanup_reset = {
    .id = "trainer-cleanup-reset",
    .title = "Reset the trainer cleanup after it runs (stops the crash when cancelling a server join after riding offline)",
    .crash_id = "session-teardown-null",
    .install = install,
    .on_exception = NULL,
};

/*
 * Event Deinit is sent at the start of the unload, just before the trainer cleanup runs. Read the state it
 * will use (read-only) and log what an unpatched game would do. Objects the cleanup destroys, in order:
 * [+0x109E0A4] if [+0xF3DB68] != 0, [+0xF3E244], [+0xF3DB4C], then [entry+4] for each of 10 entries at
 * +0x106BB40 (stride 0x5054) whose first dword is set. Object n (1..10) is entry n-1 of the table at
 * +0xE4B380; 0 there is the crash.
 */
#define TRN_FLAG    0xF3DB54u
#define TRN_FLAG2   0xF3DB68u
#define OBJ_TABLE   0xE4B380u

static int s_cleanup_ran;                 /* the flag was set at an earlier unload this launch */

static int empty_object(uint8_t *base, uint32_t number_rva, int *n_out)
{
    int n = 0;
    uint64_t entry = 1;
    if (!safe_read(base + number_rva, &n, 4) || n < 1 || n > 10) return 0;
    if (!safe_read(base + OBJ_TABLE + (uint32_t)(n - 1) * 8, &entry, 8)) return 0;
    *n_out = n;
    return entry == 0;
}

static int stale_object(uint8_t *base, int check_flag2, int *n)
{
    int flag2 = 0;
    if (check_flag2 && safe_read(base + TRN_FLAG2, &flag2, 4) && flag2 && empty_object(base, 0x109E0A4u, n)) return 1;
    if (empty_object(base, 0xF3E244u, n) || empty_object(base, 0xF3DB4Cu, n)) return 1;
    for (int i = 0; i < 10; i++) {
        uint32_t e = 0x106BB40u + (uint32_t)i * 0x5054u, used = 0;
        if (safe_read(base + e, &used, 4) && used && empty_object(base, e + 4, n)) return 1;
    }
    return 0;
}

void trainer_cleanup_note(void)
{
    uint8_t *base = image_base();
    int flag = 0, n = 0;
    if (!base || !safe_read(base + TRN_FLAG, &flag, 4)) return;
    if (flag) {
        if (stale_object(base, 1, &n))
            log_msg("trainer cleanup is about to destroy trainer object %d, which is already gone: %s", n,
                    fix_trainer_cleanup_reset.active ? "the fix's null check covers it (unexpected: the reset should have cleared the flag)"
                                                     : "EXPECT A CRASH (trainer-cleanup-reset not active)");
        s_cleanup_ran = 1;
    } else if (s_cleanup_ran && fix_trainer_cleanup_reset.active && stale_object(base, 0, &n)) {
        /* An unpatched game would still have the flag set here (no trainer loaded since the last cleanup)
         * unless one of its own reset paths cleared it, hence "very likely". */
        log_msg("trainer cleanup skipped: no trainer loaded since the last cleanup and object %d is gone; an unpatched game would very likely crash here (trainer-cleanup-reset covers it)", n);
        s_cleanup_ran = 0;           /* once per episode: an unpatched game would have crashed at this first one */
        InterlockedIncrement(&fix_trainer_cleanup_reset.saves);
    }
}
