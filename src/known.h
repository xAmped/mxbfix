/*
 * Known crash fingerprints, so a report can say "this is crash X" by itself.
 *
 * A fingerprint is the machine code at the faulting instruction (hex, "??" =
 * any byte). Code bytes survive game rebuilds even when offsets move, so this
 * matches across builds. Seeded from the mxbmrp3 project's catalogue
 * (crash_analysis/known_game_crashes.json, MIT).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    const char *id;          /* stable id */
    const char *module;      /* module the fault is in, e.g. "mxbikes.exe" */
    const char *bytes;       /* fingerprint at the faulting instruction */
    int         access;      /* 0 read, 1 write, -1 any */
    const char *title;
} known_crash_t;

/* Returns the matching entry or NULL. code = bytes read at the fault address. */
const known_crash_t *known_match(const char *module, const uint8_t *code, size_t n, int access);

/* 1 if code matches pattern ("41 89 42 d0", "4189 ??", spaces optional). */
int bytes_match(const uint8_t *code, size_t n, const char *pattern);
