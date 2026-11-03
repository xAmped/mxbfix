/*
 * Update check: once per game start, a background thread asks GitHub for the latest release. [updates] check=0 turns it off.
 * Installing only happens when the player clicks: the download must carry a valid signature from the release key, and the new
 * plugin takes effect at the next game start. If anything fails, the current plugin stays.
 */
#pragma once

typedef enum { UPDATE_OFF, UPDATE_CHECKING, UPDATE_CURRENT, UPDATE_AVAILABLE, UPDATE_FAILED } update_state_t;

void           update_start(void);       /* game thread */
void           update_stop(void);        /* game thread, Shutdown: the worker must be gone before the DLL unloads */
update_state_t update_state(void);
typedef enum { INSTALL_IDLE, INSTALL_WORKING, INSTALL_READY, INSTALL_FAILED } install_state_t;
void            update_install_start(void);   /* game thread: the player clicked Install */
install_state_t update_install_state(void);
float           update_install_progress(const char **label);   /* 0 to 1, and what it's doing now, while INSTALL_WORKING */
const char    *update_latest(void);      /* e.g. "0.1.6"; valid once update_state() is CURRENT or AVAILABLE */
