/*
 * Fix: trainer-repair
 * Crash: msvcr90.dll+0x36ede while a track loads (damaged trainer file)
 *
 * Defence in depth next to trainer-series-lookup, and the only protection on
 * game builds where that patch doesn't apply. At startup, before any track
 * loads, every trainer file in <save>\profiles\*\trainers\*.trn is checked:
 *
 *  - the 26-byte header must match exactly (GHS, version 2, "mxbikes", record
 *    type 6, record size 0x57c), and the bike name at 0x62 must be printable
 *    ASCII terminated inside [0x62, 0x92). Anything else is left alone.
 *  - junk is zeroed in [name terminator + 1, 0x92): the bike-name slack and the
 *    start of the series-name field. The series field is 32 bytes, [0x82, 0xA2)
 *    (the loader puts it at record +0x68 and the next field at +0x88; series
 *    names are limited to 31 characters, `cmp rcx, 0x20` at +0x3983). If it holds
 *    a plausible name (printable ASCII terminated inside those 32 bytes), the name
 *    and its terminator are kept whole; only positively corrupt data
 *    (non-printable bytes, or no terminator) is cleared. Bytes from 0x92 on are
 *    never changed (junk after an empty name there is harmless). Every damaged
 *    file seen so far has non-printable junk at 0x82, and on this game build an
 *    unknown name is handled by trainer-series-lookup anyway.
 *  - if anything changes, the original is backed up once to
 *    <save>\mxbfix\backups\trainers\<profile>\<file> first (written to a temp
 *    file and renamed into place, so a backup is never partial). An existing
 *    backup is only trusted if it is a regular file with the trainer header;
 *    otherwise the file is left alone and reported. Nothing else changes.
 *
 * Files are replaced atomically (temp file + rename) and only after the backup
 * is safely in place.
 */
#include "../fixes.h"
#include <wchar.h>

#define NAME_OFF   0x62
#define SERIES_OFF 0x82
#define NAME_END   0x92                  /* end of the range that may be zeroed */
#define SERIES_END 0xA2                  /* end of the 32-byte series field */
#define MAX_TRN   (16u << 20)

static const uint8_t HEADER[0x1a] = {
    'G', 'H', 'S', 0, 2, 0, 0, 0, 'm', 'x', 'b', 'i', 'k', 'e', 's', 0,
    '%', 's', 6, 0, 0, 0, 0x7c, 0x05, 0, 0,
};

static int read_all(const wchar_t *path, uint8_t **out, DWORD *len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    int ok = 0;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart >= NAME_END && sz.QuadPart <= MAX_TRN) {
        *len = (DWORD)sz.QuadPart;
        *out = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, *len);
        DWORD got = 0;
        ok = *out && ReadFile(h, *out, *len, &got, NULL) && got == *len;
        if (!ok && *out) { HeapFree(GetProcessHeap(), 0, *out); *out = NULL; }
    }
    CloseHandle(h);
    return ok;
}

static int write_all(const wchar_t *path, const uint8_t *data, DWORD len, DWORD disposition)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0;
    BOOL ok = WriteFile(h, data, len, &w, NULL) && w == len && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) DeleteFileW(path);
    return ok;
}

/* Does the series-name field hold a plausible name (printable, terminated inside the 32-byte field)? */
static int plausible_series(const uint8_t *b, DWORD len)
{
    if (len < SERIES_END) return 0;
    for (int i = SERIES_OFF; i < SERIES_END; i++) {
        if (b[i] == 0) return i > SERIES_OFF;
        if (b[i] < 32 || b[i] > 126) return 0;
    }
    return 0;                                                        /* no terminator */
}

/* An existing backup is trusted only if it is a regular file with the trainer header and at least one whole
 * record (a truncated copy is not a backup). */
static int backup_ok(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    uint8_t *b = NULL;
    DWORD len = 0;
    if (!read_all(path, &b, &len)) return 0;
    int ok = len >= sizeof HEADER + 0x57c && memcmp(b, HEADER, sizeof HEADER) == 0;   /* header + one full record */
    HeapFree(GetProcessHeap(), 0, b);
    return ok;
}

/* Write a new backup: temp file, then rename without replacing (never overwrites an existing backup). */
static int write_backup(const wchar_t *backup, const uint8_t *data, DWORD len)
{
    wchar_t tmp[MAX_PATH];
    if (lstrlenW(backup) + 12 >= MAX_PATH) return 0;
    lstrcpyW(tmp, backup);
    lstrcatW(tmp, L".mxbfix.tmp");
    if (!write_all(tmp, data, len, CREATE_ALWAYS)) return 0;
    if (!MoveFileExW(tmp, backup, MOVEFILE_WRITE_THROUGH)) { DeleteFileW(tmp); return 0; }
    return 1;
}

