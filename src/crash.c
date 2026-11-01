/*
 * Crash capture.
 *
 * Two layers:
 *  1. Unhandled-exception filter: runs when the game is about to die. Writes a
 *     JSON report (reports\pending\) and a minidump (dumps\). It chains to the
 *     previous filter, so mxbmrp3's handler still runs too.
 *  2. session.lock: written at startup, deleted on clean shutdown. If it is
 *     still there next launch and no report was written, the game died in a way
 *     that skipped every handler (fast-fail, killed, power loss). We then look
 *     up the Windows "Application Error" event for that process and file an
 *     "unclean_exit" report.
 *
 * The filter code must not use the heap, locks or CRT formatting: the process
 * is in an unknown state. Everything below the "crash-time" marker follows that.
 */
#define PSAPI_VERSION 2
#include "crash.h"
#include "context.h"
#include "fixes.h"
#include "json.h"
#include "known.h"
#include "batches.h"
#include "dev.h"
#include <psapi.h>
#include <dbghelp.h>
#include <winevt.h>
#include <wchar.h>

typedef BOOL(WINAPI *MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

static LPTOP_LEVEL_EXCEPTION_FILTER s_prev_filter;
static PVOID                        s_veh;
static MiniDumpWriteDump_t          s_minidump;
static volatile LONG                s_in_crash;
static volatile LONG                s_shut_down;      /* after Shutdown: only pass crashes on */
static volatile LONG                s_dump_stuck;     /* the dump worker didn't finish in time */
static int                          s_write_dumps;
static wchar_t                      s_lock_path[MAX_PATH];

/* ======================================================================== */
/* shared report pieces                                                       */
/* ======================================================================== */

static const char *event_type_name(int t)
{
    switch (t) {
    case 1: return "testing";
    case 2: return "race";
    case 4: return "straight_rhythm";
    case -1: return "replay";
    default: return "unknown";
    }
}

static void make_report_id(char *out, const char *kind)
{
    SYSTEMTIME t;
    GetSystemTime(&t);
    wsprintfA(out, "%s-%08X-%04u%02u%02u-%02u%02u%02u-%lu", kind, g_game_build, t.wYear, t.wMonth, t.wDay, t.wHour,
              t.wMinute, t.wSecond, GetCurrentProcessId());
}

static void write_header(jw_t *w, const char *kind, const char *id)
{
    char now[32];
    utc_now_iso(now);
    jw_u64(w, "schema", 1);
    jw_str(w, "kind", kind);
    jw_str(w, "id", id);
    jw_str(w, "time_utc", now);
    jw_obj(w, "plugin");
    jw_str(w, "name", "mxbfix");
    jw_str(w, "version", MXBFIX_VERSION);
    jw_end_obj(w);
    jw_obj(w, "game");
    jw_hex(w, "build", g_game_build);
    jw_u64(w, "image_size", g_game_size);
    jw_end_obj(w);
}

static void write_env(jw_t *w)
{
    jw_obj(w, "env");
    jw_str(w, "os", g_env.os);
    jw_str(w, "cpu", g_env.cpu);
    jw_u64(w, "ram_mb", g_env.ram_mb);
    jw_str(w, "gpu", g_env.gpu);
    jw_str(w, "gpu_driver_module", g_env.gpu_driver_module);
    jw_str(w, "gpu_driver_version", g_env.gpu_driver_version);
    jw_bool(w, "reshade", g_env.reshade);
    if (g_env.reshade) jw_str(w, "reshade_version", g_env.reshade_version);
    jw_end_obj(w);
}

/* Crash-safe: lifetime totals are cached at startup (fixes_init), no INI access here. */
static void write_fixes(jw_t *w)
{
    jw_arr(w, "fixes");
    for (int i = 0; i < fixes_count(); i++) {
        fix_t *f = fixes_get(i);
        jw_obj(w, NULL);
        jw_str(w, "id", f->id);
        jw_bool(w, "active", f->active);
        jw_i64(w, "saves_session", f->saves);
        jw_u64(w, "saves_total", (uint64_t)f->saves_total_base + (uint64_t)f->saves);
        jw_end_obj(w);
    }
    jw_end_arr(w);
}

static int write_file_atomic(const wchar_t *dir, const char *id, const char *ext, const char *data, size_t len)
{
    wchar_t name[160], tmp[MAX_PATH], fin[MAX_PATH];
    wsprintfW(name, L"%S%S", id, ext);
    path_join(fin, MAX_PATH, dir, name);
    wsprintfW(name, L"%S%S.tmp", id, ext);
    path_join(tmp, MAX_PATH, dir, name);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, data, (DWORD)len, &wr, NULL) && wr == len;
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp); return 0; }
    return MoveFileExW(tmp, fin, MOVEFILE_REPLACE_EXISTING) != 0;
}

static void pending_dir(wchar_t *out)
{
    path_join(out, MAX_PATH, g_base_dir, L"reports\\pending\\");
}

/* ======================================================================== */
/* session.lock                                                               */
/* ======================================================================== */

static void lock_set(const wchar_t *key, const char *utf8)
{
    wchar_t v[256];
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, v, 256);
    WritePrivateProfileStringW(L"session", key, v, s_lock_path);
}

static void lock_set_int(const wchar_t *key, long v)
{
    char t[24];
    wsprintfA(t, "%ld", v);
    lock_set(key, t);
}

