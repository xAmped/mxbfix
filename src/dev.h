/*
 * Hooks used only by the developer build (test harness and investigation tools). In the public build
 * they compile to nothing.
 */
#pragma once
#include "common.h"

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

#ifdef MXBFIX_TESTHOOKS
#define MXBFIX_SELFTEST_CODE 0xE0F1C5EDu
uint8_t *test_target(const char *name);              /* stand-in address for a patch target, or NULL */
int      test_protect_fail(const char *id, int nth); /* make the n-th VirtualProtect of a fix fail */
void     test_paths(void);                           /* log how sample paths are written in reports */
int      test_flag(const char *name);                /* MXBFIX_TEST_<name> is set */
int      test_env(const char *name, char *buf, unsigned cap); /* value of MXBFIX_TEST_<name> */
#else
static inline uint8_t *test_target(const char *name) { (void)name; return NULL; }
static inline int      test_protect_fail(const char *id, int nth) { (void)id; (void)nth; return 0; }
static inline void     test_paths(void) {}
static inline int      test_flag(const char *name) { (void)name; return 0; }
static inline int      test_env(const char *name, char *buf, unsigned cap) { (void)name; (void)buf; (void)cap; return 0; }
#endif
