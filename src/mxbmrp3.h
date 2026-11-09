/*
 * Match our reports to mxbmrp3's crash records for the same crash (startup only, game thread; uses the heap and CRT).
 *
 * mxbmrp3 (the HUD plugin many players run) has its own crash handler: it writes
 * <save>\mxbmrp3\crashes\mxbmrp3_crash_<local date>_<local time>_<pid>.log (+ .dmp), and keeps the newest crash's
 * fault, code and stack in <save>\mxbmrp3\pending_crash.json. Only file names, file times and that small JSON are read;
 * its logs hold other riders' names and are never opened.
 */
#pragma once
#include "common.h"
#include "json.h"

typedef struct {
    int      found;              /* a crash log for this process, written after it started */
    int      dump;               /* its .dmp exists too */
    uint64_t log_time;           /* FILETIME (UTC) of the log */
    wchar_t  name[96];           /* the log's file name (local log only: it holds the local time) */
    int      details;            /* pending_crash.json is this crash (time within 10 s of the log) */
    char     fault[100], code[16];
    char     stack[16][100];
    int      nstack;
} mxbmrp3_match_t;

/* Find mxbmrp3's record of a crash of process `pid`, created at FILETIME `created`. */
int  mxbmrp3_find(unsigned long pid, uint64_t created, mxbmrp3_match_t *m);
/* Write the "mxbmrp3" member. `ours` is our report's FILETIME (0: none, e.g. an unclean exit). */
void mxbmrp3_write(jw_t *w, const mxbmrp3_match_t *m, uint64_t ours, const char *our_signature, int with_stack);
/* Add the "mxbmrp3" member to our pending reports from that process (ones that don't have it yet). */
void mxbmrp3_annotate_reports(unsigned long pid, uint64_t created, const mxbmrp3_match_t *m);
