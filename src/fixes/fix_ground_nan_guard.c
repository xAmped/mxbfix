/*
 * Fix: ground-nan-guard
 * Crash: mxbikes.exe+0x1f1923 while racing (~17% of community crash reports)
 *
 * Bug: three terrain lookups test "is this point outside the map" with
 * `comiss min, x` + `ja exit` (exit if min > x), and the same for the far
 * edge. A comparison with NaN is unordered, so `ja` isn't taken and a NaN
 * position is treated as inside the map:
 *   +0x1f1720  height grid:  the NaN becomes index 0x80000000 and the grid read
 *              lands gigabytes away (the crash at +0x1f1923);
 *   +0x1f11a0  triangle box tree: a NaN visits every box and the triangle test
 *              records false hits, so the ground and track queries report
 *              "found" with NaN results;
 *   +0x1f1d10  surface lookup: same checks, same kind of bad grid index; two
 *              callers reach it without going through the height lookup.
 * The NaN comes from other riders' positions holding junk bytes, or from the
 * local bike's own physics (crash doc, steps 1b to 1d).
 *
 * Fix: in each lookup, flip the "below the map" check for x and for z so it
 * reads `comiss x, min` + `jb exit` (exit if x < min, or if x is NaN). For
 * every ordinary number the result is identical, including equality, -0/+0
 * and infinities; a NaN now takes the lookup's own "off the map" exit, the same
 * path a position past the map edge takes. This stops the bad grid read; it
 * doesn't repair the broken rider: some callers then use an unwritten output
 * (+0x8fd9) or keep applying forces to a NaN bike (+0x1a8297), as they would for
 * any off-map position (crash doc, step 1d). Two bytes change per check (operand order in
 * the ModRM/REX byte, and ja -> jb); lengths and jump targets are unchanged.
 * Checked against the game image: each exit restores exactly the registers
 * saved before the checks, and nothing branches into the patched spans.
 *
 * Six spans, one per check, applied with fix_apply_patches(): all are verified
 * and made writable before any byte changes; if one differs or can't be made
 * writable, nothing is patched. This fix can't count prevented crashes (there is no hook at the
 * crash moment); the passive check below logs every rider whose position goes
 * bad instead, which is the evidence it worked.
 */
#include "../fixes.h"
#include "../dev.h"
#include "../context.h"
#include <stdio.h>
#include <stdlib.h>

#define GNG_BUILD 0x6A21833Du

typedef struct {
    uint32_t rva;
    uint8_t len;
    const char *what;
    uint8_t orig[27];
    uint8_t patch[27];
} site_t;

