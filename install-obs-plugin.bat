@echo off
chcp 65001 >nul
rem Вся логика в install-obs-plugin.ps1: ищу все OBS (обычную, Steam, портативную) и кладу плагин туда, где его ищет каждая.
rem Папку портативной OBS можно перетащить на этот файл.
if not exist "%~dp0install-obs-plugin.ps1" (
  echo Сначала распакуйте архив целиком: правой кнопкой по архиву, «Извлечь все…».
  echo Потом запустите install-obs-plugin.bat из распакованной папки.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-obs-plugin.ps1" -ObsPath "%~1"
pause
