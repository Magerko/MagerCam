#!/bin/sh
# Собираю magercam.so для OBS на Linux.
# Нужны: gcc, pkg-config, libobs-dev, libavcodec-dev, libavutil-dev, libcairo2-dev
#   Ubuntu/Debian: sudo apt install build-essential pkg-config libobs-dev libavcodec-dev libavutil-dev libcairo2-dev
# Запуск: sh obs-plugin/build-linux.sh
# libobs.pc из OBS 32 не даёт путь к своим заголовкам и включает -Werror, поэтому путь добавляю сам, а -Werror снимаю.
# OBS_INC — папка libobs из исходников OBS 30.0 (так собирает build-all.sh): тогда плагин подойдёт к любому OBS от 30.0.
# Без неё беру заголовки системного libobs-dev, и плагин пойдёт в OBS не старше этой версии.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$HERE/build-linux"

OBS_SYS="$(pkg-config --variable=includedir libobs)/obs"
${CC:-cc} -O2 -std=gnu11 -shared -fPIC -Wall -Wno-unknown-pragmas -Wno-missing-braces \
  -fvisibility=hidden \
  ${OBS_INC:+-I"$OBS_INC"} -I"$OBS_SYS" \
  "$HERE/src/magercam.c" "$HERE/src/ffdec.c" \
  -Wl,--as-needed $(pkg-config --cflags --libs libobs libavcodec libavutil cairo) -lpthread \
  -Wno-error \
  -o "$HERE/build-linux/magercam.so"

echo "built: $HERE/build-linux/magercam.so"
