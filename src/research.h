/*
 * Render batch snapshots for the gate crash (research_batches.c, read-only, in every build) and the
 * developer-only research tools (game image dump, write watch, crash lab). Those are built only with
 * MXBFIX_RESEARCH and are not part of the public release, which gets the empty stand-ins below.
 * Details: docs/crashes/render-vertex-fill-truncation.md.
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

/* research_batches.c */
void        batches_init(void);
void        batches_log(const char *when);           /* game thread */
void        batches_write_json(jw_t *w);             /* crash-safe */
void        batches_run_init(void);                  /* game thread: RunInit */
void        batches_poll_activity(void);             /* game thread, telemetry */
uint64_t    batches_table(void);
int         batches_read(int i, batch_t *b);
const char *batches_mem_state(uint64_t a);
void        batches_forget(int i);

#ifdef MXBFIX_RESEARCH
void research_start(void);                           /* Startup, after batches_init */
void research_stop(void);                            /* Shutdown */
void batches_watch_arm(void);                        /* game thread */
void batches_watch_poll(void);                       /* game thread */
void batches_watch_disarm(void);                     /* game thread, Shutdown, before removing the handler */
int  batches_on_exception(EXCEPTION_POINTERS *ep);   /* vectored handler */
void batches_force_gate_state(int new_run);          /* game thread: RunInit (1) and RunStart (0) */
int  batches_gate_forced(void);
#else
static inline void research_start(void) {}
static inline void research_stop(void) {}
static inline void batches_watch_arm(void) {}
static inline void batches_watch_poll(void) {}
static inline void batches_watch_disarm(void) {}
static inline int  batches_on_exception(EXCEPTION_POINTERS *ep) { (void)ep; return 0; }
static inline void batches_force_gate_state(int new_run) { (void)new_run; }
static inline int  batches_gate_forced(void) { return 0; }
#endif