/* Only the bytes shown change; the rest of each span is copied as context and verified. */
static const site_t SITES[] = {
    {0x1F1765, 9, "height grid, x below map",
     {0x0f, 0x2f, 0xcb, 0x0f, 0x87, 0x81, 0x05, 0x00, 0x00},           /* comiss xmm1,xmm3 ; ja +0x1f1cef */
     {0x0f, 0x2f, 0xd9, 0x0f, 0x82, 0x81, 0x05, 0x00, 0x00}},          /* comiss xmm3,xmm1 ; jb +0x1f1cef */
    {0x1F1798, 10, "height grid, z below map",
     {0x41, 0x0f, 0x2f, 0xd0, 0x0f, 0x87, 0x4d, 0x05, 0x00, 0x00},     /* comiss xmm2,xmm8 ; ja */
     {0x44, 0x0f, 0x2f, 0xc2, 0x0f, 0x82, 0x4d, 0x05, 0x00, 0x00}},    /* comiss xmm8,xmm2 ; jb */
    {0x1F11D7, 10, "triangle box tree, x below box",
     {0x41, 0x0f, 0x2f, 0xc0, 0x0f, 0x87, 0xbe, 0x02, 0x00, 0x00},     /* comiss xmm0,xmm8 ; ja +0x1f149f */
     {0x44, 0x0f, 0x2f, 0xc0, 0x0f, 0x82, 0xbe, 0x02, 0x00, 0x00}},    /* comiss xmm8,xmm0 ; jb */
    {0x1F11F3, 9, "triangle box tree, z below box",
     {0x0f, 0x2f, 0xc7, 0x0f, 0x87, 0xa3, 0x02, 0x00, 0x00},           /* comiss xmm0,xmm7 ; ja */
     {0x0f, 0x2f, 0xf8, 0x0f, 0x82, 0xa3, 0x02, 0x00, 0x00}},          /* comiss xmm7,xmm0 ; jb */
    /* One span so the compare and its jump can never be changed separately (three flag-neutral movaps
     * sit between them; flipping one without the other would break ordinary lookups). */
    {0x1F1D62, 27, "surface lookup, x below map",
     {0x45, 0x0f, 0x2f, 0xd6,                                          /* comiss xmm10,xmm14 */
      0x44, 0x0f, 0x29, 0x64, 0x24, 0x30,                              /* movaps [rsp+30h],xmm12 */
      0x44, 0x0f, 0x29, 0x6c, 0x24, 0x20,                              /* movaps [rsp+20h],xmm13 */
      0x44, 0x0f, 0x29, 0x3c, 0x24,                                    /* movaps [rsp],xmm15 */
      0x0f, 0x87, 0x3d, 0x06, 0x00, 0x00},                             /* ja +0x1f23ba */
     {0x45, 0x0f, 0x2f, 0xf2,                                          /* comiss xmm14,xmm10 */
      0x44, 0x0f, 0x29, 0x64, 0x24, 0x30,
      0x44, 0x0f, 0x29, 0x6c, 0x24, 0x20,
      0x44, 0x0f, 0x29, 0x3c, 0x24,
      0x0f, 0x82, 0x3d, 0x06, 0x00, 0x00}},                            /* jb +0x1f23ba */
    {0x1F1DAC, 10, "surface lookup, z below map",
     {0x45, 0x0f, 0x2f, 0xdf, 0x0f, 0x87, 0x04, 0x06, 0x00, 0x00},     /* comiss xmm11,xmm15 ; ja */
     {0x45, 0x0f, 0x2f, 0xfb, 0x0f, 0x82, 0x04, 0x06, 0x00, 0x00}},    /* comiss xmm15,xmm11 ; jb */
};
#define NSITES ((int)(sizeof SITES / sizeof SITES[0]))

/* Base of the image to patch and read: the game (or the test harness's stand-in in the developer build).
 * Looked up once (the scan below runs every frame). */
static uint8_t *image_base(void)
{
    static uint8_t *s_cached;
    static int s_done;
    if (s_done) return s_cached;
    s_done = 1;
    s_cached = test_target("GAME_IMAGE");
    if (!s_cached && g_game_build == GNG_BUILD)
        s_cached = g_game_base;
    return s_cached;
}

static int install(fix_t *self)
{
    fix_span_t spans[NSITES];
    for (int i = 0; i < NSITES; i++)
        spans[i] = (fix_span_t){SITES[i].rva, SITES[i].len, SITES[i].what, SITES[i].orig, SITES[i].patch};
    return fix_apply_patches(self->id, image_base(), spans, NSITES);
}

fix_t fix_ground_nan_guard = {
    .id = "ground-nan-guard",
    .title = "Treat a broken (NaN) position as off the map in the ground lookups (stops the +0x1f1923 crash)",
    .crash_id = "physics-contact-nan",
    .install = install,
    .on_exception = NULL,
};

/*
 * Passive bad-position log (read-only, game thread). Scans the 50 rider slots
 * the same way the game's track-position loop does and logs any rider whose
 * position becomes NaN, infinite or absurdly large, and when it recovers.
 * Race numbers go to the local log only, never into reports.
 *
 * Because these events are rare and mxbfix.log only keeps two sessions, each event is also appended to
 * bad_positions.log (never wiped; appending stops at 1 MB) and counted in stats.ini [bad_positions]
 * (total, would_crash).
 *
 * Rider table +0xF4EE20, 50 slots of 0x5b24 bytes: +0x0 in use, +0xc race number,
 * +0x50b0 and +0x50c4 further in-use flags (the loop checks all three), position
 * at +0x50d4/+0x50e4/+0x50f4 (matrix translation). +0xe62710 = local slot + 1.
 */
