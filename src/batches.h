/*
 * Render batches behind the gate crash (mxbikes.exe+0x24085c): read-only snapshots for the log and crash
 * reports, and the gate fix's prevented-crash count.
 */
#pragma once
#include "common.h"
#include "json.h"

#define OFF_ARRAY0        0x80
#define OFF_VB            0xa8
#define OFF_CLIENT        (OFF_VB + 0x10)

typedef struct {
    uint64_t obj, client, array0;
    int      quads, vb_count, vb_stride, gpu_buffer;
    int      off30, off34, written;       /* skip flags checked by the per-frame loop, vertices written */
} batch_t;

/* batches.c */
void        batches_init(void);
void        batches_log(const char *when);           /* game thread */
void        batches_write_json(jw_t *w);             /* crash-safe */
void        batches_run_init(void);                  /* game thread: RunInit */
void        batches_poll_activity(void);             /* game thread, telemetry */
uint64_t    batches_table(void);
int         batches_read(int i, batch_t *b);
const char *batches_mem_state(uint64_t a);
void        batches_forget(int i);
