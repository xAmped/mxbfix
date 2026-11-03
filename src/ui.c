#include "common.h"
#include "context.h"
#include "fixes.h"
#include "mxb_api.h"
#include "ui.h"
#include "update.h"
#include "upload.h"
#include "ui_font_data.h"
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#define URL_RELEASE L"https://github.com/xAmped/mxbfix/releases/latest"
#define URL_ISSUE   L"https://github.com/xAmped/mxbfix/issues/new"
#define URL_HOME    L"https://github.com/xAmped/mxbfix"

/* Colours are ABGR. */
#define C_PANEL      0xFA0F0F0Ful    /* onyx */
#define C_HEAD       0xFF191817ul
#define C_LINE       0xFF33302Eul
#define C_HOVER      0xFF242220ul
#define C_BTN        0xFF272523ul
#define C_BTN_HOT    0xFF3A3633ul
#define C_ACCENT     0xFF14FF39ul    /* fluorescent green */
#define C_ACCENT_HOT 0xFF5EFF7Cul
#define C_ON_ACCENT  0xFF0A0A0Aul
#define C_TEXT       0xFFF2F5F4ul
#define C_MUTED      0xFFA2A8A3ul
#define C_WARN       0xFF20B0FFul
#define C_OFF        0xFF6E726Eul

#define F_REG  1
#define F_BOLD 2
#define J_LEFT 0
#define J_CENTER 1
#define J_RIGHT 2

#define MAX_QUADS   96
#define MAX_STRINGS 56

static int             s_ok;              /* fonts in place: safe to draw */
static SPluginQuad_t   s_q[MAX_QUADS];
static SPluginString_t s_s[MAX_STRINGS];
static int             s_nq, s_ns;
static float           s_mx, s_my;        /* cursor in UI coordinates */
static int             s_mouse_ok, s_click, s_was_down, s_open;
static float           s_last_mx, s_last_my;
static ULONGLONG       s_cursor_until;
static int             s_reports;
static int             s_logged_state = -1;
static int             s_have_mouse, s_was_running;
static volatile LONG   s_setup_done, s_share;   /* [setup] done, [reports] share; the upload thread reads them */
static int             s_save_failed;

/* ---- fonts: one .dlo to install, so the plugin unpacks its font next to itself ---- */

static int write_if_different(const wchar_t *path, const unsigned char *data, DWORD size)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &a) && !a.nFileSizeHigh && a.nFileSizeLow == size) return 1;
    wchar_t tmp[MAX_PATH];
    if (lstrlenW(path) + 5 >= MAX_PATH) return 0;
    lstrcpyW(tmp, path);
    lstrcatW(tmp, L".tmp");
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0;
    BOOL ok = WriteFile(h, data, size, &w, NULL) && w == size;
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp);
    return ok;
}

static int unpack_fonts(void)
{
    HMODULE self;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(void *)unpack_fonts, &self)) return 0;
    DWORD n = GetModuleFileNameW(self, dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return 0;
    slash[1] = 0;
    if (!path_join(path, MAX_PATH, dir, L"mxbfix_data")) return 0;
    CreateDirectoryW(path, NULL);
    if (!path_join(dir, MAX_PATH, path, L"\\fonts")) return 0;
    CreateDirectoryW(dir, NULL);
    if (!path_join(path, MAX_PATH, dir, L"\\BarlowSemiCondensed-Regular.fnt") ||
        !write_if_different(path, UI_FONT_REGULAR, sizeof UI_FONT_REGULAR)) return 0;
    if (!path_join(path, MAX_PATH, dir, L"\\BarlowSemiCondensed-SemiBold.fnt") ||
        !write_if_different(path, UI_FONT_SEMIBOLD, sizeof UI_FONT_SEMIBOLD)) return 0;
    return 1;
}