#define SLOT_TABLE  0xF4EE20u
#define SLOT_SIZE   0x5b24u
#define SLOT_COUNT  50
#define LOCAL_SLOT1 0xE62710u
#define LOG_CAP     200                      /* lines per session for ordinary bad positions */
#define CRASH_EXTRA 200                      /* crash values still listed after that, per session */

static int      s_watch = -1;                  /* -1 not decided, 0 off, 1 on */
static uint8_t  s_bad[SLOT_COUNT];
static uint8_t  s_listed[SLOT_COUNT];         /* this episode's start was logged, so log its end too */
static uint64_t s_bad_since[SLOT_COUNT];
static int      s_lines, s_crash_extra, s_unlisted, s_episodes, s_max_in_use, s_seen_riders;
static uint64_t s_scans;

#define RECORD_MAX (1024 * 1024)

/* Append one line to bad_positions.log with the local date, time and track. Game thread, rare events. */
static void record(const char *msg)
{
    wchar_t path[MAX_PATH];
    path_join(path, MAX_PATH, g_base_dir, L"bad_positions.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size) && size.QuadPart < RECORD_MAX) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char line[640];
        int n = snprintf(line, sizeof line, "%04u-%02u-%02u %02u:%02u:%02u  %s  %s\r\n", t.wYear, t.wMonth, t.wDay,
                         t.wHour, t.wMinute, t.wSecond, g_ctx.track_name[0] ? g_ctx.track_name : "-", msg);
        if (n > (int)sizeof line - 1) n = (int)sizeof line - 1;
        DWORD w;
        if (n > 0) WriteFile(h, line, (DWORD)n, &w, NULL);
    }
    CloseHandle(h);
}

/* Lifetime counts in stats.ini. Added in memory and written in batches, so a flickering position can't cause
 * a file write every frame: written right away while under the log cap, the rest at shutdown. */
static unsigned s_pending_total, s_pending_crash;
static unsigned s_crash_base, s_crash_session;

static void flush_counts(void)
{
    if (!s_pending_total && !s_pending_crash) return;
    wchar_t ini[MAX_PATH], val[24];
    if (!path_join(ini, MAX_PATH, g_base_dir, L"stats.ini")) return;
    wsprintfW(val, L"%u", GetPrivateProfileIntW(L"bad_positions", L"total", 0, ini) + s_pending_total);
    WritePrivateProfileStringW(L"bad_positions", L"total", val, ini);
    wsprintfW(val, L"%u", GetPrivateProfileIntW(L"bad_positions", L"would_crash", 0, ini) + s_pending_crash);
    WritePrivateProfileStringW(L"bad_positions", L"would_crash", val, ini);
    s_pending_total = s_pending_crash = 0;
}

static int f_nan(uint32_t b) { return (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu); }
static int f_inf(uint32_t b) { return (b & 0x7FFFFFFFu) == 0x7F800000u; }
static int f_huge(uint32_t b) { return (b & 0x7FFFFFFFu) > 0x4B189680u && !f_nan(b) && !f_inf(b); }   /* |v| > 1e7 */

static uint32_t rd32(const uint8_t *p) { return *(const volatile uint32_t *)p; }

