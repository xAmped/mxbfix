/*
 * Fix: gate-vertex-pointer
 * Crash: mxbikes.exe+0x24085c, a few seconds after riding out of the pits
 *
 * Bug: when a render batch has no GPU buffer (its glBufferData step reported a
 * GL error, so the game fell back to client memory), the batch's four array
 * fields hold full 64-bit addresses. The per-frame setup at +0x240aa1 reads them
 * with 32-bit loads (`mov ecx, dword ptr [rdx+0xc8]` ...) and adds the GPU
 * mapping base, which is 0 in that mode. If the client buffer sits above 4 GB
 * the result is the address with its top half missing, and the fill loop at
 * +0x24085c writes there and crashes.
 *
 * Fix: rewrite that 86-byte block to load the fields as 64-bit values. With a
 * GPU buffer the fields hold small offsets written as 64-bit (upper half 0), so
 * behaviour is unchanged; without one the full address survives. The three
 * redundant reloads of the batch pointer are dropped to make room; the spare
 * 11 bytes become NOPs. Register state after the block matches the original.
 *
 * Applied once at Startup (game thread, before any batch is drawn), only if the
 * build and every original byte match.
 */
#include "../fixes.h"
#include "../dev.h"
#include <stdlib.h>

#define GATE_BUILD 0x6A21833Du
#define GATE_RVA   0x240AA1u
#define GATE_LEN   86

static const uint8_t ORIG[GATE_LEN] = {
    0x49, 0x8b, 0x54, 0xfd, 0x00,                           /* mov rdx, [r13+rdi*8]   */
    0x33, 0xf6,                                             /* xor esi, esi           */
    0x8b, 0x8a, 0xc8, 0x00, 0x00, 0x00,                     /* mov ecx, [rdx+0xc8]    */
    0x48, 0x03, 0xc8,                                       /* add rcx, rax           */
    0x48, 0x89, 0x8a, 0x80, 0x00, 0x00, 0x00,               /* mov [rdx+0x80], rcx    */
    0x49, 0x8b, 0x54, 0xfd, 0x00,
    0x8b, 0x8a, 0xd8, 0x00, 0x00, 0x00,                     /* mov ecx, [rdx+0xd8]    */
    0x48, 0x03, 0xc8,
    0x48, 0x89, 0x8a, 0x88, 0x00, 0x00, 0x00,               /* mov [rdx+0x88], rcx    */
    0x49, 0x8b, 0x54, 0xfd, 0x00,
    0x8b, 0x8a, 0xd0, 0x00, 0x00, 0x00,                     /* mov ecx, [rdx+0xd0]    */
    0x48, 0x03, 0xc8,
    0x48, 0x89, 0x8a, 0x90, 0x00, 0x00, 0x00,               /* mov [rdx+0x90], rcx    */
    0x49, 0x8b, 0x54, 0xfd, 0x00,
    0x8b, 0x8a, 0xf8, 0x00, 0x00, 0x00,                     /* mov ecx, [rdx+0xf8]    */
    0x48, 0x03, 0xc8,
    0x48, 0x89, 0x8a, 0x98, 0x00, 0x00, 0x00,               /* mov [rdx+0x98], rcx    */
};

static const uint8_t PATCH[GATE_LEN] = {
    0x49, 0x8b, 0x54, 0xfd, 0x00,                           /* mov rdx, [r13+rdi*8]   */
    0x33, 0xf6,                                             /* xor esi, esi           */
    0x48, 0x8b, 0x8a, 0xc8, 0x00, 0x00, 0x00,               /* mov rcx, [rdx+0xc8]    */
    0x48, 0x03, 0xc8,                                       /* add rcx, rax           */
    0x48, 0x89, 0x8a, 0x80, 0x00, 0x00, 0x00,               /* mov [rdx+0x80], rcx    */
    0x48, 0x8b, 0x8a, 0xd8, 0x00, 0x00, 0x00,               /* mov rcx, [rdx+0xd8]    */
    0x48, 0x03, 0xc8,
    0x48, 0x89, 0x8a, 0x88, 0x00, 0x00, 0x00,               /* mov [rdx+0x88], rcx    */
    0x48, 0x8b, 0x8a, 0xd0, 0x00, 0x00, 0x00,               /* mov rcx, [rdx+0xd0]    */
    0x48, 0x03, 0xc8,
    0x48, 0x89, 0x8a, 0x90, 0x00, 0x00, 0x00,               /* mov [rdx+0x90], rcx    */
    0x48, 0x8b, 0x8a, 0xf8, 0x00, 0x00, 0x00,               /* mov rcx, [rdx+0xf8]    */
    0x48, 0x03, 0xc8,
    0x48, 0x89, 0x8a, 0x98, 0x00, 0x00, 0x00,               /* mov [rdx+0x98], rcx    */
    0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,         /* 8-byte nop             */
    0x0f, 0x1f, 0x00,                                       /* 3-byte nop             */
};

/* Patch target: the game (or the test harness's stand-in in the developer build). */
static uint8_t *target(void)
{
    uint8_t *test = test_target("GATE_CODE");
    if (test) return test;
    if (g_game_build != GATE_BUILD) return NULL;
    return g_game_base + GATE_RVA;
}

static int install(fix_t *self)
{
    return fix_apply_patch(self->id, target(), ORIG, PATCH, GATE_LEN);
}

/*
 * Prevented-crash accounting, driven by batches.c (game thread only):
 *  - gate_fix_is_risky(): a batch with no GPU buffer and client memory above 4 GB.
 *    That alone is a near miss: nothing crashes until the batch draws.
 *  - gate_fix_record_save(): called once when a risky batch 0 actually draws
 *    (vertices written > 0). Without the fix that is the moment of the crash,
 *    so only this counts as a prevented crash.
 */
int  gate_fix_is_risky(uint64_t client, int gpu_buffer_id);
void gate_fix_record_save(uint64_t client);

fix_t fix_gate_vertex_pointer;

int gate_fix_is_risky(uint64_t client, int gpu_buffer_id)
{
    return gpu_buffer_id == 0 && client >= 0x100000000ull;
}

void gate_fix_record_save(uint64_t client)
{
    if (!fix_gate_vertex_pointer.active) {
        log_msg("gate-vertex-pointer: render batch 0 drew with its memory above 4 GB and no GPU buffer (%p): EXPECT A CRASH (fix not active)",
                (void *)client);
        return;
    }
    log_msg("gate-vertex-pointer: render batch 0 drew with its memory above 4 GB and no GPU buffer (%p): this would have crashed; the fix covers it",
            (void *)client);
    InterlockedIncrement(&fix_gate_vertex_pointer.saves);
}

fix_t fix_gate_vertex_pointer = {
    .id = "gate-vertex-pointer",
    .title = "Keep render batch vertex pointers 64-bit (stops the crash right after riding out)",
    .crash_id = "render-vertex-fill-truncation",
    .install = install,
    .on_exception = NULL,
};
