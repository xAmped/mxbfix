/*
 * Entry points called by MX Bikes. Each callback updates the session context
 * (context.c) so a crash report can say what the player was doing.
 *
 * Startup order matters: paths -> log -> config -> game identity -> environment
 * -> check last session -> start this session -> crash handler -> fixes.
 */
#include "common.h"
#include "context.h"
#include "crash.h"
#include "fixes.h"
#include "mxb_api.h"
#include "batches.h"
#include "dev.h"

#define EXPORT __declspec(dllexport)


static void copy_str(char *dst, size_t cap, const char *src, size_t src_max)
{
    acp_to_utf8(src, src_max, dst, cap);
}

EXPORT char *GetModID(void)
{
    static char id[] = MXB_MOD_ID;
    return id;
}
EXPORT int GetModDataVersion(void) { return MXB_MOD_DATA_VERSION; }
EXPORT int GetInterfaceVersion(void) { return MXB_INTERFACE_VERSION; }

EXPORT int Startup(char *save_path)
{
    /* One copy per game process: a second mxbfix (e.g. an old .dlo left in plugins) would patch, log and
     * handle crashes twice over the same files. Returning -1 makes the game unload it. */
    wchar_t mutex_name[64];
    wsprintfW(mutex_name, L"Local\\mxbfix-%lu", GetCurrentProcessId());
    HANDLE single = CreateMutexW(NULL, FALSE, mutex_name);
    if (!single || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (single) CloseHandle(single);
        return -1;
    }
    g_game_thread_id = GetCurrentThreadId();
    util_init_paths(save_path);
    log_init();
    cfg_init();
    util_init_game_identity();
    log_msg("mxbfix %s starting, game build 0x%08X, image %u bytes", MXBFIX_VERSION, g_game_build, g_game_size);
    test_paths();                    /* developer build only */

    env_collect_startup();
    crash_check_previous_session();
    crash_session_begin();
    crash_install();
    fixes_init();
    crash_cleanup_old_dumps();
    batches_init();
    research_start();                /* developer build only */
    batches_log("startup");
    batches_watch_arm();

    ctx_event("Startup", 0, 0);
    return MXB_TELEMETRY_10HZ;
}

EXPORT void Shutdown(void)
{
    ctx_event("Shutdown", 0, 0);
    research_stop();
    fixes_poll();
    ground_watch_summary();
    batches_watch_poll();
    batches_watch_disarm();          /* before the handler goes away */
    log_msg("clean shutdown");
    crash_session_end();
    crash_uninstall();
    log_close();
}

EXPORT void EventInit(void *data, int size)
{
    if (data && size >= (int)sizeof(SPluginsBikeEvent_t)) {
        SPluginsBikeEvent_t *e = (SPluginsBikeEvent_t *)data;
        copy_str(g_ctx.track_id, sizeof g_ctx.track_id, e->m_szTrackID, sizeof e->m_szTrackID);
        copy_str(g_ctx.track_name, sizeof g_ctx.track_name, e->m_szTrackName, sizeof e->m_szTrackName);
        copy_str(g_ctx.bike_id, sizeof g_ctx.bike_id, e->m_szBikeID, sizeof e->m_szBikeID);
        copy_str(g_ctx.bike_name, sizeof g_ctx.bike_name, e->m_szBikeName, sizeof e->m_szBikeName);
        copy_str(g_ctx.category, sizeof g_ctx.category, e->m_szCategory, sizeof e->m_szCategory);
        copy_str(g_ctx.server, sizeof g_ctx.server, e->m_szServerName, sizeof e->m_szServerName);
        g_ctx.track_length = e->m_fTrackLength;
        g_ctx.event_type = e->m_iType;
        g_ctx.server_type = e->m_iServerType;
    }
    g_ctx.in_event = 1;
    g_ctx.event_tick = GetTickCount64();
    g_ctx.centerline_segments = 0;
    ctx_reset_riders();
    ctx_event("EventInit", g_ctx.event_type, g_ctx.server_type);
    env_collect_late();
    crash_session_update();
    trainer_fix_note("event load");
    batches_log("event start");
    batches_watch_arm();
    log_msg("event: %s, track '%s', bike '%s', server '%s'", g_ctx.event_type == 2 ? "race" : "testing",
            g_ctx.track_name, g_ctx.bike_name, g_ctx.server);
}

EXPORT void EventDeinit(void)
{
    trainer_cleanup_note();          /* sent just before the game's trainer cleanup runs */
    ctx_event("EventDeinit", 0, 0);
    g_ctx.in_event = 0;
    g_ctx.on_track = g_ctx.running = 0;
    fixes_poll();
    crash_session_update();
}

EXPORT void RunInit(void *data, int size)
{
    if (data && size >= (int)sizeof(SPluginsBikeSession_t)) {
        SPluginsBikeSession_t *s = (SPluginsBikeSession_t *)data;
        g_ctx.session = s->m_iSession;
        g_ctx.conditions = s->m_iConditions;
    }
    g_ctx.on_track = 1;
    g_ctx.run_init_tick = GetTickCount64();
    g_ctx.have_telemetry = 0;
    ctx_event("RunInit", g_ctx.session, g_ctx.conditions);
    crash_session_update();
    batches_watch_poll();
    trainer_fix_note("on track");
    batches_run_init();
    batches_force_gate_state(1);
    batches_log("on track");
    batches_watch_arm();
}

