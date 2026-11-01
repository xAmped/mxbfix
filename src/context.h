/*
 * What the player was doing, kept up to date from the game's plugin callbacks,
 * plus facts about the PC. The crash handler reads these to explain a crash.
 */
#pragma once
#include "common.h"

typedef struct {
    /* event (EventInit) */
    int   in_event;
    int   event_type;              /* 1 testing, 2 race, 4 straight rhythm */
    int   server_type;
    char  track_id[160], track_name[160];
    char  bike_id[160], bike_name[160], category[160];
    char  server[128];
    float track_length;
    int   centerline_segments;

    /* run (RunInit .. RunDeinit = on track; RunStart .. RunStop = simulation running) */
    int   on_track, running;
    int   session, conditions;
    int   race_session, race_session_state;
    int   riders;
    ULONGLONG event_tick, run_init_tick, run_start_tick;

    /* last telemetry sample (10 Hz) */
    int   have_telemetry;
    ULONGLONG telemetry_tick;
    float on_track_time, track_pos;
    float speed, throttle, front_brake, rear_brake, clutch;
    int   rpm, gear, crashed;
    int   wheel_material[2];
    float wheel_speed[2];
    float pos[3];
} session_ctx_t;

typedef struct {
    ULONGLONG   tick;
    const char *name;
    int         a, b;
} event_rec_t;

typedef struct {
    char     os[48];
    char     cpu[96];
    char     gpu[128];
    char     gpu_driver_module[32];
    char     gpu_driver_version[48];
    uint64_t ram_mb;
    int      reshade;               /* a ReShade DLL is loaded from the game folder */
    char     reshade_version[32];
} env_info_t;

extern session_ctx_t g_ctx;
extern env_info_t    g_env;

void ctx_event(const char *name, int a, int b);
int  ctx_events_snapshot(event_rec_t *out, int max);   /* oldest first */
void ctx_rider_add(int race_num, int unactive);
void ctx_rider_remove(int race_num);
void ctx_reset_riders(void);

void env_collect_startup(void);
void env_collect_late(void);    /* after the GPU driver and injectors are loaded */
