#!/bin/sh
# Собираю magercam.dll через llvm-mingw, MSVC мне тут не нужен.
# Запуск: sh obs-plugin/build.sh, зависимости сначала готовлю через obs-plugin/setup-deps.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DEPS="${MC_DEPS:-/c/mc-build}"
TC="$(ls -d "$DEPS"/llvm-mingw-*-ucrt-x86_64 | head -1)"
CC="$TC/bin/x86_64-w64-mingw32-clang"
mkdir -p "$HERE/build"

"$CC" -O2 -std=gnu11 -shared -Wall -Wno-unknown-pragmas -Wno-missing-braces \
  -D_WIN32_WINNT=0x0A00 -DWIN32 \
  -I"$DEPS/inc" -I"$DEPS/obs-studio/libobs" -I"$DEPS/ffmpeg" \
  "$HERE/src/magercam.c" \
  -L"$DEPS/implib" -lobs -lavcodec-61 -lavutil-59 \
  -lws2_32 -liphlpapi \
  -static-libgcc -Wl,--no-insert-timestamp \
  -o "$HERE/build/magercam.dll"

echo "built: $HERE/build/magercam.dll"
