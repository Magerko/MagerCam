@echo off
chcp 65001 >nul
if not exist "%~dp0install-obs-plugin.ps1" (
  rmdir /S /Q "%ProgramData%\obs-studio\plugins\magercam" && echo Плагин MagerCam удалён. || echo Не удалось удалить: закройте OBS или запустите от имени администратора.
  pause
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-obs-plugin.ps1" -Uninstall -ObsPath "%~1"
pause
