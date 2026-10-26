#!/bin/sh
# Builds MagerCam for Windows with llvm-mingw (see obs-plugin/setup-deps.sh):
#   build/magercam-vcam.dll  virtual camera media source (loaded by the Frame Server service)
#   build/MagerCam.exe       client: phone -> preview, virtual camera, audio
set -e
cd "$(dirname "$0")"
DEPS="${MC_DEPS:-/c/mc-build}"
BIN="$(ls -d "$DEPS"/llvm-mingw-*-ucrt-x86_64)/bin"
mkdir -p build
"$BIN/x86_64-w64-mingw32-clang++" -O2 -std=c++17 -shared -static -Wall -Wno-unknown-pragmas \
	-o build/magercam-vcam.dll vcam/vcam.cpp vcam/vcam.def \
	-lmfplat -lmfuuid -lole32 -luuid -ladvapi32 -lgdi32 -luser32
echo "built: build/magercam-vcam.dll"
if [ -d client ]; then
	"$BIN/x86_64-w64-mingw32-windres" client/client.rc -O coff -o build/client.res
	"$BIN/x86_64-w64-mingw32-clang" -O2 -std=c11 -Wall -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -mwindows -municode -static \
		-o build/MagerCam.exe client/*.c build/client.res \
		-lmfplat -lmfuuid -lmf -lole32 -luuid -lws2_32 -liphlpapi -lcomctl32 -lgdi32 -luser32 \
		-ladvapi32 -lshell32 -lshlwapi -lcomdlg32 -lwinmm -lpropsys
	echo "built: build/MagerCam.exe"
fi

# stage the distributable folder
DIST=../dist/windows
mkdir -p "$DIST/adb"
cp build/MagerCam.exe build/magercam-vcam.dll install.ps1 "$DIST/"
cp ../dist/obs-plugin/magercam/data/adb/* "$DIST/adb/" 2>/dev/null || true
echo "staged: dist/windows"
