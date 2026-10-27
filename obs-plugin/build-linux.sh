#!/bin/sh
# Собираю magercam.so для OBS на Linux.
# Нужны: gcc, pkg-config, libobs-dev, libavcodec-dev, libavutil-dev, libcairo2-dev
#   Ubuntu/Debian: sudo apt install build-essential pkg-config libobs-dev libavcodec-dev libavutil-dev libcairo2-dev
# Запуск: sh obs-plugin/build-linux.sh
# libobs.pc из OBS 32 не даёт путь к своим заголовкам и включает -Werror, поэтому путь добавляю сам, а -Werror снимаю
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$HERE/build-linux"

${CC:-cc} -O2 -std=gnu11 -shared -fPIC -Wall -Wno-unknown-pragmas -Wno-missing-braces \
  -fvisibility=hidden \
  "$HERE/src/magercam.c" \
  -Wl,--as-needed $(pkg-config --cflags --libs libobs libavcodec libavutil cairo) -lpthread \
  -I"$(pkg-config --variable=includedir libobs)/obs" -Wno-error \
  -o "$HERE/build-linux/magercam.so"

echo "built: $HERE/build-linux/magercam.so"
