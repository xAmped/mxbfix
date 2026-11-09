/*
 * Freeze evidence for unclean exits. A background thread writes one small record every 2 s while the game runs
 * (%TEMP%\mxbfix\alive-<pid>.txt, not Documents, which is often synced): when the process last ran, when the game thread
 * last called the plugin, and whether Windows saw the game window as not responding. The next start reads it for the
 * unclean-exit report, so a freeze can be told from a quit or an instant death. Nothing in it identifies the player.
 */
#pragma once
#include "common.h"

typedef struct {
    int      found;
    uint64_t process_ft;      /* FILETIME (UTC) the process was last seen running */
    int64_t  game_quiet_s;    /* the game thread hadn't called the plugin for this long by then; -1 if it never did */
    int      hung;            /* Windows saw the window as not responding at that moment */
    int64_t  hung_s;          /* for this long */
} alive_info_t;

void alive_start(void);                                   /* Startup, after crash_session_begin */
void alive_beat(void);                                    /* game thread callbacks (Draw, telemetry, track positions) */
void alive_stop(void);                                    /* Shutdown: a clean end leaves no record */
/* The previous process's record, if it matches that pid and creation time; then it's deleted. */
int  alive_read_previous(unsigned long pid, uint64_t created, alive_info_t *out);
