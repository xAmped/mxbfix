#!/usr/bin/env bash
# Build mxbfix.dlo (64-bit Windows DLL) with Zig 0.13. Works on Windows, Linux and WSL.
#   ./build.sh                 # zig on PATH
#   ZIG=/path/to/zig ./build.sh
set -euo pipefail
cd "$(dirname "$0")"
ZIG="${ZIG:-zig}"
mkdir -p dist
"$ZIG" cc -target x86_64-windows-gnu -shared -O2 -g0 \
  -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -Wno-missing-field-initializers \
  -fno-sanitize=undefined \
  -o dist/mxbfix.dlo src/*.c src/fixes/*.c \
  -lkernel32 -luser32 -ladvapi32 -lversion -lwevtapi -lwinhttp -lshell32 -lbcrypt
rm -f dist/*.lib dist/*.pdb
echo "built dist/mxbfix.dlo"