int ui_draw_init(int *num_sprites, char **sprite_names, int *num_fonts, char **font_names)
{
    static char names[] = "mxbfix_data\\fonts\\BarlowSemiCondensed-Regular.fnt\0"
                          "mxbfix_data\\fonts\\BarlowSemiCondensed-SemiBold.fnt\0";
    (void)sprite_names;
    if (num_sprites) *num_sprites = 0;
    if (num_fonts) *num_fonts = 0;
    update_start();
    if (!cfg_int("ui", "enabled", 1)) { log_msg("in-game page: off in mxbfix.ini"); return 0; }
    if (!num_fonts || !font_names) return 0;
    if (!unpack_fonts()) {
        log_msg("in-game page: off, couldn't write its font next to the plugin (error %lu)", GetLastError());
        return 0;
    }
    *font_names = names;
    *num_fonts = 2;
    s_setup_done = cfg_int("setup", "done", 0) != 0;
    s_share = cfg_int("reports", "share", 1) != 0;
    s_ok = 1;
    if (ui_reports_allowed()) upload_start();
    return 0;
}

/* ---- drawing primitives ---- */

/* A box whose top edge is shifted right by `slant`. */
static void box(float x0, float y0, float x1, float y1, float slant, unsigned long c)
{
    if (s_nq >= MAX_QUADS) return;
    SPluginQuad_t *q = &s_q[s_nq++];
    q->m_aafPos[0][0] = x0 + slant; q->m_aafPos[0][1] = y0;
    q->m_aafPos[1][0] = x0;         q->m_aafPos[1][1] = y1;
    q->m_aafPos[2][0] = x1;         q->m_aafPos[2][1] = y1;
    q->m_aafPos[3][0] = x1 + slant; q->m_aafPos[3][1] = y0;
    q->m_iSprite = 0;
    q->m_ulColor = c;
}

static void text(float x, float y, float size, int font, int justify, unsigned long c, const char *s)
{
    if (s_ns >= MAX_STRINGS) return;
    SPluginString_t *t = &s_s[s_ns++];
    lstrcpynA(t->m_szString, s, sizeof t->m_szString);
    t->m_afPos[0] = x;
    t->m_afPos[1] = y;
    t->m_iFont = font;
    t->m_fSize = size;
    t->m_iJustify = justify;
    t->m_ulColor = c;
}

static int over(float x0, float y0, float x1, float y1)
{
    return s_mouse_ok && s_mx >= x0 && s_mx <= x1 && s_my >= y0 && s_my <= y1;
}

static int button(float x0, float y0, float x1, float y1, const char *label, int primary)
{
    int hot = over(x0, y0, x1, y1);
    box(x0, y0, x1, y1, 0.006f, primary ? (hot ? C_ACCENT_HOT : C_ACCENT) : (hot ? C_BTN_HOT : C_BTN));
    text((x0 + x1) / 2 + 0.003f, y0 + (y1 - y0 - 0.027f) / 2, 0.027f, F_BOLD, J_CENTER, primary ? C_ON_ACCENT : C_TEXT, label);
    return hot && s_click;
}

/* A tick box and its label; returns 1 when clicked. */
static int check_row(float x, float y, float x1, int on, const char *label)
{
    int hot = over(x, y, x1, y + 0.040f);
    box(x, y + 0.004f, x + 0.018f, y + 0.036f, 0.0f, hot ? C_BTN_HOT : C_BTN);
    if (on) box(x + 0.0035f, y + 0.0105f, x + 0.0145f, y + 0.0295f, 0.0f, C_ACCENT);
    text(x + 0.028f, y + 0.003f, 0.032f, F_BOLD, J_LEFT, C_TEXT, label);
    return hot && s_click;
}

static void set_share(int on)
{
    s_share = on;                                 /* takes effect right away, even if saving fails */
    s_save_failed = !cfg_set_int("reports", "share", on);
    if (s_save_failed) log_msg("in-game page: couldn't save the crash reports setting");
    if (on && s_setup_done) upload_mark_consent();
    log_msg("in-game page: crash reports %s", on ? "on" : "off");
}

