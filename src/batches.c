/*
 * The render batch objects behind the gate crash (mxbikes.exe+0x24085c).
 *
 * Build 0x6A21833D keeps pointers to batch objects in a table at
 * mxbikes.exe+0x5815a0. Each object holds its vertex count at +0x00, a vertex
 * buffer descriptor at +0xa8 (count, stride, client pointer at +0xb8) and the
 * four per-vertex array cursors at +0x80..+0x98. The crash happens when the
 * client pointer is bad before the fill loop runs.
 *
 * batches_log() logs each batch's buffer pointer and whether it is real memory, at
 * startup and on every event/run change (only when it changed). batches_write_json()
 * puts the same snapshot into crash reports. batches_poll_activity() counts the gate
 * fix's saves. Read-only.
 */
#include "common.h"
#include "json.h"
#include "batches.h"
#include "dev.h"
#include "context.h"
#include <stdlib.h>

#define BUILD_WITH_TABLE  0x6A21833Du
#define TABLE_RVA         0x5815A0u
#define MAX_BATCHES       32
#define OFF_QUADS         0x00

static uint64_t s_table;
static uint64_t s_last_client[MAX_BATCHES];
static int      s_activity_logged;

int  gate_fix_is_risky(uint64_t client, int gpu_buffer_id);      /* fixes/fix_gate_vertex_pointer.c */
void gate_fix_record_save(uint64_t client);

/* ---- reading ---- */

int batches_read(int i, batch_t *b)
{
    memset(b, 0, sizeof *b);
    if (!s_table || !safe_read((void *)(s_table + 8ull * i), &b->obj, 8) || !b->obj) return 0;
    safe_read((void *)(b->obj + OFF_QUADS), &b->quads, 4);
    safe_read((void *)(b->obj + OFF_VB), &b->vb_count, 4);
    safe_read((void *)(b->obj + OFF_VB + 4), &b->vb_stride, 4);
    safe_read((void *)(b->obj + OFF_CLIENT), &b->client, 8);
    safe_read((void *)(b->obj + OFF_ARRAY0), &b->array0, 8);
    safe_read((void *)(b->obj + OFF_VB + 0x18), &b->gpu_buffer, 4);
    safe_read((void *)(b->obj + 0x30), &b->off30, 4);
    safe_read((void *)(b->obj + 0x34), &b->off34, 4);
    safe_read((void *)(b->obj + 0x78), &b->written, 4);
    return 1;
}

const char *batches_mem_state(uint64_t a)
{
    MEMORY_BASIC_INFORMATION m;
    if (!a) return "null";
    if (VirtualQuery((void *)a, &m, sizeof m) != sizeof m) return "invalid";
    return m.State == MEM_COMMIT ? "ok" : m.State == MEM_RESERVE ? "RESERVED (bad)" : "FREE (bad)";
}

void batches_init(void)
{
    s_table = (uint64_t)(uintptr_t)test_target("BATCH_TABLE");
    if (!s_table && g_game_build == BUILD_WITH_TABLE)
        s_table = (uint64_t)g_game_base + TABLE_RVA;
    log_msg("render batch check: %s", s_table ? "on" : "not available for this game version");
}

uint64_t batches_table(void) { return s_table; }
void     batches_forget(int i) { if (i >= 0 && i < MAX_BATCHES) s_last_client[i] = 0; }
void     batches_run_init(void) { s_activity_logged = 0; }

void batches_log(const char *when)
{
    if (!s_table) return;
    for (int i = 0; i < MAX_BATCHES; i++) {
        batch_t b;
        if (!batches_read(i, &b) || b.client == s_last_client[i]) continue;
        s_last_client[i] = b.client;
        log_msg("batch[%d] at %s: obj=%p quads=%d vb=%dx%d gpu_buffer=%d skip=%d/%d client=%p (%s)%s array0=%p", i, when,
                (void *)b.obj, b.quads, b.vb_count, b.vb_stride, b.gpu_buffer, b.off30, b.off34, (void *)b.client,
                batches_mem_state(b.client), b.client && b.client < 0x100000000ull ? " BELOW 4GB" : "", (void *)b.array0);
        if (i == 0 && b.client && gate_fix_is_risky(b.client, b.gpu_buffer))
            log_msg("batch 0 is in the risky state (no GPU buffer, memory above 4 GB): it crashes when it first draws unless the fix is active");
    }
}

void batches_write_json(jw_t *w)
{
    if (!s_table) return;
    jw_arr(w, "batches");
    for (int i = 0; i < MAX_BATCHES; i++) {
        batch_t b;
        if (!batches_read(i, &b)) continue;
        jw_obj(w, NULL);
        jw_i64(w, "index", i);
        jw_hex(w, "obj", b.obj);
        jw_i64(w, "quads", b.quads);
        jw_i64(w, "vb_count", b.vb_count);
        jw_i64(w, "vb_stride", b.vb_stride);
        jw_hex(w, "client", b.client);
        jw_str(w, "client_state", batches_mem_state(b.client));
        jw_hex(w, "array0", b.array0);
        jw_i64(w, "gpu_buffer", b.gpu_buffer);
        jw_i64(w, "vertices_written", b.written);
        jw_end_obj(w);
    }
    jw_end_arr(w);
}

/* Game thread, every telemetry tick: report the first time batch 0 draws something. */
void batches_poll_activity(void)
{
    if (!s_table || s_activity_logged) return;
    batch_t b;
    if (!batches_read(0, &b) || b.written <= 0) return;
    s_activity_logged = 1;
    int risky = gate_fix_is_risky(b.client, b.gpu_buffer);
    log_msg("batch 0 became active: %d vertices written (on-track time %.1fs)%s", b.written, (double)g_ctx.on_track_time,
            risky ? ", in the risky state" : ", not in the risky state (would not crash)");
    /* Count only real prevented crashes: risky state + first draw, never a forced test state. */
    if (risky && !batches_gate_forced()) gate_fix_record_save(b.client);
}
