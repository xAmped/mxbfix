# Changelog

## 0.1.8

- Fixed a physics blow-up of your bike when it touches a thin fence, mostly after a fall (`mxbikes.exe+0x1a29ae`).
- Freezes are now reported as freezes, with how long the game stopped responding, instead of just "didn't close properly".
- CPU values from the moment of a crash stay on your PC; they're no longer sent.
- Crashes prevented counts each crash once, and nothing for a fix you turned off.
- Keeps the newest 3 crash dumps instead of 10 (about 60 MB each). Existing settings files keep their number.
- Crash reports note when mxbmrp3 recorded the same crash.

## 0.1.7

- The MXBFix page now opens on online servers too: press Esc and move the mouse.

## 0.1.6

- Added an in-game page in the pause menu: update status, crashes prevented and each fix.
- Install updates from the game with one click. Updates are signed.
- Added crash reports and daily crash counts, which help find new crashes. You choose during a short setup and can turn them off anytime.

## 0.1.5

- Better logging of bad rider positions. No changes to the fixes.

## 0.1.4

- Crash reports no longer include riders' race numbers.
- More reliable crash reports.

## 0.1.3

- Cleaner log and settings file. No changes to the fixes.

## 0.1.2

- Removed unused debug code. No changes to the fixes.

## 0.1.1

- `trainer-repair` keeps a legitimate series name up to 31 characters (0.1.0 could cut names longer than 15).

## 0.1.0

- Fixes: `gate-vertex-pointer` (gate crash), `trainer-series-lookup` and `trainer-repair` (track loading crash),
  `ground-nan-guard` (physics crash), `trainer-cleanup-reset` (cancel crash).
- Crash reports and dumps saved locally. Reports leave out rider names, passwords and the Windows user name;
  dumps are raw memory.
- Each fix can be switched off in `mxbfix.ini`.