static void open_target(const wchar_t *what)
{
    log_msg("in-game page: opening %ls", what);
    ShellExecuteW(NULL, L"open", what, NULL, NULL, SW_SHOWNORMAL);
}

/* ---- the numbers ---- */

extern fix_t fix_gate_vertex_pointer, fix_trainer_series_lookup, fix_trainer_repair, fix_ground_nan_guard, fix_trainer_cleanup_reset;

typedef struct {
    const char *name, *help;
    unsigned    total, session;
    int         active, enabled;
} crash_row_t;

static int collect(crash_row_t r[4], unsigned *total, unsigned *session)
{
    unsigned gt, gs;
    ground_watch_saves(&gt, &gs);
    r[0] = (crash_row_t){"Track loading", "Damaged ghost lap files, while a track loads.",
                         fix_trainer_series_lookup.saves_total_base + (unsigned)fix_trainer_series_lookup.saves,
                         (unsigned)fix_trainer_series_lookup.saves,
                         fix_trainer_series_lookup.active || fix_trainer_repair.active,
                         fix_trainer_series_lookup.enabled || fix_trainer_repair.enabled};
    r[1] = (crash_row_t){"Physics", "A rider's broken position reads outside the map.", gt, gs,
                         fix_ground_nan_guard.active, fix_ground_nan_guard.enabled};
    r[2] = (crash_row_t){"Gate", "Half a memory address lost, just after riding out.",
                         fix_gate_vertex_pointer.saves_total_base + (unsigned)fix_gate_vertex_pointer.saves,
                         (unsigned)fix_gate_vertex_pointer.saves, fix_gate_vertex_pointer.active, fix_gate_vertex_pointer.enabled};
    r[3] = (crash_row_t){"Cancel", "Cancelling a server join runs a cleanup twice.",
                         fix_trainer_cleanup_reset.saves_total_base + (unsigned)fix_trainer_cleanup_reset.saves,
                         (unsigned)fix_trainer_cleanup_reset.saves, fix_trainer_cleanup_reset.active, fix_trainer_cleanup_reset.enabled};
    *total = *session = 0;
    int best = -1;
    for (int i = 0; i < 4; i++) {
        *total += r[i].total;
        *session += r[i].session;
        if (r[i].total && (best < 0 || r[i].total > r[best].total)) best = i;
    }
    return best;
}

static int count_reports(void)
{
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW d;
    int n = 0;
    if (!path_join(pat, MAX_PATH, g_base_dir, L"reports\\pending\\*.json")) return 0;
    HANDLE h = FindFirstFileW(pat, &d);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do n++; while (FindNextFileW(h, &d));
    FindClose(h);
    return n;
}

/* ---- the page ---- */

/* Bottom left, rising from the tab: the game draws its own menus over plugins and clicks reach them, so the page stays
 * clear of the pause menu (x 0.375 to 0.625). */
#define PX 0.012f
#define PY 0.234f
#define PW 0.350f
#define PH 0.710f
#define PR (PX + PW)
#define PAD 0.024f

