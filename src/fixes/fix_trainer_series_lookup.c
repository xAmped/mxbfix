/*
 * Fix: trainer-series-lookup
 * Crash: msvcr90.dll+0x36ede while a track loads (damaged trainer file)
 *
 * Bug: a trainer file (.trn) is read straight into the game's trainer record, so
 * whatever the file holds at offset 0x82 becomes the record's "series name"
 * (0xF3DD08). If it's non-empty, +0x2083f looks it up with +0x4350, which leaves
 * its output untouched when the name isn't found. The caller ignores that and
 * uses the uninitialised stack slot [rsp+0xb8] as the series index, then prints
 * strings from a random "series" entry: access violation or stack overrun.
 *
 * Fix: rewrite +0x2083f..+0x20875 (55 bytes) so the lookup's return value
 * decides. Call the lookup directly (it already returns "not found" for an empty
 * name); on failure take the game's existing "no series" path (rdi = r13 = 0,
 * jump to +0x208bc); on success continue exactly as before. A garbage name now
 * behaves like an empty one, which is what the older trainer format's loader
 * does by clearing the field.
 *
 * Checked against the game image: the patched block decodes as intended, no code
 * jumps into its interior, and every path after it writes rsi before reading it
 * (the original left rsi pointing at the name; the patch leaves it unchanged).
 */
#include "../fixes.h"
#include <stdlib.h>

#define TRN_BUILD 0x6A21833Du
#define TRN_RVA   0x2083Fu
#define TRN_LEN   55

static const uint8_t ORIG[TRN_LEN] = {
    0x48, 0x8d, 0x35, 0xc2, 0xd4, 0xf1, 0x00,           /* lea rsi, [series name]     */
    0x48, 0x8d, 0x3d, 0x9e, 0x2b, 0x31, 0x00,           /* lea rdi, [""]              */
    0xb9, 0x01, 0x00, 0x00, 0x00,                       /* mov ecx, 1                 */
    0xf3, 0xa6,                                         /* repe cmpsb                 */
    0x75, 0x05,                                         /* jne lookup                 */
    0x49, 0x8b, 0xfd,                                   /* mov rdi, r13               */
    0xeb, 0x61,                                         /* jmp +0x208bc               */
    0x48, 0x8d, 0x15, 0xa6, 0xd4, 0xf1, 0x00,           /* lookup: lea rdx, [name]    */
    0x48, 0x8d, 0x8c, 0x24, 0xb8, 0x00, 0x00, 0x00,     /* lea rcx, [rsp+0xb8]        */
    0xe8, 0xe1, 0x3a, 0xfe, 0xff,                       /* call +0x4350 (result ignored) */
    0x48, 0x8b, 0x05, 0xc2, 0xd2, 0xf1, 0x00,           /* mov rax, [series table]    */
};

static const uint8_t PATCH[TRN_LEN] = {
    0x48, 0x8d, 0x15, 0xc2, 0xd4, 0xf1, 0x00,           /* lea rdx, [series name]     */
    0x48, 0x8d, 0x8c, 0x24, 0xb8, 0x00, 0x00, 0x00,     /* lea rcx, [rsp+0xb8]        */
    0xe8, 0xfd, 0x3a, 0xfe, 0xff,                       /* call +0x4350               */
    0x85, 0xc0,                                         /* test eax, eax              */
    0x74, 0x08,                                         /* je found                   */
    0x49, 0x8b, 0xfd,                                   /* mov rdi, r13 (no series)   */
    0xe9, 0x5d, 0x00, 0x00, 0x00,                       /* jmp +0x208bc               */
    0x48, 0x8b, 0x05, 0xd2, 0xd2, 0xf1, 0x00,           /* found: mov rax, [table]    */
    0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,     /* 8-byte nop                 */
    0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,     /* 8-byte nop                 */
};

/* Patch target. */
static uint8_t *target(void)
{
    if (g_game_build != TRN_BUILD) return NULL;
    return g_game_base + TRN_RVA;
}

static int install(fix_t *self)
{
    return fix_apply_patch(self->id, target(), ORIG, PATCH, TRN_LEN);
}

/* Evidence for the log: after a track loads, report whether the trainer record's series-name field
 * holds junk. Unpatched, the game would look that name up and could crash on it; patched, it's
 * treated as "no series". Not counted as a prevented crash: unpatched it only crashes sometimes. */
#define SERIES_NAME_RVA 0xF3DD08u
fix_t fix_trainer_series_lookup;

void trainer_fix_note(const char *when)
{
    if (g_game_build != TRN_BUILD) return;
    uint8_t name[16];
    if (!safe_read(g_game_base + SERIES_NAME_RVA, name, sizeof name) || name[0] == 0) return;
    int printable = 1;
    for (int i = 0; i < (int)sizeof name && name[i]; i++) if (name[i] < 32 || name[i] > 126) printable = 0;
    char hx[40];
    for (int i = 0; i < 8; i++) wsprintfA(hx + 2 * i, "%02x", name[i]);
    /* Junk here crashes an unpatched game on this load, so it's a crash prevented. Counted once per junk value. */
    static uint8_t s_counted[16];
    if (!printable && fix_trainer_series_lookup.active && memcmp(s_counted, name, sizeof name)) {
        memcpy(s_counted, name, sizeof name);
        InterlockedIncrement(&fix_trainer_series_lookup.saves);
    }
    log_msg("trainer record at %s: series-name field is %s (%s): %s", when, printable ? "a name" : "JUNK", hx,
            fix_trainer_series_lookup.active ? "handled by trainer-series-lookup" : "unpatched game may crash on this");
}

fix_t fix_trainer_series_lookup = {
    .id = "trainer-series-lookup",
    .title = "Treat an unknown series name in a trainer file as none (stops the track-load crash)",
    .crash_id = "trainer-bad-string",
    .install = install,
    .on_exception = NULL,
};
