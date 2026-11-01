/*
 * MX Bikes plugin interface (PiBoSo), as used by mxbfix.
 *
 * The game loads every 64-bit DLL named *.dlo in <game>\plugins\ and calls the
 * exported functions below. Layouts and meanings follow PiBoSo's published
 * header (mirrored in the MIT-licensed mxbmrp3 project, vendor/piboso/mxb_api.h).
 * Only the structures this plugin reads are declared here.
 *
 * Version handshake for MX Bikes beta21: GetModID() = "mxbikes",
 * GetModDataVersion() = 8, GetInterfaceVersion() = 9. A mismatch makes the
 * game silently skip the plugin.
 *
 * Startup() returns the telemetry rate: 0 = 100Hz, 1 = 50Hz, 2 = 20Hz,
 * 3 = 10Hz. Returning -1 makes the game unload the plugin.
 *
 * All callbacks are called on the game's main thread and are blocking.
 */
#pragma once

#define MXB_MOD_ID            "mxbikes"
#define MXB_MOD_DATA_VERSION  8
#define MXB_INTERFACE_VERSION 9
#define MXB_TELEMETRY_10HZ    3

typedef struct {
    char  m_szRiderName[100];
    char  m_szBikeID[100];
    char  m_szBikeName[100];
    int   m_iNumberOfGears;
    int   m_iMaxRPM;
    int   m_iLimiter;
    int   m_iShiftRPM;
    float m_fEngineOptTemperature;
    float m_afEngineTemperatureAlarm[2];
    float m_fMaxFuel;
    float m_afSuspMaxTravel[2];
    float m_fSteerLock;
    char  m_szCategory[100];
    char  m_szTrackID[100];
    char  m_szTrackName[100];
    float m_fTrackLength;          /* centerline length, meters */
    int   m_iType;                 /* 1 = testing; 2 = race; 4 = straight rhythm */
    char  m_szServerName[64];
    int   m_iServerType;
    char  m_szGUID[100];
} SPluginsBikeEvent_t;

typedef struct {
    int   m_iSession;              /* see SPluginsRaceSession_t */
    int   m_iConditions;           /* 0 = sunny; 1 = cloudy; 2 = rainy */
    float m_fAirTemperature;
    char  m_szSetupFileName[100];
} SPluginsBikeSession_t;

typedef struct {
    int   m_iRPM;
    float m_fEngineTemperature;
    float m_fWaterTemperature;
    int   m_iGear;                 /* 0 = neutral */
    float m_fFuel;
    float m_fSpeedometer;          /* m/s */
    float m_fPosX, m_fPosY, m_fPosZ;
    float m_fVelocityX, m_fVelocityY, m_fVelocityZ;
    float m_fAccelerationX, m_fAccelerationY, m_fAccelerationZ;
    float m_aafRot[3][3];
    float m_fYaw, m_fPitch, m_fRoll;
    float m_fYawVelocity, m_fPitchVelocity, m_fRollVelocity;
    float m_afSuspLength[2];
    float m_afSuspVelocity[2];
    int   m_iCrashed;              /* 1 = rider detached from bike */
    float m_fSteer;
    float m_fThrottle;
    float m_fFrontBrake;
    float m_fRearBrake;
    float m_fClutch;
    float m_afWheelSpeed[2];
    int   m_aiWheelMaterial[2];    /* 0 = not in contact */
    float m_afBrakePressure[2];
    float m_fSteerTorque;
} SPluginsBikeData_t;

typedef struct {
    int   m_iType;                 /* 0 = straight; 1 = curve */
    float m_fLength;
    float m_fRadius;
    float m_fAngle;
    float m_afStart[2];
    float m_fHeight;
} SPluginsTrackSegment_t;

typedef struct {
    int   m_iRaceNum;
    char  m_szName[100];
    char  m_szBikeName[100];
    char  m_szBikeShortName[100];
    char  m_szCategory[100];
    int   m_iUnactive;             /* 1 = rider left the event */
    int   m_iNumberOfGears;
    int   m_iMaxRPM;
} SPluginsRaceAddEntry_t;

typedef struct {
    int   m_iRaceNum;
} SPluginsRaceRemoveEntry_t;

typedef struct {
    int   m_iSession;              /* race: 0 waiting, 1 practice, 2 pre-qualify, 3 qualify practice,
                                      4 qualify, 5 warmup, 6 race1, 7 race2. testing: 0 waiting, 1 in progress */
    int   m_iSessionState;         /* 16 in progress, 32 completed, 64 sighting lap, 256 pre-start,
                                      512 race over, 2048 cancelled */
    int   m_iSessionLength;
    int   m_iSessionNumLaps;
    int   m_iConditions;
    float m_fAirTemperature;
} SPluginsRaceSession_t;

typedef struct {
    int   m_iSession;
    int   m_iSessionState;
    int   m_iSessionLength;
} SPluginsRaceSessionState_t;