static void page(void)
{
    char b[120];
    crash_row_t r[4];
    unsigned total, session;
    int best = collect(r, &total, &session);
    int actives = r[0].active + r[1].active + r[2].active + r[3].active;
    update_state_t us = update_state();
    install_state_t ins = update_install_state();
    float y;

    box(PX, PY, PR, PY + PH, 0.0f, C_PANEL);

    /* header */
    box(PX, PY, PR, PY + 0.082f, 0.0f, C_HEAD);
    box(PX, PY, PX + 0.012f, PY + 0.082f, 0.0f, C_ACCENT);
    box(PX + 0.012f, PY, PX + 0.018f, PY + 0.082f, 0.006f, C_ACCENT);
    text(PX + 0.036f, PY + 0.016f, 0.052f, F_BOLD, J_LEFT, C_TEXT, "MXBFix");
    snprintf(b, sizeof b, "Version %s", MXBFIX_VERSION);
    text(PR - 0.052f, PY + 0.027f, 0.030f, F_REG, J_RIGHT, C_MUTED, b);
    if (button(PR - 0.044f, PY + 0.021f, PR - 0.014f, PY + 0.061f, "X", 0)) s_open = 0;

    /* update status */
    y = PY + 0.098f;
    unsigned long dot = us == UPDATE_CURRENT || us == UPDATE_AVAILABLE ? C_ACCENT : us == UPDATE_FAILED ? C_WARN : C_OFF;
    box(PX + PAD, y + 0.012f, PX + PAD + 0.008f, y + 0.032f, 0.003f, dot);
    switch (us) {
    case UPDATE_CURRENT:   snprintf(b, sizeof b, "Up to date (latest is %s)", update_latest()); break;
    case UPDATE_AVAILABLE:
        if (ins == INSTALL_READY) snprintf(b, sizeof b, "Restart the game to finish");
        else if (ins == INSTALL_WORKING) { const char *l; update_install_progress(&l); snprintf(b, sizeof b, "%s", l); }
        else if (ins == INSTALL_FAILED) snprintf(b, sizeof b, "Couldn't install %s", update_latest());
        else snprintf(b, sizeof b, "Update available: %s", update_latest());
        break;
    case UPDATE_CHECKING:  snprintf(b, sizeof b, "Checking for updates..."); break;
    case UPDATE_FAILED:    snprintf(b, sizeof b, "Couldn't check for updates"); break;
    default:               snprintf(b, sizeof b, "Update check is off"); break;
    }
    text(PX + PAD + 0.020f, y + 0.005f, 0.034f, F_BOLD, J_LEFT, C_TEXT, b);
    if (us == UPDATE_AVAILABLE && ins == INSTALL_IDLE && button(PR - PAD - 0.092f, y, PR - PAD, y + 0.044f, "Install", 1)) update_install_start();
    if (us == UPDATE_AVAILABLE && ins == INSTALL_FAILED && button(PR - PAD - 0.092f, y, PR - PAD, y + 0.044f, "Get update", 1)) open_target(URL_RELEASE);
    box(PX + PAD, y + 0.058f, PR - PAD, y + 0.060f, 0.0f, C_LINE);
    if (us == UPDATE_AVAILABLE && ins == INSTALL_WORKING) {
        /* The divider becomes a progress bar while installing. */
        float f = update_install_progress(NULL);
        box(PX + PAD, y + 0.054f, PR - PAD, y + 0.062f, 0.0f, C_BTN);
        box(PX + PAD, y + 0.054f, PX + PAD + (PW - 2 * PAD) * f, y + 0.062f, 0.0f, C_ACCENT);
        snprintf(b, sizeof b, "%d%%", (int)(f * 100.0f + 0.5f));
        text(PR - PAD, y + 0.008f, 0.030f, F_BOLD, J_RIGHT, C_MUTED, b);
    }

    /* crashes prevented */
    y += 0.072f;
    text(PX + PAD, y, 0.026f, F_BOLD, J_LEFT, C_MUTED, "CRASHES PREVENTED");
    snprintf(b, sizeof b, "%u", total);
    text(PX + PAD, y + 0.026f, 0.108f, F_BOLD, J_LEFT, C_ACCENT, b);
    snprintf(b, sizeof b, "%u this session", session);
    text(PX + PAD, y + 0.130f, 0.028f, F_REG, J_LEFT, C_MUTED, b);

    float mid = PX + PW * 0.5f;
    text(mid, y, 0.026f, F_BOLD, J_LEFT, C_MUTED, "MOST COMMON");
    if (best >= 0) {
        text(mid, y + 0.052f, 0.048f, F_BOLD, J_LEFT, C_TEXT, r[best].name);
        snprintf(b, sizeof b, r[best].total == 1 ? "%u time" : "%u times", r[best].total);
        text(mid, y + 0.130f, 0.028f, F_REG, J_LEFT, C_MUTED, b);
    } else {
        text(mid, y + 0.052f, 0.048f, F_BOLD, J_LEFT, C_MUTED, "None yet");
    }
    box(PX + PAD, y + 0.172f, PR - PAD, y + 0.174f, 0.0f, C_LINE);

    /* one row per crash type */
    y += 0.182f;
    text(PX + PAD + 0.010f, y, 0.023f, F_BOLD, J_LEFT, C_MUTED, "CRASH");
    text(mid - 0.004f, y, 0.023f, F_BOLD, J_LEFT, C_MUTED, "FIX");
    text(PR - PAD - 0.010f, y, 0.023f, F_BOLD, J_RIGHT, C_MUTED, "PREVENTED");
    y += 0.028f;
    const char *help = NULL;
    for (int i = 0; i < 4; i++) {
        float ry = y + 0.042f * (float)i;
        if (over(PX + PAD, ry, PR - PAD, ry + 0.040f)) {
            box(PX + PAD, ry, PR - PAD, ry + 0.040f, 0.0f, C_HOVER);
            help = r[i].help;
        }
        text(PX + PAD + 0.010f, ry + 0.006f, 0.030f, F_BOLD, J_LEFT, C_TEXT, r[i].name);
        const char *st = r[i].active ? "Active" : r[i].enabled ? "Not supported" : "Turned off";
        box(mid - 0.004f, ry + 0.012f, mid + 0.002f, ry + 0.028f, 0.002f, r[i].active ? C_ACCENT : C_OFF);
        text(mid + 0.012f, ry + 0.008f, 0.027f, F_REG, J_LEFT, r[i].active ? C_TEXT : C_MUTED, st);
        snprintf(b, sizeof b, "%u", r[i].total);
        text(PR - PAD - 0.010f, ry + 0.006f, 0.030f, F_BOLD, J_RIGHT, C_TEXT, b);
    }
    y += 0.170f;
    if (s_save_failed) text(PX + PAD, y + 0.004f, 0.026f, F_REG, J_LEFT, C_WARN, "Couldn't save that setting. It applies until you exit.");
    else if (help) text(PX + PAD, y + 0.004f, 0.026f, F_REG, J_LEFT, C_TEXT, help);
    else if (!actives) text(PX + PAD, y + 0.004f, 0.026f, F_REG, J_LEFT, C_WARN, "This game version isn't supported yet: fixes are off.");
    else {
        snprintf(b, sizeof b, "Ghost lap files repaired: %u    Crash reports: %d", trainer_repairs_total(), s_reports);
        text(PX + PAD, y + 0.004f, 0.026f, F_REG, J_LEFT, C_MUTED, b);
    }

    /* crash reports, changeable anytime */
    y += 0.036f;
    if (check_row(PX + PAD, y, PR - PAD, s_share, "Send crash reports and crash counts")) set_share(!s_share);

    /* actions */
    y = PY + PH - 0.066f;
    float bw = (PW - 2 * PAD - 0.024f) / 3.0f;
    float x = PX + PAD;
    if (button(x, y, x + bw, y + 0.046f, "Crash reports", 0)) {
        wchar_t dir[MAX_PATH];
        if (path_join(dir, MAX_PATH, g_base_dir, L"reports\\pending\\")) { ensure_dir(dir); open_target(dir); }
    }
    x += bw + 0.012f;
    if (button(x, y, x + bw, y + 0.046f, "Report issue", 0)) open_target(URL_ISSUE);
    x += bw + 0.012f;
    if (button(x, y, x + bw, y + 0.046f, "Project page", 0)) open_target(URL_HOME);
}