void ground_watch_poll(void)
{
    if (s_watch < 0) {
        /* The table is static data inside the game image (always mapped); check it once, then read it
         * directly: this runs every frame, and safe_read is a system call. */
        uint8_t *base = image_base();
        uint32_t probe;
        uint32_t last = SLOT_TABLE + (SLOT_COUNT - 1) * SLOT_SIZE + 0x50f4;
        s_watch = base && cfg_int("capture", "log_bad_positions", 1) && safe_read(base + SLOT_TABLE, &probe, 4) &&
                  safe_read(base + last, &probe, 4) && safe_read(base + LOCAL_SLOT1, &probe, 4) ? 1 : 0;
        wchar_t ini[MAX_PATH];
        if (path_join(ini, MAX_PATH, g_base_dir, L"stats.ini")) s_crash_base = GetPrivateProfileIntW(L"bad_positions", L"would_crash", 0, ini);
    }
    if (!s_watch) return;
    uint8_t *base = image_base();
    int local1 = (int)rd32(base + LOCAL_SLOT1);
    int in_use_count = 0;
    s_scans++;
    for (int i = 0; i < SLOT_COUNT; i++) {
        const uint8_t *slot = base + SLOT_TABLE + (uint32_t)i * SLOT_SIZE;
        uint32_t pos[3] = {rd32(slot + 0x50d4), rd32(slot + 0x50e4), rd32(slot + 0x50f4)};
        int in_use = rd32(slot) && rd32(slot + 0x50b0) && rd32(slot + 0x50c4);
        in_use_count += in_use;
        int bad = in_use && (f_nan(pos[0]) || f_nan(pos[1]) || f_nan(pos[2]) || f_inf(pos[0]) || f_inf(pos[1]) ||
                             f_inf(pos[2]) || f_huge(pos[0]) || f_huge(pos[1]) || f_huge(pos[2]));
        if (bad && !s_bad[i]) {
            s_bad[i] = 1;
            s_bad_since[i] = GetTickCount64();
            s_episodes++;
            int crashes = f_nan(pos[0]) || f_nan(pos[2]);       /* x or z NaN: what reaches +0x1f1923 unguarded */
            s_pending_total++;
            if (crashes) { s_pending_crash++; if (fix_ground_nan_guard.active) s_crash_session++; }
            /* Ordinary episodes stop being listed at the cap (one flickering rider can make thousands); crash values
             * keep their own allowance, because they're the rare events worth a timestamp. Both are bounded. */
            int over = s_lines >= LOG_CAP;
            int list = !over || (crashes && s_crash_extra < CRASH_EXTRA);
            s_listed[i] = (uint8_t)list;
            if (!list) s_unlisted++;
            else {
                if (over) s_crash_extra++; else s_lines++;
                flush_counts();
                char msg[400];
                snprintf(msg, sizeof msg, "bad rider position: slot %d (race %d, %s) x=%08x y=%08x z=%08x%s", i,
                         (int)rd32(slot + 0xc), i + 1 == local1 ? "YOU" : "another rider", pos[0], pos[1], pos[2],
                         !crashes ? "" : fix_ground_nan_guard.active ? ": an unguarded game crashes on this; ground-nan-guard covers it"
                                                                     : ": EXPECT A CRASH (ground-nan-guard not active)");
                log_msg("%s", msg);
                record(msg);
            }
        } else if (!bad && s_bad[i]) {
            s_bad[i] = 0;
            if (s_listed[i]) {
                if (s_lines < LOG_CAP) s_lines++;
                char msg[200], how[80];
                if (in_use) lstrcpyA(how, "recovered");
                else snprintf(how, sizeof how, "left (slot no longer in use: flags %x/%x/%x)", rd32(slot), rd32(slot + 0x50b0),
                              rd32(slot + 0x50c4));
                snprintf(msg, sizeof msg, "bad rider position: slot %d %s after %llu ms", i, how,
                         (unsigned long long)(GetTickCount64() - s_bad_since[i]));
                log_msg("%s", msg);
                record(msg);
            }
        }
    }
    if (in_use_count > s_max_in_use) s_max_in_use = in_use_count;
    if (in_use_count && !s_seen_riders) {          /* proof the check runs and the table layout holds */
        s_seen_riders = 1;
        int ls = local1 - 1;
        int lrace = ls >= 0 && ls < SLOT_COUNT ? (int)rd32(base + SLOT_TABLE + (uint32_t)ls * SLOT_SIZE + 0xc) : -1;
        log_msg("bad-position check running: %d rider(s) in use, you are slot %d (race %d)", in_use_count, ls, lrace);
    }
}

void ground_watch_saves(unsigned *total, unsigned *session)
{
    *total = s_crash_base + s_crash_session;
    *session = s_crash_session;
}

void ground_watch_summary(void)
{
    if (s_watch != 1) return;
    flush_counts();
    log_msg("bad-position check: %llu scans, up to %d rider(s) in use at once, %d bad position(s)",
            (unsigned long long)s_scans, s_max_in_use, s_episodes);
    if (!s_episodes) return;
    char msg[120];
    if (s_unlisted) snprintf(msg, sizeof msg, "bad rider positions this session: %d (%d not listed, log limit reached)", s_episodes, s_unlisted);
    else snprintf(msg, sizeof msg, "bad rider positions this session: %d", s_episodes);
    log_msg("%s", msg);
    record(msg);
}