static void lock_get(const wchar_t *key, char *out, int cap)
{
    wchar_t v[256];
    GetPrivateProfileStringW(L"session", key, L"", v, 256, s_lock_path);
    int n = WideCharToMultiByte(CP_UTF8, 0, v, -1, out, cap, NULL, NULL);
    if (n <= 0) out[0] = 0;
}

static wchar_t s_marker_path[MAX_PATH];   /* session.crashed: written by the crash handler */

static uint64_t process_created(void)
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    return ((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime;
}

void crash_session_begin(void)
{
    char now[32], t[24];
    utc_now_iso(now);
    DeleteFileW(s_lock_path);
    DeleteFileW(s_marker_path);
    lock_set_int(L"pid", (long)GetCurrentProcessId());
    wsprintfA(t, "0x%I64x", (unsigned long long)process_created());
    lock_set(L"created", t);
    lock_set(L"started_utc", now);
    wsprintfA(t, "0x%08X", g_game_build);
    lock_set(L"build", t);
    lock_set_int(L"crashed", 0);
}

void crash_session_update(void)
{
    lock_set(L"track", g_ctx.track_name);
    lock_set(L"track_id", g_ctx.track_id);
    lock_set(L"bike", g_ctx.bike_name);
    lock_set(L"event_type", g_ctx.in_event ? event_type_name(g_ctx.event_type) : "none");
    lock_set(L"server", g_ctx.server);
    lock_set_int(L"on_track", g_ctx.on_track);
    lock_set_int(L"running", g_ctx.running);
    lock_set_int(L"session", g_ctx.race_session);
    lock_set_int(L"riders", g_ctx.riders);
    char now[32];
    utc_now_iso(now);
    lock_set(L"updated_utc", now);
}

void crash_session_end(void)
{
    DeleteFileW(s_lock_path);
    DeleteFileW(s_marker_path);
}

/* --- Windows event log lookup for a process that died without our report --- */

/* "0000000000036ede" or "c0000005" -> "0x36ede" / "0xc0000005" */
static void norm_hex(char *s)
{
    char *p = s;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    while (*p == '0' && p[1]) p++;
    char tmp[40];
    lstrcpynA(tmp, p, sizeof tmp);
    lstrcpyA(s, "0x");
    lstrcatA(s, tmp);
    CharLowerA(s);
}

static int xml_data(const wchar_t *xml, const wchar_t *name, char *out, int cap)
{
    wchar_t pat[64];
    wsprintfW(pat, L"<Data Name='%s'>", name);
    const wchar_t *p = wcsstr(xml, pat);
    if (!p) return 0;
    p += wcslen(pat);
    const wchar_t *e = wcsstr(p, L"</Data>");
    if (!e) return 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, p, (int)(e - p), out, cap - 1, NULL, NULL);
    if (n < 0) n = 0;
    out[n] = 0;
    return 1;
}

typedef struct {
    int  found;
    char module[96], offset[40], code[24], time[40], app_path[MAX_PATH];
} win_event_t;

static uint64_t parse_hex(const char *s)
{
    uint64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; s++) {
        int d = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 : (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
        if (d < 0) break;
        v = v * 16 + (uint64_t)d;
    }
    return v;
}

/* Find the newest "Application Error" event (ID 1000) for mxbikes.exe with this pid
 * and, if known, this process creation time (FILETIME). PIDs get reused; the
 * creation time makes the match unique. 1 s tolerance for rounding. */
static void find_windows_crash_event(unsigned long pid, uint64_t created, win_event_t *ev)
{
    memset(ev, 0, sizeof *ev);
    EVT_HANDLE q = EvtQuery(NULL, L"Application",
                            L"*[System[Provider[@Name='Application Error'] and (EventID=1000) and "
                            L"TimeCreated[timediff(@SystemTime) <= 1209600000]]]",
                            EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!q) return;
    wchar_t pidhex[24];
    wsprintfW(pidhex, L"0x%lx", pid);
    static wchar_t xml[16384];
    EVT_HANDLE evs[16];
    DWORD got = 0;
    int scanned = 0;
    while (!ev->found && scanned < 200 && EvtNext(q, 16, evs, 2000, 0, &got)) {
        for (DWORD i = 0; i < got; i++) {
            DWORD used = 0, props = 0;
            if (!ev->found && EvtRender(NULL, evs[i], EvtRenderEventXml, sizeof xml - 2, xml, &used, &props)) {
                char app[64], p[24];
                if (xml_data(xml, L"AppName", app, sizeof app) && !lstrcmpiA(app, "mxbikes.exe") &&
                    xml_data(xml, L"ProcessId", p, sizeof p)) {
                    char want[24];
                    WideCharToMultiByte(CP_UTF8, 0, pidhex, -1, want, sizeof want, NULL, NULL);
                    char ct[40] = "";
                    uint64_t evc = xml_data(xml, L"ProcessCreationTime", ct, sizeof ct) ? parse_hex(ct) : 0;
                    uint64_t diff = evc > created ? evc - created : created - evc;
                    int time_ok = !created || !evc || diff < 10000000ull;
                    if (!lstrcmpiA(p, want) && time_ok) {
                        ev->found = 1;
                        xml_data(xml, L"ModuleName", ev->module, sizeof ev->module);
                        xml_data(xml, L"FaultingOffset", ev->offset, sizeof ev->offset);
                        xml_data(xml, L"ExceptionCode", ev->code, sizeof ev->code);
                        norm_hex(ev->offset);
                        norm_hex(ev->code);
                        const wchar_t *tc = wcsstr(xml, L"SystemTime='");
                        if (tc) {
                            int n = WideCharToMultiByte(CP_UTF8, 0, tc + 12, 19, ev->time, sizeof ev->time - 2, NULL, NULL);
                            if (n < 0) n = 0;
                            ev->time[n] = 'Z';            /* keep "YYYY-MM-DDTHH:MM:SS" + Z */
                            ev->time[n + 1] = 0;
                        }
                    }
                }
            }
            EvtClose(evs[i]);
            scanned++;
        }
    }
    EvtClose(q);
}

