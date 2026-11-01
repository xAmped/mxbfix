/*
 * Crash capture. See docs/ARCHITECTURE.md ("Crash capture") and
 * docs/REPORT_FORMAT.md for what ends up in a report.
 */
#pragma once
#include "common.h"

void crash_install(void);               /* vectored handler (fixes) + unhandled filter (reports) */
void crash_uninstall(void);
int  crash_veh_active(void);           /* research code that raises private exceptions needs this */

/* session.lock tracks whether the game exited cleanly. */
void crash_check_previous_session(void);   /* call before crash_session_begin() */
void crash_session_begin(void);
void crash_session_update(void);            /* call when track/session state changes */
void crash_session_end(void);               /* clean shutdown */

void crash_cleanup_old_dumps(void);
