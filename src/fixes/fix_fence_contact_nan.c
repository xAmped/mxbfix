/*
 * fence-contact-nan: the "Forest" crash, mxbikes.exe+0x1a29ae, after scraping a fence.
 *
 * The game's sphere-vs-track-mesh collision (OPCODE sphere-triangle, the routine from +0x2d75b0) works out how deep a
 * small sphere sits in a triangle with
 *
 *     proj = dot(centre - closest, normal) / sqrt(|centre - closest|^2)     divss  xmm10, xmm0    (+0x2d7dc9)
 *     if (0 > proj) skip this triangle                                       comiss xmm9, xmm10 ; ja skip
 *
 * When the sphere's centre lies on the triangle (a handlebar end sliding into a zero-thickness fence), that's 0/0 = NaN.
 * `comiss 0, NaN` sets the "unordered" flags, `ja` isn't taken, and the contact goes to the physics with a NaN depth. One
 * such contact turns the whole bike and rider into NaN in a step; the suspension then stores a NaN length and the next
 * tick's table lookup crashes. (Typically a handlebar-end sphere against a zero-thickness, double-sided fence.)
 *
 * The patch swaps the comparison and the jump: `comiss xmm10, xmm9 ; jb skip` skips when proj < 0 **or** when it's NaN
 * (unordered sets CF). For every ordinary value the result is identical to the original (proj < 0 skips, 0 and above,
 * including -0.0, are kept). Same length, nothing jumps into the span, xmm9 is 0 throughout (cleared at +0x2d7674 and
 * +0x2d782d, never written otherwise, callee-saved across the sqrtf call). Tested on the game's own bytes.
 *
 * It's silent: there is no crash to count, so it doesn't count saves. Its effect shows as this crash disappearing.
 */
#include "../fixes.h"

#define FCN_BUILD 0x6A21833Du
#define FCN_RVA   0x2D7DC9u
#define FCN_LEN   11

static const uint8_t ORIG[FCN_LEN] = {
    0xf3, 0x44, 0x0f, 0x5e, 0xd0,                       /* divss  xmm10, xmm0        proj = dot / len       */
    0x45, 0x0f, 0x2f, 0xca,                             /* comiss xmm9, xmm10        0 vs proj              */
    0x77, 0x7a,                                         /* ja     +0x2d7e4e          skip if 0 > proj       */
};

static const uint8_t PATCH[FCN_LEN] = {
    0xf3, 0x44, 0x0f, 0x5e, 0xd0,                       /* divss  xmm10, xmm0        unchanged              */
    0x45, 0x0f, 0x2f, 0xd1,                             /* comiss xmm10, xmm9        proj vs 0              */
    0x72, 0x7a,                                         /* jb     +0x2d7e4e          skip if proj < 0 or NaN */
};

static uint8_t *target(void)
{
    if (g_game_build != FCN_BUILD) return NULL;
    return g_game_base + FCN_RVA;
}

static int install(fix_t *self)
{
    return fix_apply_patch(self->id, target(), ORIG, PATCH, FCN_LEN);
}

fix_t fix_fence_contact_nan = {
    .id = "fence-contact-nan",
    .title = "Drop a sphere-vs-track contact whose depth is NaN (stops the +0x1a29ae crash after scraping a fence)",
    .crash_id = "suspension-nan-lookup",
    .install = install,
    .on_exception = NULL,
};