EXPORT void RunDeinit(void)
{
    ctx_event("RunDeinit", 0, 0);
    g_ctx.on_track = g_ctx.running = 0;
    fixes_poll();
    crash_session_update();
}

EXPORT void RunStart(void)
{
    g_ctx.running = 1;
    g_ctx.run_start_tick = GetTickCount64();
    ctx_event("RunStart", 0, 0);
    crash_session_update();
    batches_watch_poll();
    batches_force_gate_state(0);
    batches_log("ride out");
}

EXPORT void RunStop(void)
{
    g_ctx.running = 0;
    ctx_event("RunStop", 0, 0);
}

EXPORT void RunLap(void *data, int size)
{
    (void)size;
    ctx_event("RunLap", data ? *(int *)data : 0, 0);
}

EXPORT void RunSplit(void *data, int size)
{
    (void)size;
    ctx_event("RunSplit", data ? *(int *)data : 0, 0);
}

EXPORT void RunTelemetry(void *data, int size, float time, float pos)
{
    if (data && size >= (int)sizeof(SPluginsBikeData_t)) {
        SPluginsBikeData_t *b = (SPluginsBikeData_t *)data;
        g_ctx.speed = b->m_fSpeedometer;
        g_ctx.rpm = b->m_iRPM;
        g_ctx.gear = b->m_iGear;
        g_ctx.throttle = b->m_fThrottle;
        g_ctx.front_brake = b->m_fFrontBrake;
        g_ctx.rear_brake = b->m_fRearBrake;
        g_ctx.clutch = b->m_fClutch;
        g_ctx.crashed = b->m_iCrashed;
        g_ctx.wheel_material[0] = b->m_aiWheelMaterial[0];
        g_ctx.wheel_material[1] = b->m_aiWheelMaterial[1];
        g_ctx.wheel_speed[0] = b->m_afWheelSpeed[0];
        g_ctx.wheel_speed[1] = b->m_afWheelSpeed[1];
        g_ctx.pos[0] = b->m_fPosX;
        g_ctx.pos[1] = b->m_fPosY;
        g_ctx.pos[2] = b->m_fPosZ;
    }
    g_ctx.on_track_time = time;
    g_ctx.track_pos = pos;
    g_ctx.telemetry_tick = GetTickCount64();
    g_ctx.have_telemetry = 1;
    fixes_poll();
    ground_watch_poll();
    batches_watch_poll();
    batches_poll_activity();
}

/* Called by the game every frame with every rider's track position while a session runs (also while
 * spectating or in the pits). Used only as a heartbeat for the bad-position log; the data is ignored. */
EXPORT void RaceTrackPosition(int num_vehicles, void *data, int size)
{
    (void)num_vehicles;
    (void)data;
    (void)size;
    ground_watch_poll();
}

EXPORT void TrackCenterline(int num_segments, SPluginsTrackSegment_t *segments, void *race_data)
{
    (void)segments;
    (void)race_data;
    g_ctx.centerline_segments = num_segments;
    ctx_event("TrackCenterline", num_segments, 0);
}

EXPORT void RaceEvent(void *data, int size)
{
    (void)size;
    ctx_event("RaceEvent", data ? *(int *)data : 0, 0);
}

EXPORT void RaceDeinit(void) { ctx_event("RaceDeinit", 0, 0); }

EXPORT void RaceAddEntry(void *data, int size)
{
    if (data && size >= (int)sizeof(SPluginsRaceAddEntry_t)) {
        SPluginsRaceAddEntry_t *e = (SPluginsRaceAddEntry_t *)data;
        ctx_rider_add(e->m_iRaceNum, e->m_iUnactive);
        ctx_event("RaceAddEntry", e->m_iRaceNum, e->m_iUnactive);
    }
}

EXPORT void RaceRemoveEntry(void *data, int size)
{
    if (data && size >= (int)sizeof(SPluginsRaceRemoveEntry_t)) {
        int num = ((SPluginsRaceRemoveEntry_t *)data)->m_iRaceNum;
        ctx_rider_remove(num);
        ctx_event("RaceRemoveEntry", num, 0);
    }
}

EXPORT void RaceSession(void *data, int size)
{
    if (data && size >= (int)sizeof(SPluginsRaceSession_t)) {
        SPluginsRaceSession_t *s = (SPluginsRaceSession_t *)data;
        g_ctx.race_session = s->m_iSession;
        g_ctx.race_session_state = s->m_iSessionState;
        ctx_event("RaceSession", s->m_iSession, s->m_iSessionState);
        crash_session_update();
    }
}

EXPORT void RaceSessionState(void *data, int size)
{
    if (data && size >= (int)sizeof(SPluginsRaceSessionState_t)) {
        SPluginsRaceSessionState_t *s = (SPluginsRaceSessionState_t *)data;
        g_ctx.race_session = s->m_iSession;
        g_ctx.race_session_state = s->m_iSessionState;
        ctx_event("RaceSessionState", s->m_iSession, s->m_iSessionState);
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}