static char s_json[512 * 1024];

static void make_dirs(void)
{
    static const wchar_t *sub[] = {L"reports\\pending\\", L"reports\\sent\\", L"dumps\\"};
    for (size_t i = 0; i < sizeof sub / sizeof sub[0]; i++) {
        wchar_t d[MAX_PATH];
        path_join(d, MAX_PATH, g_base_dir, sub[i]);
        ensure_dir(d);
    }
}

void crash_check_previous_session(void)
{
    make_dirs();
    path_join(s_lock_path, MAX_PATH, g_base_dir, L"session.lock");
    path_join(s_marker_path, MAX_PATH, g_base_dir, L"session.crashed");
    if (GetFileAttributesW(s_lock_path) == INVALID_FILE_ATTRIBUTES) { DeleteFileW(s_marker_path); return; }

    char crashed[8], pidtxt[24], created[40];
    lock_get(L"crashed", crashed, sizeof crashed);
    lock_get(L"pid", pidtxt, sizeof pidtxt);
    lock_get(L"created", created, sizeof created);
    if (GetFileAttributesW(s_marker_path) != INVALID_FILE_ATTRIBUTES || crashed[0] == '1') {
        log_msg("previous session crashed and was already reported");
        return;
    }
    unsigned long pid = 0;
    for (const char *p = pidtxt; *p >= '0' && *p <= '9'; p++) pid = pid * 10 + (unsigned long)(*p - '0');

    win_event_t ev;
    find_windows_crash_event(pid, parse_hex(created), &ev);

    char id[96];
    make_report_id(id, "unclean");
    jw_t w;
    jw_init(&w, s_json, sizeof s_json);
    jw_obj(&w, NULL);
    write_header(&w, "unclean_exit", id);
    if (ev.found) {
        char sig[160];
        wsprintfA(sig, "%s+%s", ev.module, ev.offset);
        CharLowerA(sig);
        jw_str(&w, "signature", sig);
    } else {
        jw_str(&w, "signature", "unclean-exit");
    }
    jw_str(&w, "summary",
           ev.found ? "The game crashed in a way that skipped the plugin's crash handler; details come from Windows' own crash record."
                    : "The game did not shut down cleanly and Windows recorded no crash for it (killed, hung, or power loss).");

    jw_obj(&w, "previous_session");
    static const wchar_t *keys[] = {L"pid", L"created", L"started_utc", L"updated_utc", L"build", L"track", L"track_id", L"bike",
                                    L"event_type", L"server", L"on_track", L"running", L"session", L"riders"};
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        char kn[32], val[256];
        WideCharToMultiByte(CP_UTF8, 0, keys[k], -1, kn, sizeof kn, NULL, NULL);
        lock_get(keys[k], val, sizeof val);
        jw_str(&w, kn, val);
    }
    jw_end_obj(&w);

    jw_obj(&w, "windows_event");
    jw_bool(&w, "found", ev.found);
    if (ev.found) {
        jw_str(&w, "module", ev.module);
        jw_str(&w, "offset", ev.offset);
        jw_str(&w, "exception_code", ev.code);
        jw_str(&w, "time_utc", ev.time);
    }
    jw_end_obj(&w);
    write_env(&w);
    /* No "fixes": this runs before this session's fixes load, and the previous session's state isn't known. */
    jw_end_obj(&w);

    wchar_t dir[MAX_PATH];
    pending_dir(dir);
    if (!w.overflow && write_file_atomic(dir, id, ".json", s_json, w.len))
        log_msg("previous session ended uncleanly (pid %lu, windows event %s): wrote %s.json", pid, ev.found ? "found" : "not found", id);
    else
        log_msg("previous session ended uncleanly but writing the report failed");
}

/* ======================================================================== */
/* crash-time code: no heap, no locks, no CRT formatting below this line      */
/* ======================================================================== */

typedef struct {
    uint64_t base;
    uint32_t size, timestamp;
    wchar_t  path[MAX_PATH];
    const wchar_t *name;
} mod_t;

static mod_t   s_mods[400];
static int     s_nmods;
static HMODULE s_hmods[400];

static void snapshot_modules(void)
{
    DWORD need = 0;
    s_nmods = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), s_hmods, sizeof s_hmods, &need)) return;
    int n = (int)(need / sizeof(HMODULE));
    if (n > 400) n = 400;
    for (int i = 0; i < n; i++) {
        mod_t *m = &s_mods[s_nmods];
        MODULEINFO mi;
        if (!K32GetModuleInformation(GetCurrentProcess(), s_hmods[i], &mi, sizeof mi)) continue;
        m->base = (uint64_t)mi.lpBaseOfDll;
        m->size = mi.SizeOfImage;
        if (!K32GetModuleFileNameExW(GetCurrentProcess(), s_hmods[i], m->path, MAX_PATH)) m->path[0] = 0;
        const wchar_t *nm = wcsrchr(m->path, L'\\');
        m->name = nm ? nm + 1 : m->path;
        m->timestamp = 0;
        IMAGE_DOS_HEADER dos;
        IMAGE_NT_HEADERS64 nt;
        if (safe_read((void *)m->base, &dos, sizeof dos) && safe_read((uint8_t *)m->base + dos.e_lfanew, &nt, sizeof nt))
            m->timestamp = nt.FileHeader.TimeDateStamp;
        s_nmods++;
    }
}

