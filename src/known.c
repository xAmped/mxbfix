#include "known.h"
#include <string.h>

static const known_crash_t KNOWN[] = {
    {"render-vertex-fill-truncation", "mxbikes.exe", "418942d0418b41084883c210418942d4", 1,
     "Render buffer write through a pointer cut to 32 bits (crashes right after riding out)"},
    {"physics-contact-nan", "mxbikes.exe", "410fbf0c41488d0416498bd6490fafd0", 0,
     "Physics contact blow-up: NaN turned into a table index"},
    {"session-teardown-null", "mxbikes.exe", "488b4910e8????????488b0cdfe8", 0,
     "Null pointer while leaving a session or connecting"},
    {"session-end-oob-read", "mxbikes.exe", "448b04908b83d4040000ffc84183c004", 0,
     "Out-of-range array read at session end"},
    {"session-end-network-null", "mxbikes.exe", "443959b40f84????????0f28c70f28cf", 0,
     "Null pointer in network code at session end"},
    {"trainer-bad-string", "msvcr90.dll", "443811740848ffc1413bc275f12bcb89", 0,
     "Damaged trainer file read while a track loads"},
    {"stack-pointer-truncation", "mxbikes.exe", "8b11b8080000004533c93bd08915????", 0,
     "Stack address cut to 32 bits (depends on memory layout)"},
    {"teardown-stale-input-pointer", "mxbikes.exe", "41397429207e774c8b05????????6690", 0,
     "Stale pointer in the input/message pump while leaving a session"},
    {"centerline-off-by-one-read", "mxbikes.exe", "8b4419044c8d442470488d54247c418900", 0,
     "Read one element past the end of a 3,652-point position list (seen with page heap)"},
    {"obs-capture-hook", "ntdll.dll", "ff40244c8b7f184c8d67104c897de749", 1,
     "OBS Game Capture hook (third-party, not MX Bikes)"},
};

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int bytes_match(const uint8_t *code, size_t n, const char *p)
{
    size_t i = 0;
    while (*p) {
        if (*p == ' ') { p++; continue; }
        if (!p[1]) return 0;
        if (i >= n) return 0;
        if (p[0] != '?') {
            int hi = hexval(p[0]), lo = hexval(p[1]);
            if (hi < 0 || lo < 0 || code[i] != (uint8_t)(hi * 16 + lo)) return 0;
        }
        i++;
        p += 2;
    }
    return 1;
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

const known_crash_t *known_match(const char *module, const uint8_t *code, size_t n, int access)
{
    for (size_t k = 0; k < sizeof KNOWN / sizeof KNOWN[0]; k++) {
        const known_crash_t *e = &KNOWN[k];
        if (module && !ieq(module, e->module)) continue;
        if (e->access >= 0 && access >= 0 && e->access != access) continue;
        if (bytes_match(code, n, e->bytes)) return e;
    }
    return NULL;
}
