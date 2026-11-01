/*
 * Settings file: <save path>\mxbfix\mxbfix.ini. Created with defaults on first run.
 * The player-facing keys are listed in the README.
 */
#include "common.h"

static wchar_t s_ini[MAX_PATH];

static const char DEFAULT_INI[] =
    "; mxbfix settings. Delete this file to restore defaults.\r\n"
    "; All settings: https://github.com/xAmped/mxbfix#settings\r\n"
    "\r\n"
    "[capture]\r\n"
    "; Write a crash dump (.dmp) for each crash, in the dumps folder. 1 = on, 0 = off.\r\n"
    "write_dumps=1\r\n"
    "; How many crash dumps to keep before deleting the oldest.\r\n"
    "keep_dumps=10\r\n"
    "; Log riders whose position goes bad (NaN), with how long it lasts. Read-only. 1 = on.\r\n"
    "log_bad_positions=1\r\n"
    "\r\n"
    "[fixes]\r\n"
    "; Each fix can be switched off here: <fix id>=0\r\n";

void cfg_init(void)
{
    path_join(s_ini, MAX_PATH, g_base_dir, L"mxbfix.ini");
    if (GetFileAttributesW(s_ini) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(s_ini, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(h, DEFAULT_INI, sizeof DEFAULT_INI - 1, &w, NULL);
            CloseHandle(h);
        }
    }
}

int cfg_int(const char *section, const char *key, int def)
{
    wchar_t s[64], k[96];
    MultiByteToWideChar(CP_UTF8, 0, section, -1, s, 64);
    MultiByteToWideChar(CP_UTF8, 0, key, -1, k, 96);
    return (int)GetPrivateProfileIntW(s, k, def, s_ini);
}