static const mod_t *mod_find(uint64_t a)
{
    for (int i = 0; i < s_nmods; i++)
        if (a >= s_mods[i].base && a < s_mods[i].base + s_mods[i].size) return &s_mods[i];
    return NULL;
}

/* "module+0xoffset" or "0xaddress" */
static void loc_str(uint64_t a, char *out, size_t cap)
{
    const mod_t *m = mod_find(a);
    char hx[24];
    if (m) {
        int n = WideCharToMultiByte(CP_UTF8, 0, m->name, -1, out, (int)cap - 24, NULL, NULL);
        if (n <= 0) n = 1;
        out[n - 1] = '+';
        fmt_hex(out + n, a - m->base);
    } else {
        fmt_hex(hx, a);
        lstrcpynA(out, hx, (int)cap);
    }
}

static const char *exc_name(DWORD c)
{
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
    case 0xC0000409: return "STACK_BUFFER_OVERRUN";
    case 0xE06D7363: return "CPP_EXCEPTION";
    default: return "OTHER";
    }
}

static int region(uint64_t a, MEMORY_BASIC_INFORMATION *mbi)
{
    return VirtualQuery((void *)a, mbi, sizeof *mbi) == sizeof *mbi;
}

static int is_committed(uint64_t a)
{
    MEMORY_BASIC_INFORMATION mbi;
    return region(a, &mbi) && mbi.State == MEM_COMMIT;
}

static const char *state_name(DWORD s)
{
    return s == MEM_COMMIT ? "commit" : s == MEM_RESERVE ? "reserve" : "free";
}

static int is_code_addr(uint64_t a)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!region(a, &mbi) || mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) return 0;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

/* Looks like a return address: inside code and preceded by a call instruction. */
static int after_call(uint64_t ra)
{
    uint8_t b[8];
    if (!is_code_addr(ra) || !safe_read((void *)(ra - 8), b, 8)) return 0;
    if (b[3] == 0xE8) return 1;                                       /* call rel32 */
    if (b[2] == 0xFF && b[3] == 0x15) return 1;                       /* call [rip+disp32] */
    if (b[6] == 0xFF && (b[7] & 0xF8) == 0xD0) return 1;              /* call reg */
    if (b[5] == 0x41 && b[6] == 0xFF && (b[7] & 0xF8) == 0xD0) return 1;
    if (b[5] == 0xFF && (b[6] & 0x38) == 0x10) return 1;              /* call [reg+disp8] */
    if (b[2] == 0xFF && (b[3] & 0x38) == 0x10) return 1;              /* call [reg+disp32] */
    if (b[1] == 0xFF && (b[2] & 0x38) == 0x10) return 1;              /* call [reg+sib+disp32] */
    if (b[4] == 0xFF && (b[5] & 0x38) == 0x10) return 1;              /* call [reg+sib+disp8] */
    return 0;
}

static uint64_t s_frames[64];
static int      s_nframes;
static CONTEXT  s_walk;

static void unwind_stack(const CONTEXT *start)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    uint64_t lo = (uint64_t)tib->StackLimit, hi = (uint64_t)tib->StackBase;
    s_walk = *start;
    s_nframes = 0;
    while (s_nframes < 64) {
        uint64_t pc = s_walk.Rip, sp = s_walk.Rsp;
        if (!pc || sp < lo || sp >= hi) break;
        s_frames[s_nframes++] = pc;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(pc, &image, NULL);
        if (fn) {
            PVOID hd;
            DWORD64 ef;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, pc, fn, &s_walk, &hd, &ef, NULL);
        } else {
            uint64_t ra;
            if (!safe_read((void *)sp, &ra, 8)) break;
            s_walk.Rip = ra;
            s_walk.Rsp = sp + 8;
        }
        if (s_walk.Rsp <= sp && s_walk.Rip == pc) break;     /* no progress */
    }
}

static int known_access(const EXCEPTION_RECORD *r)
{
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) return -1;
    return r->ExceptionInformation[0] == 1 ? 1 : r->ExceptionInformation[0] == 0 ? 0 : 8;
}

static void diag(jw_t *w, const char *id, const char *text)
{
    jw_obj(w, NULL);
    jw_str(w, "id", id);
    jw_str(w, "text", text);
    jw_end_obj(w);
}

