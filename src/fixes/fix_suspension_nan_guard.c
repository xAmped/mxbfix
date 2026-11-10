/*
 * suspension-nan-guard: a physics blow-up of the bike (crash at mxbikes.exe+0x1a29ae), when the bike's physics has already gone bad.
 *
 * The rear suspension update (+0x1a29e0) looks up a ratio in a 100-entry table with the leaf function +0x1a2970:
 *
 *     comiss 0, x ; jb  ...      x < 0 -> first entry       (NaN falls through)
 *     comiss x, 1 ; jae ...      x >= 1 -> last entry        (NaN falls through)
 *     cvttss2si edx, x*99        NaN -> 0x80000000
 *     cmp edx, 99 ; jne ...      0x80000000 isn't 99
 *     movss xmm2, [rcx + rax*4 + 0x4fe8]     <- +0x1a29ae: the read lands 8 GB below the bike object
 *
 * x is NaN when the bike's state is already NaN, from any cause: a fence contact with a NaN depth (fence-contact-nan
 * stops that one) or a hard hit from another rider that blew the bike's physics up.
 *
 * This fix changes no game code and nothing about how the bike rides. It acts only at the moment the game would crash,
 * and only on exactly this fault: the instruction bytes, the NaN-made index in edx/rax, a NaN x in xmm3, the one caller's
 * return address and its bike object in both rcx and rbx. It then does what the function does for an out-of-range x:
 * returns the table's last entry (+0x5174) to the caller. The bike is already broken at that point; what the game
 * does with it next (its own crash reset, or more guarded ticks) is logged by suspension_guard_poll().
 *
 * A broken bike can hit this every 2 ms physics tick, so saves are counted once per episode, not per tick. An episode ends
 * only when the game thread sees the bike healthy again (no guarded tick for a second while riding, with a finite
 * position in telemetry), so a pause or a stall doesn't split one broken bike into several saves. Only the game thread
 * is handled (the crash reports show the physics tick there); anywhere else the crash goes ahead as before.
 */
#include "../fixes.h"
#include "../context.h"
#include <stdio.h>
#include <string.h>

#define SNG_BUILD   0x6A21833Du
#define SNG_FUNC    0x1A2970u           /* the table lookup, a leaf function with no stack frame */
#define SNG_FAULT   0x1A29AEu           /* movss xmm2, [rcx+rax*4+0x4fe8] */
#define SNG_RET     0x1A2E84u           /* return address of its only call (+0x1a2e7f, in the suspension update) */
#define SNG_LAST    0x5174u             /* table entry 99, what the function returns for x >= 1 */
#define EPISODE_GAP 1000u               /* ms without a recovery, while riding, before the bike can count as healthy */

/* The whole function, +0x1a2970 .. +0x1a29d3, checked once at startup. */
static const uint8_t FUNC[0x64] = {
    0x0f, 0x57, 0xc0,                                       /* xorps   xmm0, xmm0                  */
    0x0f, 0x28, 0xd9,                                       /* movaps  xmm3, xmm1                  */
    0x0f, 0x2f, 0xc3,                                       /* comiss  xmm0, xmm3                  */
    0x72, 0x09,                                             /* jb      +0x1a2984                   */
    0xf3, 0x0f, 0x10, 0x81, 0xe8, 0x4f, 0x00, 0x00,         /* movss   xmm0, [rcx+0x4fe8]          */
    0xc3,                                                   /* ret                                 */
    0x0f, 0x2f, 0x1d, 0x7d, 0x0e, 0x1b, 0x00,               /* comiss  xmm3, [rip+...] (1.0)       */
    0x73, 0x11,                                             /* jae     +0x1a299e                   */
    0xf3, 0x0f, 0x59, 0x1d, 0xdb, 0x69, 0x18, 0x00,         /* mulss   xmm3, [rip+...] (99.0)      */
    0xf3, 0x0f, 0x2c, 0xd3,                                 /* cvttss2si edx, xmm3                 */
    0x83, 0xfa, 0x63,                                       /* cmp     edx, 0x63                   */
    0x75, 0x09,                                             /* jne     +0x1a29a7                   */
    0xf3, 0x0f, 0x10, 0x81, 0x74, 0x51, 0x00, 0x00,         /* movss   xmm0, [rcx+0x5174]          */
    0xc3,                                                   /* ret                                 */
    0x48, 0x63, 0xc2,                                       /* movsxd  rax, edx                    */
    0x66, 0x0f, 0x6e, 0xca,                                 /* movd    xmm1, edx                   */
    0xf3, 0x0f, 0x10, 0x94, 0x81, 0xe8, 0x4f, 0x00, 0x00,   /* movss   xmm2, [rcx+rax*4+0x4fe8] <- */
    0xf3, 0x0f, 0x10, 0x84, 0x81, 0xec, 0x4f, 0x00, 0x00,   /* movss   xmm0, [rcx+rax*4+0x4fec]    */
    0xf3, 0x0f, 0x5c, 0xc2,                                 /* subss   xmm0, xmm2                  */
    0x0f, 0x5b, 0xc9,                                       /* cvtdq2ps xmm1, xmm1                 */
    0xf3, 0x0f, 0x5c, 0xd9,                                 /* subss   xmm3, xmm1                  */
    0xf3, 0x0f, 0x59, 0xc3,                                 /* mulss   xmm0, xmm3                  */
    0xf3, 0x0f, 0x58, 0xc2,                                 /* addss   xmm0, xmm2                  */
    0xc3,                                                   /* ret                                 */
};
/* The caller's bytes around the call: mov rcx, rbx ... call +0x1a2970 (checked so rbx really is the bike object). */
static const uint8_t CALLER[0x16] = {
    0x48, 0x8b, 0xcb,                                       /* +0x1a2e6e mov rcx, rbx              */
    0x66, 0x41, 0x0f, 0x28, 0xc1,                           /* movapd  xmm0, xmm9                  */
    0xf2, 0x41, 0x0f, 0x5c, 0xc5,                           /* subsd   xmm0, xmm13                 */
    0xf2, 0x0f, 0x5a, 0xc8,                                 /* cvtsd2ss xmm1, xmm0                 */
    0xe8, 0xec, 0xfa, 0xff, 0xff,                           /* call    +0x1a2970                   */
};

