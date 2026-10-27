#!/bin/sh
# Ставлю плагин MagerCam в OBS текущего пользователя, root не нужен.
# Рядом лежат сборки под разные libavcodec, выбираю ту, с которой собран установленный OBS.
# Запуск: sh install.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"

if ! command -v obs >/dev/null 2>&1; then
	if command -v flatpak >/dev/null 2>&1 && flatpak info com.obsproject.Studio >/dev/null 2>&1; then
		echo "Найден только OBS из Flatpak. Этот плагин собран под OBS из пакетов дистрибутива"
		echo "(apt install obs-studio или PPA obsproject/obs-studio), во Flatpak он не загрузится."
	else
		echo "OBS не найден. Установите его: sudo apt install obs-studio"
	fi
	exit 1
fi

# libavcodec подгружает libobs, поэтому смотрю, какую версию тянет именно он
LIBOBS="$(ldd "$(command -v obs)" 2>/dev/null | awk '/libobs\.so/ {print $3; exit}')"
AVC="$(ldd "${LIBOBS:-$(command -v obs)}" 2>/dev/null | grep -o 'libavcodec\.so\.[0-9]*' | head -1)"
# если ldd ничего не дал, беру libavcodec, которая есть в системе
[ -n "$AVC" ] || AVC="$(ldconfig -p 2>/dev/null | grep -o 'libavcodec\.so\.6[0-9]' | sort -u | tail -1)"
case "$AVC" in
libavcodec.so.60) VARIANT=ubuntu24.04 ;;
libavcodec.so.61) VARIANT=debian13 ;;
libavcodec.so.62) VARIANT=ubuntu26.04 ;;
*)
	echo "Не понял, с какой libavcodec собран OBS (${AVC:-не найдена})."
	echo "Соберите плагин из исходников: sh obs-plugin/build-linux.sh (см. README)."
	exit 1
	;;
esac

DEST="${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins/magercam"
mkdir -p "$DEST/bin/64bit" "$DEST/data"
cp "$HERE/$VARIANT/magercam.so" "$DEST/bin/64bit/magercam.so"
echo "Плагин установлен ($VARIANT, $AVC): $DEST"

if ! ldd "$DEST/bin/64bit/magercam.so" | grep -q "not found"; then
	echo "Перезапустите OBS и добавьте источник «MagerCam (телефон)»."
else
	echo "Не хватает библиотек:"
	ldd "$DEST/bin/64bit/magercam.so" | grep "not found"
	echo "Обычно помогает: sudo apt install libcairo2"
	exit 1
fi
command -v adb >/dev/null 2>&1 || echo "Для подключения по USB поставьте adb: sudo apt install adb"