static const char *REG_NAMES[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

static void regs_of(const CONTEXT *c, uint64_t r[16])
{
    r[0] = c->Rax; r[1] = c->Rcx; r[2] = c->Rdx; r[3] = c->Rbx; r[4] = c->Rsp; r[5] = c->Rbp; r[6] = c->Rsi; r[7] = c->Rdi;
    r[8] = c->R8; r[9] = c->R9; r[10] = c->R10; r[11] = c->R11; r[12] = c->R12; r[13] = c->R13; r[14] = c->R14; r[15] = c->R15;
}

static int f32_bad(uint32_t bits) { return (bits & 0x7F800000u) == 0x7F800000u; }   /* NaN or Inf */

/* Crash-time marker: "this session's crash was reported". Plain file write, no INI APIs. */
static void mark_session_crashed(const char *id)
{
    HANDLE h = CreateFileW(s_marker_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, id, (DWORD)lstrlenA(id), &w, NULL);
    CloseHandle(h);
}

static struct {
    HANDLE              file;
    DWORD               tid;
    EXCEPTION_POINTERS *ep;
    volatile LONG       ok;            /* MiniDumpWriteDump succeeded */
} s_dump_job;
static int s_test_dump_fail;            /* test harness: make the dump fail (developer build only) */

static DWORD WINAPI dump_worker(LPVOID unused)
{
    (void)unused;
    MINIDUMP_EXCEPTION_INFORMATION mei = {s_dump_job.tid, s_dump_job.ep, FALSE};
    MINIDUMP_TYPE t = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs |
                                      MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo |
                                      MiniDumpWithThreadInfo | MiniDumpWithHandleData);
    BOOL ok;
    if (s_test_dump_fail) { DWORD wr; ok = FALSE; WriteFile(s_dump_job.file, "MDMP", 4, &wr, NULL); }   /* partial */
    else ok = s_minidump(GetCurrentProcess(), GetCurrentProcessId(), s_dump_job.file, t, &mei, NULL, NULL);
    CloseHandle(s_dump_job.file);
    s_dump_job.ok = ok ? 1 : 0;
    return 0;
}

/* Turn `"dump_file":"<name>"` (with an optional leading comma) into `"dump_file":null` padded with spaces, in
 * place, so the report keeps its length and stays valid JSON. Crash-safe. */
static int null_dump_field(char *p, size_t n)
{
    static const char key[] = "\"dump_file\":";
    size_t k = sizeof key - 1;
    for (size_t i = 0; i + k + 4 <= n; i++) {
        if (memcmp(p + i, key, k)) continue;
        char *v = p + i + k;
        size_t left = n - i - k;
        if (left < 4 || v[0] != '"') return 0;
        memcpy(v, "null", 4);
        for (size_t j = 4; j < left; j++) v[j] = ' ';
        return 1;
    }
    return 0;
}