static uint8_t *s_base;                 /* image the checks ran against (the game, or the test host's copy) */

/* Episode state, game thread only: written by the handler (no locks, no logging there) and by suspension_guard_poll. */
static volatile LONG64 s_ep_start, s_ep_last;   /* GetTickCount64 of the episode's first and latest recovery (0: none) */
static volatile LONG   s_ep_ticks;              /* recoveries this episode */
static volatile LONG   s_ticks_total;           /* recoveries this session */

static uint8_t *image_base(void)
{
    return g_game_build == SNG_BUILD ? g_game_base : NULL;
}

static int install(fix_t *self)
{
    uint8_t *base = image_base();
    uint8_t cur[sizeof FUNC];
    if (!base) return 0;
    if (!safe_read(base + SNG_FUNC, cur, sizeof FUNC) || memcmp(cur, FUNC, sizeof FUNC) ||
        !safe_read(base + SNG_RET - sizeof CALLER, cur, sizeof CALLER) || memcmp(cur, CALLER, sizeof CALLER)) {
        log_msg("fix %s: game code differs from what the fix was written for; not active", self->id);
        return 0;
    }
    s_base = base;
    return 1;
}

static int f32_nan(uint32_t b) { return (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu); }
static int f32_bad(uint32_t b) { return (b & 0x7F800000u) == 0x7F800000u; }        /* NaN or infinity */

static int on_exception(fix_t *self, EXCEPTION_POINTERS *ep)
{
    (void)self;
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    uint8_t *base = s_base;
    if (!base || r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || GetCurrentThreadId() != g_game_thread_id) return 0;
    if (r->NumberParameters < 2 || r->ExceptionInformation[0] != 0) return 0;          /* a read */
    if (c->Rip != (DWORD64)(uintptr_t)(base + SNG_FAULT)) return 0;
    if ((c->ContextFlags & (CONTEXT_INTEGER | CONTEXT_CONTROL | CONTEXT_FLOATING_POINT)) !=
        (CONTEXT_INTEGER | CONTEXT_CONTROL | CONTEXT_FLOATING_POINT)) return 0;
    /* The index came from a NaN: edx = 0x80000000, rax = its sign extension, and x (xmm3) is NaN. */
    if ((uint32_t)c->Rdx != 0x80000000u || c->Rax != 0xFFFFFFFF80000000ull) return 0;
    if (!f32_nan((uint32_t)c->Xmm3.Low)) return 0;
    /* The fault address is what that index gives (rcx - 8 GB + 0x4fe8); Windows reports -1 only for a non-canonical one. */
    DWORD64 want = c->Rcx + 0x4fe8 - 0x200000000ull;
    int noncanonical = (want >> 47) != 0 && (want >> 47) != 0x1FFFF;
    if (r->ExceptionInformation[1] != want && !(noncanonical && r->ExceptionInformation[1] == ~0ull)) return 0;
    /* Called from the suspension update, with its bike object (rbx) passed in rcx. */
    uint64_t ret = 0;
    if (!safe_read((void *)(uintptr_t)c->Rsp, &ret, 8) || ret != (uint64_t)(uintptr_t)(base + SNG_RET)) return 0;
    if (c->Rcx != c->Rbx) return 0;
    uint32_t last;
    if (!safe_read((void *)(uintptr_t)(c->Rcx + SNG_LAST), &last, 4)) return 0;

    /* Return the last table entry, as the function does for x >= 1 (movss zeroes the rest of xmm0). */
    c->Xmm0.Low = last;
    c->Xmm0.High = 0;
    c->Rip = ret;
    c->Rsp += 8;

    /* Game thread only, like suspension_guard_poll: no other writer. */
    LONG64 now = (LONG64)GetTickCount64();
    s_ep_last = now;
    s_ticks_total++;
    if (s_ep_start) {
        s_ep_ticks++;
        return FIX_RECOVERED_SAME;                        /* the same broken bike, already counted */
    }
    s_ep_ticks = 1;
    s_ep_start = now;
    return 1;
}

