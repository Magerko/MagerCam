#!/bin/sh
# Собираю magercam.dll через llvm-mingw, MSVC мне тут не нужен.
# Запуск: sh obs-plugin/build.sh, зависимости сначала готовлю через obs-plugin/setup-deps.sh
#
# Совместимость: заголовки libobs беру от OBS 30.0, иначе OBS старше версии заголовков откажется грузить плагин
# («compiled with newer libobs»). Декодер ffdec.c собираю трижды, под FFmpeg 6.1, 7.1 и 8.0 (avcodec-60/61/62),
# а нужный вариант плагин выбирает при запуске по тому, какой FFmpeg лежит в OBS. Так один DLL идёт в OBS 30 и новее.
# Кроме x64 собираю ARM64: с OBS 31 есть сборка под Windows на ARM, и x64-плагин она не загрузит.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DEPS="${MC_DEPS:-/c/mc-build}"
TC="$(ls -d "$DEPS"/llvm-mingw-*-ucrt-x86_64 | head -1)"
OBS_INC="$DEPS/obs-studio-30/libobs"

# import-библиотека obs.dll под ARM64 из того же списка экспортов, что и для x64
mkdir -p "$DEPS/implib/arm64"
[ -f "$DEPS/implib/arm64/libobs.dll.a" ] || "$TC/bin/llvm-dlltool" -m arm64 -d "$DEPS/implib/obs.def" -l "$DEPS/implib/arm64/libobs.dll.a"

# аргументы: архитектура, папка результата, папка с import-библиотеками (в путях бывают пробелы, поэтому не через set --)
build_arch() {
  CC="$TC/bin/$1-w64-mingw32-clang"
  OUT="$2"
  OBJ="$OUT/obj"
  FLAGS="-O2 -std=gnu11 -Wall -Wno-unknown-pragmas -Wno-missing-braces -D_WIN32_WINNT=0x0A00 -DWIN32 -I$DEPS/inc -I$OBS_INC"
  mkdir -p "$OBJ"
  for v in "6 ffmpeg-n6.1.2" "7 ffmpeg" "8 ffmpeg-n8.0"; do
    n=${v%% *}
    "$CC" $FLAGS -c -DMC_FFV=$n -I"$DEPS/${v#* }" "$HERE/src/ffdec.c" -o "$OBJ/ffdec$n.o"
  done
  "$CC" $FLAGS -c "$HERE/src/magercam.c" -o "$OBJ/magercam.o"
  # FFmpeg не линкую: функции беру через GetProcAddress, поэтому в импортах DLL его нет
  "$CC" -shared "$OBJ/magercam.o" "$OBJ/ffdec6.o" "$OBJ/ffdec7.o" "$OBJ/ffdec8.o" \
    -L"$3" -lobs -lws2_32 -liphlpapi -lgdi32 -luser32 \
    -static-libgcc -Wl,--no-insert-timestamp \
    -o "$OUT/magercam.dll"
  echo "built: $OUT/magercam.dll"
}

build_arch x86_64 "$HERE/build" "$DEPS/implib"
build_arch aarch64 "$HERE/build/arm64" "$DEPS/implib/arm64"