static void write_crash_report(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *rec = ep->ExceptionRecord;
    CONTEXT *ctx = ep->ContextRecord;
    uint64_t fault = (uint64_t)rec->ExceptionAddress;
    int access = known_access(rec);
    uint64_t target = (access >= 0) ? rec->ExceptionInformation[1] : 0;
    uint64_t r[16];
    char loc[160], id[96];

    snapshot_modules();
    make_report_id(id, "crash");
    regs_of(ctx, r);

    const mod_t *fm = mod_find(fault);
    char fmod[96] = "";
    if (fm) WideCharToMultiByte(CP_UTF8, 0, fm->name, -1, fmod, sizeof fmod, NULL, NULL);

    uint8_t code[64];
    int code_ok = safe_read((void *)(fault - 32), code, 64);
    const known_crash_t *kc = code_ok ? known_match(fm ? fmod : NULL, code + 32, 32, access) : NULL;

    jw_t w;
    wchar_t dir[MAX_PATH];
    pending_dir(dir);
    loc_str(fault, loc, sizeof loc);

    /* 1. Minimal report straight away: if anything below faults (stack unwinding of a corrupt stack,
     *    module walking), this is what survives. The full report overwrites it. */
    jw_init(&w, s_json, sizeof s_json);
    jw_obj(&w, NULL);
    write_header(&w, "crash", id);
    {
        char sig[160];
        lstrcpynA(sig, loc, sizeof sig);
        CharLowerA(sig);
        jw_str(&w, "signature", sig);
    }
    if (kc) { jw_obj(&w, "known"); jw_str(&w, "id", kc->id); jw_str(&w, "title", kc->title); jw_end_obj(&w); }
    else jw_null(&w, "known");
    jw_obj(&w, "exception");
    jw_hex(&w, "code", rec->ExceptionCode);
    jw_str(&w, "name", exc_name(rec->ExceptionCode));
    jw_str(&w, "location", loc);
    jw_hex(&w, "address", fault);
    if (access >= 0) { jw_hex(&w, "target", target); }
    jw_end_obj(&w);
    jw_obj(&w, "registers");
    for (int i = 0; i < 16; i++) jw_hex(&w, REG_NAMES[i], r[i]);
    jw_hex(&w, "rip", ctx->Rip);
    jw_end_obj(&w);
    jw_bool(&w, "partial", 1);
    jw_end_obj(&w);
    if (!w.overflow && write_file_atomic(dir, id, ".json", s_json, w.len)) mark_session_crashed(id);

    /* 2. Full report. */
    jw_init(&w, s_json, sizeof s_json);
    jw_obj(&w, NULL);
    write_header(&w, "crash", id);

    /* signature: what the relay groups issues by */
    {
        char sig[160];
        lstrcpynA(sig, loc, sizeof sig);
        CharLowerA(sig);                 /* the relay groups issues by this exact string */
        jw_str(&w, "signature", sig);
    }
    if (kc) {
        jw_obj(&w, "known");
        jw_str(&w, "id", kc->id);
        jw_str(&w, "title", kc->title);
        jw_end_obj(&w);
    } else {
        jw_null(&w, "known");
    }

    /* exception */
    jw_obj(&w, "exception");
    jw_hex(&w, "code", rec->ExceptionCode);
    jw_str(&w, "name", exc_name(rec->ExceptionCode));
    jw_str(&w, "location", loc);
    jw_hex(&w, "address", fault);
    if (access >= 0) {
        jw_str(&w, "access", access == 1 ? "write" : access == 0 ? "read" : "execute");
        jw_hex(&w, "target", target);
        MEMORY_BASIC_INFORMATION mbi;
        if (region(target, &mbi)) jw_str(&w, "target_state", state_name(mbi.State));
    }
    jw_u64(&w, "thread_id", GetCurrentThreadId());
    jw_bool(&w, "game_thread", GetCurrentThreadId() == g_game_thread_id);
    if (code_ok) {
        jw_bytes(&w, "code_at_fault", code + 32, 32);
        jw_bytes(&w, "code_before_fault", code, 32);
    }
    jw_end_obj(&w);

    /* automatic diagnosis */
    jw_arr(&w, "diagnosis");
    if (kc) diag(&w, "known_crash", kc->title);
    if (rec->ExceptionCode == EXCEPTION_STACK_OVERFLOW) diag(&w, "stack_overflow", "The thread ran out of stack (runaway recursion).");
    if (access == 8 || (fault && !fm)) diag(&w, "bad_code_address", "Execution jumped to an address that is not code in any loaded module.");
    if (access >= 0) {
        if (target < 0x10000)
            diag(&w, "null_pointer", "Memory access near address zero: the game used an object that does not exist (null pointer).");
        else if (target >= 0xFFFFFFFF80000000ull)
            diag(&w, "sign_extended_32bit", "The address is a 32-bit number sign-extended to 64 bits: a value was stored in 32 bits and used as a pointer.");
        else if (target > 0x00007FFFFFFFFFFFull)
            diag(&w, "non_canonical_address", "The address is not a valid user-mode address: garbage was used as a pointer or index.");
        else if (target < 0x100000000ull && !is_committed(target)) {
            /* Is there real memory with the same low 32 bits above 4 GB? */
            int twins = 0;
            for (uint64_t k = 1; k < 0x80 && twins < 4; k++)
                if (is_committed((k << 32) | (target & 0xFFFFFFFFull))) twins++;
            int high_ptrs = 0;
            for (int i = 0; i < 16; i++) if (r[i] >= 0x100000000ull && r[i] < 0x800000000000ull && is_committed(r[i])) high_ptrs++;
            if (twins)
                diag(&w, "pointer_truncated_to_32bit",
                     "The bad address is below 4 GB and the same low 32 bits point at real memory above 4 GB: a 64-bit pointer was cut to 32 bits.");
            else if (high_ptrs)
                diag(&w, "pointer_maybe_truncated",
                     "The bad address is below 4 GB while the game's other live pointers are above 4 GB: possibly a pointer cut to 32 bits.");
        }
    }
    {
        int indefinite = 0;
        for (int i = 0; i < 16; i++) if ((uint32_t)r[i] == 0x80000000u) indefinite++;
        if (indefinite)
            diag(&w, "float_to_int_overflow",
                 "A register holds 0x80000000, the value the CPU produces when converting NaN/infinity to an integer: a broken float became an index.");
        const M128A *x = &ctx->Xmm0;
        int bad = 0;
        for (int i = 0; i < 16; i++) {
            uint32_t lane = (uint32_t)x[i].Low;
            if (f32_bad(lane)) bad++;
        }
        if (bad) diag(&w, "nan_in_float_registers", "Some floating-point registers hold NaN or infinity: physics or math produced a broken number.");
    }
    jw_end_arr(&w);

    /* registers, with what each pointer-looking value points at */
    jw_obj(&w, "registers");
    for (int i = 0; i < 16; i++) jw_hex(&w, REG_NAMES[i], r[i]);
    jw_hex(&w, "rip", ctx->Rip);
    jw_hex(&w, "eflags", ctx->EFlags);
    jw_end_obj(&w);
    jw_arr(&w, "register_targets");
    for (int i = 0; i < 16; i++) {
        if (r[i] < 0x10000 || r[i] > 0x7FFFFFFFFFFFull) continue;
        MEMORY_BASIC_INFORMATION mbi;
        if (!region(r[i], &mbi)) continue;
        jw_obj(&w, NULL);
        jw_str(&w, "reg", REG_NAMES[i]);
        jw_str(&w, "state", state_name(mbi.State));
        if (mbi.State == MEM_COMMIT) {
            jw_str(&w, "type", mbi.Type == MEM_IMAGE ? "image" : mbi.Type == MEM_MAPPED ? "mapped" : "private");
            jw_hex(&w, "protect", mbi.Protect);
            if (mod_find(r[i])) { loc_str(r[i], loc, sizeof loc); jw_str(&w, "module", loc); }
        }
        jw_end_obj(&w);
    }
    jw_end_arr(&w);
    jw_arr(&w, "xmm_low");
    {
        const M128A *x = &ctx->Xmm0;
        for (int i = 0; i < 16; i++) jw_hex(&w, NULL, (uint64_t)x[i].Low);
    }
    jw_end_arr(&w);

    /* call stack: real unwind, then a scan of the stack for return addresses */
    unwind_stack(ctx);
    jw_arr(&w, "stack");
    for (int i = 0; i < s_nframes; i++) { loc_str(s_frames[i], loc, sizeof loc); jw_str(&w, NULL, loc); }
    jw_end_arr(&w);
    jw_arr(&w, "stack_scan");
    {
        NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
        uint64_t sp = ctx->Rsp, hi = (uint64_t)tib->StackBase;
        int hits = 0;
        for (uint64_t a = sp & ~7ull; a + 8 <= hi && a < sp + 0x8000 && hits < 48; a += 8) {
            uint64_t v;
            if (!safe_read((void *)a, &v, 8)) break;
            if (after_call(v)) { loc_str(v, loc, sizeof loc); jw_str(&w, NULL, loc); hits++; }
        }
    }
    jw_end_arr(&w);

    /* what the player was doing */
    jw_obj(&w, "context");
    ULONGLONG now = GetTickCount64();
    jw_bool(&w, "in_event", g_ctx.in_event);
    if (g_ctx.in_event) {
        jw_str(&w, "event_type", event_type_name(g_ctx.event_type));
        jw_str(&w, "track", g_ctx.track_name);
        jw_str(&w, "track_id", g_ctx.track_id);
        jw_f(&w, "track_length_m", g_ctx.track_length, 1);
        jw_i64(&w, "centerline_segments", g_ctx.centerline_segments);
        jw_str(&w, "bike", g_ctx.bike_name);
        jw_str(&w, "bike_id", g_ctx.bike_id);
        jw_str(&w, "category", g_ctx.category);
        jw_str(&w, "server", g_ctx.server);
        jw_i64(&w, "riders", g_ctx.riders);
        jw_i64(&w, "race_session", g_ctx.race_session);
        jw_i64(&w, "race_session_state", g_ctx.race_session_state);
        jw_f(&w, "seconds_in_event", (double)(now - g_ctx.event_tick) / 1000.0, 1);
    }
    jw_bool(&w, "on_track", g_ctx.on_track);
    jw_bool(&w, "simulation_running", g_ctx.running);
    if (g_ctx.on_track) jw_f(&w, "seconds_since_on_track", (double)(now - g_ctx.run_init_tick) / 1000.0, 1);
    if (g_ctx.running) jw_f(&w, "seconds_since_run_start", (double)(now - g_ctx.run_start_tick) / 1000.0, 1);
    if (g_ctx.have_telemetry) {
        jw_obj(&w, "telemetry");
        jw_f(&w, "age_s", (double)(now - g_ctx.telemetry_tick) / 1000.0, 2);
        jw_f(&w, "on_track_time_s", g_ctx.on_track_time, 2);
        jw_f(&w, "track_pos", g_ctx.track_pos, 4);
        jw_f(&w, "speed_kmh", g_ctx.speed * 3.6, 1);
        jw_i64(&w, "rpm", g_ctx.rpm);
        jw_i64(&w, "gear", g_ctx.gear);
        jw_f(&w, "throttle", g_ctx.throttle, 2);
        jw_f(&w, "front_brake", g_ctx.front_brake, 2);
        jw_f(&w, "rear_brake", g_ctx.rear_brake, 2);
        jw_f(&w, "clutch", g_ctx.clutch, 2);
        jw_bool(&w, "crashed", g_ctx.crashed);
        jw_arr(&w, "wheel_material");
        jw_i64(&w, NULL, g_ctx.wheel_material[0]);
        jw_i64(&w, NULL, g_ctx.wheel_material[1]);
        jw_end_arr(&w);
        jw_arr(&w, "wheel_speed_kmh");
        jw_f(&w, NULL, g_ctx.wheel_speed[0] * 3.6, 1);
        jw_f(&w, NULL, g_ctx.wheel_speed[1] * 3.6, 1);
        jw_end_arr(&w);
        jw_arr(&w, "position");
        for (int i = 0; i < 3; i++) jw_f(&w, NULL, g_ctx.pos[i], 2);
        jw_end_arr(&w);
        jw_end_obj(&w);
    }
    jw_end_obj(&w);

    batches_write_json(&w);

    jw_arr(&w, "recent_events");
    {
        static event_rec_t evs[64];
        int n = ctx_events_snapshot(evs, 64);
        for (int i = 0; i < n; i++) {
            jw_obj(&w, NULL);
            jw_f(&w, "t", -(double)(now - evs[i].tick) / 1000.0, 2);
            jw_str(&w, "e", evs[i].name);
            if (evs[i].a || evs[i].b) { jw_i64(&w, "a", evs[i].a); jw_i64(&w, "b", evs[i].b); }
            jw_end_obj(&w);
        }
    }
    jw_end_arr(&w);

    write_env(&w);
    write_fixes(&w);

    jw_arr(&w, "modules");
    for (int i = 0; i < s_nmods; i++) {
        jw_obj(&w, NULL);
        jw_wstr(&w, "name", s_mods[i].name);
        jw_path(&w, "path", s_mods[i].path);
        jw_hex(&w, "base", s_mods[i].base);
        jw_hex(&w, "size", s_mods[i].size);
        jw_hex(&w, "timestamp", s_mods[i].timestamp);
        jw_end_obj(&w);
    }
    jw_end_arr(&w);

    char dumpname[128] = "";
    if (s_write_dumps && s_minidump) { lstrcpyA(dumpname, id); lstrcatA(dumpname, ".dmp"); }
    size_t dump_at = w.len;                  /* where "dump_file" starts, to null it if the dump fails */
    jw_str(&w, "dump_file", dumpname[0] ? dumpname : NULL);
    size_t dump_end = w.len;
    jw_bool(&w, "truncated", w.overflow);
    jw_end_obj(&w);

    /* Full report replaces the minimal one. If it overflowed, keep the minimal one. */
    int full_written = !w.overflow && write_file_atomic(dir, id, ".json", s_json, w.len);
    if (full_written) mark_session_crashed(id);

    /* 3. Dump, on a worker thread with a time limit: MiniDumpWriteDump can deadlock in a damaged
     *    process (loader lock held by another thread). After the limit we move on so the previous
     *    filter (e.g. mxbmrp3's) still runs. If the worker can't even start, there's no dump. */
    if (dumpname[0]) {
        wchar_t dpath[MAX_PATH], wname[160];
        MultiByteToWideChar(CP_UTF8, 0, dumpname, -1, wname, 160);
        path_join(dpath, MAX_PATH, g_base_dir, L"dumps\\");
        path_join(dpath, MAX_PATH, dpath, wname);
        s_dump_job.ok = 0;
        s_dump_job.file = CreateFileW(dpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (s_dump_job.file != INVALID_HANDLE_VALUE) {
            s_dump_job.tid = GetCurrentThreadId();
            s_dump_job.ep = ep;
            HANDLE t = CreateThread(NULL, 0x10000, dump_worker, NULL, 0, NULL);
            if (t) {
                if (WaitForSingleObject(t, 30000) == WAIT_TIMEOUT) s_dump_stuck = 1;
                CloseHandle(t);
            } else {
                CloseHandle(s_dump_job.file);
            }
        }
        /* No usable dump (couldn't create, couldn't start, or MiniDumpWriteDump failed): delete what's there and
         * make the report say there's no dump. A stuck dump is still being written, so it's left alone. */
        if (!s_dump_stuck && !s_dump_job.ok) {
            DeleteFileW(dpath);
            if (full_written && null_dump_field(s_json + dump_at, dump_end - dump_at))
                write_file_atomic(dir, id, ".json", s_json, w.len);
        }
    }
}

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep)
{
    /* After Shutdown the log and session files are closed: only pass the crash on. (The DLL is pinned, so a
     * filter that saved us as its predecessor can still call here safely.) */
    if (s_shut_down) return s_prev_filter ? s_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&s_in_crash, 1) == 0) write_crash_report(ep);
    else return EXCEPTION_CONTINUE_SEARCH;
    /* If our dump is still being written (timed out), don't hand over to the previous filter: it may call
     * the same dump library, which must not be used by two threads at once. Windows takes over instead. */
    if (s_dump_stuck) return EXCEPTION_CONTINUE_SEARCH;
    return s_prev_filter ? s_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep)
{
    if (batches_on_exception(ep)) return EXCEPTION_CONTINUE_EXECUTION;
    switch (ep->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
#ifdef MXBFIX_TESTHOOKS
    case MXBFIX_SELFTEST_CODE:
#endif
        return fixes_dispatch(ep);
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

/* ======================================================================== */

void crash_install(void)
{
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    s_minidump = dbg ? (MiniDumpWriteDump_t)(void *)GetProcAddress(dbg, "MiniDumpWriteDump") : NULL;
    s_write_dumps = cfg_int("capture", "write_dumps", 1);
    s_test_dump_fail = test_flag("DUMP_FAIL");

    ULONG guarantee = 64 * 1024;          /* stack kept free for exception handling on the game thread */
    SetThreadStackGuarantee(&guarantee);
    /* Never unload: a filter installed after ours (e.g. mxbmrp3's) saves ours as its predecessor and may call
     * it during a crash after the game has unloaded plugins. */
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (LPCWSTR)(void *)unhandled_filter, &self);
    s_prev_filter = SetUnhandledExceptionFilter(unhandled_filter);
    if (s_prev_filter) {                  /* and keep the filter we chain to loaded too */
        HMODULE prev;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCWSTR)(void *)s_prev_filter, &prev);
    }
    s_veh = AddVectoredExceptionHandler(1, vectored_handler);
    log_msg("crash handler installed (previous filter %s, dumps %s)", s_prev_filter ? "chained" : "none",
            s_write_dumps && s_minidump ? "on" : "off");
}

