# Changelog

## 0.1.2

- Removed unused debug code. No changes to the fixes.

## 0.1.1

- `trainer-repair` keeps a legitimate series name up to 31 characters (0.1.0 could cut names longer than 15).
- README: clearer about what's verified, what files change and what crash dumps contain.

## 0.1.0

- Fixes: `gate-vertex-pointer` (gate crash), `trainer-series-lookup` and `trainer-repair` (track loading crash),
  `ground-nan-guard` (physics crash), `trainer-cleanup-reset` (cancel crash).
- Crash reports and dumps saved locally. Reports leave out rider names, passwords and the Windows user name;
  dumps are raw memory.
- Each fix can be switched off in `mxbfix.ini`.
