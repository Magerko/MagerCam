#!/bin/sh
# Убираю плагин MagerCam из OBS текущего пользователя
DEST="${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins/magercam"
if [ -d "$DEST" ]; then
	rm -rf "$DEST"
	echo "Плагин удалён: $DEST"
else
	echo "Плагин не установлен"
fi