/* First time: nothing is sent until the player has seen this and pressed Continue. */
static void setup_page(void)
{
    float y = PY;
    box(PX, PY, PR, PY + PH, 0.0f, C_PANEL);
    box(PX, PY, PR, PY + 0.082f, 0.0f, C_HEAD);
    box(PX, PY, PX + 0.012f, PY + 0.082f, 0.0f, C_ACCENT);
    box(PX + 0.012f, PY, PX + 0.018f, PY + 0.082f, 0.006f, C_ACCENT);
    text(PX + 0.036f, PY + 0.016f, 0.052f, F_BOLD, J_LEFT, C_TEXT, "MXBFix");
    if (button(PR - 0.044f, PY + 0.021f, PR - 0.014f, PY + 0.061f, "X", 0)) s_open = 0;

    y += 0.104f;
    text(PX + PAD, y, 0.052f, F_BOLD, J_LEFT, C_ACCENT, "Welcome");
    y += 0.060f;
    text(PX + PAD, y, 0.028f, F_REG, J_LEFT, C_TEXT, "The fixes are already on. You don't need to set them up.");
    box(PX + PAD, y + 0.046f, PR - PAD, y + 0.048f, 0.0f, C_LINE);

    y = PY + 0.250f;
    if (check_row(PX + PAD, y, PR - PAD, s_share, "Send crash reports and crash counts")) set_share(!s_share);
    y += 0.056f;
    text(PX + PAD, y, 0.027f, F_REG, J_LEFT, C_TEXT, "If the game crashes, this helps find and fix new crashes.");
    y += 0.044f;
    text(PX + PAD, y, 0.027f, F_REG, J_LEFT, C_MUTED, "Sent: where it crashed, versions, PC specs, server, track,");
    y += 0.036f;
    text(PX + PAD, y, 0.027f, F_REG, J_LEFT, C_MUTED, "bike, loaded plugins, crash counts, a random install ID.");
    y += 0.036f;
    text(PX + PAD, y, 0.027f, F_REG, J_LEFT, C_MUTED, "Never sent: rider names, race numbers, passwords, crash dumps.");
    y += 0.036f;
    text(PX + PAD, y, 0.027f, F_REG, J_LEFT, C_MUTED, "You can change this anytime on this page.");

    y = PY + PH - 0.066f;
    if (button(PX + PAD, y, PR - PAD, y + 0.046f, "Continue", 1)) {
        s_save_failed = !cfg_set_int("reports", "share", s_share);
        s_setup_done = 1;
        cfg_set_int("setup", "done", 1);
        if (s_share) upload_mark_consent();
        log_msg("in-game page: setup finished, crash reports %s", s_share ? "on" : "off");
    }
}