int crash_veh_active(void) { return s_veh != NULL; }

void crash_uninstall(void)
{
    s_shut_down = 1;
    if (s_veh) RemoveVectoredExceptionHandler(s_veh);
    s_veh = NULL;
    /* Only restore if nobody installed a filter after us. */
    LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(s_prev_filter);
    if (cur != unhandled_filter) SetUnhandledExceptionFilter(cur);
}

typedef struct {
    wchar_t  name[MAX_PATH];
    FILETIME t;
} dfile_t;

void crash_cleanup_old_dumps(void)
{
    int keep = cfg_int("capture", "keep_dumps", 10);
    if (keep < 0) keep = 0;               /* a bad setting must never become a negative index */
    if (keep > 256) keep = 256;
    wchar_t dir[MAX_PATH], pat[MAX_PATH];
    path_join(dir, MAX_PATH, g_base_dir, L"dumps\\");
    path_join(pat, MAX_PATH, dir, L"*.dmp");
    static dfile_t files[256];
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (n < 256) { lstrcpynW(files[n].name, fd.cFileName, MAX_PATH); files[n].t = fd.ftLastWriteTime; n++; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    /* newest first */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && CompareFileTime(&files[j].t, &files[j - 1].t) > 0; j--) {
            dfile_t tmp = files[j];
            files[j] = files[j - 1];
            files[j - 1] = tmp;
        }
    for (int i = keep; i < n; i++) {
        wchar_t p[MAX_PATH];
        path_join(p, MAX_PATH, dir, files[i].name);
        DeleteFileW(p);
    }
}