fix_t fix_suspension_nan_guard = {
    .id = "suspension-nan-guard",
    .title = "Stop the +0x1a29ae crash when the bike's physics has already broken (NaN suspension value)",
    .crash_id = "suspension-nan-lookup",
    .install = install,
    .on_exception = on_exception,
};

/* ---- what the game does with the broken bike next (game thread, from RunTelemetry at 10 Hz) ---- */

static LONG64 s_seen_start;     /* episode already announced */
static LONG64 s_reported_long;  /* "still broken" already logged for this episode at this many seconds */

static int pos_finite(void)
{
    uint32_t b[3];
    memcpy(b, g_ctx.pos, sizeof b);
    return !f32_bad(b[0]) && !f32_bad(b[1]) && !f32_bad(b[2]);
}

static void fmt_pos(char *out, size_t cap)
{
    if (!pos_finite()) snprintf(out, cap, "not finite");
    else snprintf(out, cap, "%.1f, %.1f, %.1f", g_ctx.pos[0], g_ctx.pos[1], g_ctx.pos[2]);
}

void suspension_guard_poll(void)
{
    if (!fix_suspension_nan_guard.active) return;
    LONG64 start = s_ep_start, last = s_ep_last;
    if (!start) return;
    LONG64 now = (LONG64)GetTickCount64();
    char pos[64];
    fmt_pos(pos, sizeof pos);
    if (start != s_seen_start) {
        s_seen_start = start;
        s_reported_long = 0;
        log_msg("fix suspension-nan-guard: the bike's physics broke (NaN) and the +0x1a29ae crash was stopped; telemetry: "
                "%.0f km/h, crashed %d, position %s, %s", g_ctx.speed * 3.6f, g_ctx.crashed, pos,
                g_ctx.server[0] ? "online" : "offline");
    }
    /* Healthy again only if the game is running the bike, nothing was guarded for a second, and telemetry (fresher than
     * the last guarded tick) shows a finite position. Leaving the track also ends it: the bike is set up again on the
     * next ride out (whether that clears a broken one is part of what this log is for). */
    int quiet = now - last >= (LONG64)EPISODE_GAP;
    int riding = g_ctx.on_track && g_ctx.running;
    if (quiet && (!g_ctx.on_track || (riding && (LONG64)g_ctx.telemetry_tick > last && pos_finite()))) {
        log_msg(g_ctx.on_track ? "fix suspension-nan-guard: the bike looks healthy again (no guarded tick for 1 s while riding, finite "
                                 "position %s); broken for about %lld ms, %ld physics ticks guarded"
                               : "fix suspension-nan-guard: left the track with the bike broken (position %s); %lld ms, %ld physics "
                                 "ticks guarded",
                pos, (long long)(last - start), (long)s_ep_ticks);
        s_ep_start = 0;
        s_seen_start = 0;
        return;
    }
    LONG64 secs = (now - start) / 1000;
    if (secs >= 10 && (secs >= s_reported_long + 30 || !s_reported_long)) {
        s_reported_long = secs;
        log_msg("fix suspension-nan-guard: still broken after %lld s (%ld ticks guarded, last %lld ms ago); telemetry: crashed %d, "
                "position %s, %s", (long long)secs, (long)s_ep_ticks, (long long)(now - last), g_ctx.crashed, pos,
                riding ? "riding" : "paused or in the pits");
    }
}

void suspension_guard_summary(void)
{
    if (s_ticks_total) log_msg("fix suspension-nan-guard: %ld physics tick(s) guarded this session", (long)s_ticks_total);
}
