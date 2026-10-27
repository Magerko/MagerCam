#!/bin/sh
# Собираю magercam.so в Docker под три поколения дистрибутивов, у каждого своя libavcodec:
#   ubuntu24.04  — libavcodec60: Ubuntu 24.04, Linux Mint 22, Pop!_OS 24.04
#   debian13     — libavcodec61: Debian 13, Ubuntu 25.04 и 25.10
#   ubuntu26.04  — libavcodec62: Ubuntu 26.04
# Результат: obs-plugin/build-linux/<вариант>/magercam.so
# Запуск из корня репозитория: sh obs-plugin/linux/build-all.sh
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# Docker Desktop под Windows ждёт путь вида C:/..., а не /c/...
SRC="$ROOT"
command -v cygpath >/dev/null 2>&1 && SRC="$(cygpath -m "$ROOT")"

for v in "ubuntu24.04 ubuntu:24.04" "debian13 debian:trixie" "ubuntu26.04 ubuntu:26.04"; do
	name=${v%% *}
	base=${v#* }
	echo "== $name ($base)"
	docker build -q --build-arg BASE="$base" -t "magercam-build:$name" "$ROOT/obs-plugin/linux" >/dev/null
	MSYS_NO_PATHCONV=1 docker run --rm -v "$SRC:/src" -w /src "magercam-build:$name" sh -c "
		sh obs-plugin/build-linux.sh >/dev/null 2>&1 || sh obs-plugin/build-linux.sh
		mkdir -p obs-plugin/build-linux/$name
		mv obs-plugin/build-linux/magercam.so obs-plugin/build-linux/$name/magercam.so
		ldd obs-plugin/build-linux/$name/magercam.so | grep -E 'avcodec|not found'"
done