/* Until the setup is done: a reminder when the bike is stopped. Never in a race session. */
static int banner_due(void)
{
    if (s_setup_done) return 0;
    if (g_ctx.event_type == 2 && g_ctx.race_session >= 6) return 0;
    return g_ctx.speed < 1.0f;
}

static void banner(void)
{
    const float x0 = 0.012f, x1 = 0.262f, y0 = 0.948f, y1 = 0.984f;
    box(x0, y0, x1, y1, 0.008f, C_ACCENT);
    text(x0 + 0.015f, y0 + 0.004f, 0.028f, F_BOLD, J_LEFT, C_ON_ACCENT, "MXBFix is installed. Press Esc to set it up.");
}

/* The way in: a small tab in the corner. */
static void tab(void)
{
    int setup = !s_setup_done;
    int upd = setup || update_state() == UPDATE_AVAILABLE;
    const float x0 = 0.012f, y0 = 0.948f, y1 = 0.984f, x1 = setup ? 0.166f : upd ? 0.178f : 0.078f;
    int hot = over(x0, y0, x1, y1);
    box(x0, y0, x1, y1, 0.008f, upd ? (hot ? C_ACCENT_HOT : C_ACCENT) : (hot ? C_BTN_HOT : C_PANEL));
    if (!upd) box(x0, y0, x0 + 0.005f, y1, 0.008f, C_ACCENT);
    text(x0 + 0.015f, y0 + 0.004f, 0.028f, F_BOLD, J_LEFT, upd ? C_ON_ACCENT : C_TEXT, setup ? "MXBFix: click to set up" : !upd ? "MXBFix" : update_install_state() == INSTALL_READY ? "MXBFix: restart to update" : "MXBFix: update available");
    if (hot && s_click) {
        s_open = !s_open;
        if (s_open) s_reports = count_reports();
    }
}

