@echo off
chcp 65001 >nul
rem Удаляет MagerCam для Windows (программу и виртуальную камеру).
net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set "PS1=%ProgramFiles%\MagerCam\install.ps1"
if not exist "%PS1%" set "PS1=%~dp0install.ps1"
if not exist "%PS1%" set "PS1=%~dp0dist\windows\install.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" -Uninstall
pause
