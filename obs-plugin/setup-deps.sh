#!/bin/sh
# Запуск: sh obs-plugin/setup-deps.sh. Складываю всё для сборки в $MC_DEPS (по умолчанию C:\mc-build):
# llvm-mingw, заголовки libobs 32.1.0 и FFmpeg 7.1.1, а import-библиотеки делаю из DLL установленного OBS.
set -e
DEPS="${MC_DEPS:-/c/mc-build}"
OBS_BIN="${OBS_BIN:-/c/Program Files/obs-studio/bin/64bit}"
LLVM_TAG="${LLVM_TAG:-20261020}"
mkdir -p "$DEPS" && cd "$DEPS"

if ! ls -d llvm-mingw-*-ucrt-x86_64 >/dev/null 2>&1; then
  curl -L -o llvm.zip "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_TAG/llvm-mingw-$LLVM_TAG-ucrt-x86_64.zip"
  unzip -q llvm.zip && rm llvm.zip
fi
[ -d obs-studio ] || { git clone -q --depth 1 --branch 32.1.0 --filter=blob:none --sparse https://github.com/obsproject/obs-studio.git && (cd obs-studio && git sparse-checkout set libobs); }
[ -d ffmpeg ] || { git clone -q --depth 1 --branch n7.1.1 --filter=blob:none --sparse https://github.com/FFmpeg/FFmpeg.git ffmpeg && (cd ffmpeg && git sparse-checkout set libavcodec libavutil); }

mkdir -p inc/libavutil implib
printf '#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n' > inc/obsconfig.h
printf '#ifndef AVUTIL_AVCONFIG_H\n#define AVUTIL_AVCONFIG_H\n#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 1\n#endif\n' > inc/libavutil/avconfig.h

BIN="$(ls -d "$DEPS"/llvm-mingw-*-ucrt-x86_64)/bin"
for d in obs avcodec-61 avutil-59; do
  { echo "LIBRARY $d.dll"; echo "EXPORTS"
    "$BIN/llvm-objdump" -p "$OBS_BIN/$d.dll" | awk '/Ordinal +RVA +Name/{s=1;next} s&&NF>=3{print $3} s&&NF==0{if(n++>0)exit}'
  } > "implib/$d.def"
  "$BIN/llvm-dlltool" -m i386:x86-64 -d "implib/$d.def" -l "implib/lib$d.dll.a"
done
echo "deps ready in $DEPS"