/* ---- input ---- */

/* The cursor in UI coordinates, and the left button. */
static void read_cursor(int *down)
{
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    POINT p;
    RECT rc;
    if (w && pid == GetCurrentProcessId() && GetCursorPos(&p) && ScreenToClient(w, &p) && GetClientRect(w, &rc) &&
        rc.right > 0 && rc.bottom > 0) {
        /* The UI lives in the largest centred 16:9 rectangle of the window. */
        float cw = (float)rc.right, ch = (float)rc.bottom;
        float vw = cw, vh = ch;
        if (cw * 9.0f > ch * 16.0f) vw = ch * 16.0f / 9.0f; else vh = cw * 9.0f / 16.0f;
        s_mx = ((float)p.x - (cw - vw) / 2.0f) / vw;
        s_my = ((float)p.y - (ch - vh) / 2.0f) / vh;
        s_mouse_ok = 1;
        *down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    }
}

static void poll_input(int riding)
{
    int down = 0;
    s_mouse_ok = 0;
    read_cursor(&down);
    s_click = s_mouse_ok && down && !s_was_down;
    s_was_down = down;
    if (!s_mouse_ok) return;
    if (!s_have_mouse) {                               /* the first reading is where the mouse rests, not movement */
        s_have_mouse = 1;
        s_last_mx = s_mx;
        s_last_my = s_my;
    }
    /* Online the game can't pause and doesn't say when its menu opens, so moving the mouse is the signal. Riding needs a
     * deliberate move, so a bump or a controller's stick doesn't bring the tab up. */
    float dx = s_mx - s_last_mx, dy = s_my - s_last_my;
    float need = riding ? 0.0004f : 0.00001f;
    if (dx * dx + dy * dy > need || s_click) {
        s_cursor_until = GetTickCount64() + 3000;
        s_last_mx = s_mx;
        s_last_my = s_my;
    }
}

int ui_reports_allowed(void) { return s_ok && s_setup_done && s_share; }

void ui_draw(int state, int *num_quads, void **quads, int *num_strings, void **strings)
{
    if (!s_ok || !num_quads || !quads || !num_strings || !strings) return;
    if (state != s_logged_state) {
        s_logged_state = state;
        log_msg("in-game page: draw state %d (0 track, 1 spectate, 2 replay), riding %d", state, g_ctx.running);
    }
    s_nq = s_ns = 0;
    int riding = state == 0 && g_ctx.running;
    if (riding && !s_was_running) {                    /* back on the bike after a pause */
        s_open = 0;
        s_cursor_until = 0;
    }
    s_was_running = riding;
    poll_input(riding);
    int recent = GetTickCount64() < s_cursor_until;
    /* Online the page can be open while the bike is live: opening the throttle puts it away, as does leaving it alone. */
    if (riding && s_open && (g_ctx.throttle > 0.2f || !recent)) s_open = 0;
    if (s_open) {
        if (over(PX, PY, PR, PY + PH)) s_cursor_until = GetTickCount64() + 3000;
        if (s_setup_done) page(); else setup_page();
    }
    if (s_open || recent || (!riding && (!s_setup_done || update_state() == UPDATE_AVAILABLE))) tab();
    else if (riding && banner_due()) banner();
    if (!s_nq && !s_ns) return;
    *quads = s_q;
    *num_quads = s_nq;
    *strings = s_s;
    *num_strings = s_ns;
}
