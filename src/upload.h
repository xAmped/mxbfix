/*
 * Sends crash reports and daily crash counts, only after the player has gone through the setup with the box ticked. Runs once per game start on a background thread; never at crash time. Only reports made after
 * the box was ticked are sent, and never dumps.
 */
#pragma once

void upload_start(void);     /* game thread, if ui_reports_allowed() */
void upload_stop(void);      /* game thread, Shutdown */
void upload_mark_consent(void);   /* game thread: the box was just ticked; only what happens from now on gets sent */