/* 1 = repaired, 0 = fine or left alone, -1 = needed repair but couldn't do it safely */
static int check_file(const wchar_t *path, const wchar_t *backup_dir, const wchar_t *name)
{
    uint8_t *b = NULL;
    DWORD len = 0;
    if (!read_all(path, &b, &len)) return 0;
    int result = 0;
    if (memcmp(b, HEADER, sizeof HEADER)) goto done;               /* unknown format: don't touch */
    int nul = -1;
    for (int i = NAME_OFF; i < NAME_END; i++) {
        if (b[i] == 0) { nul = i; break; }
        if (b[i] < 32 || b[i] > 126) goto done;                     /* not a plain name: don't touch */
    }
    if (nul <= NAME_OFF) goto done;                                  /* empty or unterminated name */

    /* What to zero: after the name's terminator; a real series keeps its name and terminator. */
    int keep_from = NAME_END, keep_to = NAME_END;
    if (nul < SERIES_OFF && plausible_series(b, len)) {
        keep_from = SERIES_OFF;
        keep_to = SERIES_OFF;
        while (b[keep_to]) keep_to++;
        keep_to++;                                                   /* keep the terminator too */
    }
    int dirty = 0;
    for (int i = nul + 1; i < NAME_END; i++)
        if (i < keep_from || i >= keep_to) dirty |= b[i];
    if (!dirty) goto done;

    /* Back up the original once; never overwrite an existing backup (it's the oldest copy), and only trust
     * one that is a complete trainer file. */
    wchar_t backup[MAX_PATH], tmp[MAX_PATH];
    ensure_dir(backup_dir);
    if (!path_join(backup, MAX_PATH, backup_dir, name)) { result = -1; goto done; }
    if (GetFileAttributesW(backup) != INVALID_FILE_ATTRIBUTES) {
        if (!backup_ok(backup)) { result = -1; goto done; }
    } else if (!write_backup(backup, b, len)) {
        result = -1;
        goto done;
    }
    for (int i = nul + 1; i < NAME_END; i++)
        if (i < keep_from || i >= keep_to) b[i] = 0;
    if (lstrlenW(path) + 12 >= MAX_PATH) { result = -1; goto done; }
    lstrcpyW(tmp, path);
    lstrcatW(tmp, L".mxbfix.tmp");
    if (!write_all(tmp, b, len, CREATE_ALWAYS) || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp);
        result = -1;
        goto done;
    }
    result = 1;
done:
    HeapFree(GetProcessHeap(), 0, b);
    return result;
}

static int install(fix_t *self)
{
    wchar_t pat[MAX_PATH], profiles[MAX_PATH];
    path_join(profiles, MAX_PATH, g_save_dir, L"profiles\\");
    path_join(pat, MAX_PATH, profiles, L"*");
    int checked = 0, repaired = 0, failed = 0;
    WIN32_FIND_DATAW pd;
    HANDLE ph = FindFirstFileW(pat, &pd);
    if (ph != INVALID_HANDLE_VALUE) {
        do {
            if (!(pd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || pd.cFileName[0] == L'.') continue;
            wchar_t tdir[MAX_PATH], tpat[MAX_PATH], bdir[MAX_PATH];
            path_join(tdir, MAX_PATH, profiles, pd.cFileName);
            path_join(tdir, MAX_PATH, tdir, L"trainers\\");
            path_join(tpat, MAX_PATH, tdir, L"*.trn");
            path_join(bdir, MAX_PATH, g_base_dir, L"backups\\trainers\\");
            path_join(bdir, MAX_PATH, bdir, pd.cFileName);
            WIN32_FIND_DATAW fd;
            HANDLE fh = FindFirstFileW(tpat, &fd);
            if (fh == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                wchar_t path[MAX_PATH];
                path_join(path, MAX_PATH, tdir, fd.cFileName);
                checked++;
                int r = check_file(path, bdir, fd.cFileName);
                char u[MAX_PATH * 3];
                WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, u, sizeof u, NULL, NULL);
                if (r > 0) { repaired++; log_msg("fix %s: repaired %s (backup kept)", self->id, u); }
                if (r < 0) { failed++; log_msg("fix %s: %s needs repair but could not be written safely; left as is", self->id, u); }
            } while (FindNextFileW(fh, &fd));
            FindClose(fh);
        } while (FindNextFileW(ph, &pd));
        FindClose(ph);
    }
    log_msg("fix %s: %d trainer file(s) checked, %d repaired, %d could not be repaired", self->id, checked, repaired, failed);
    return 1;
}

fix_t fix_trainer_repair = {
    .id = "trainer-repair",
    .title = "Back up and clean damaged trainer files at startup",
    .crash_id = "trainer-bad-string",
    .install = install,
    .on_exception = NULL,
};
