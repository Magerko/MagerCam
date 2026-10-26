@echo off
chcp 65001 >nul
rem Устанавливает MagerCam для Windows: программу и виртуальную камеру (нужны права администратора).
rem Работает и из папки проекта (dist\windows), и из распакованного архива релиза.
set "SRC=%~dp0dist\windows"
if not exist "%SRC%\install.ps1" set "SRC=%~dp0."
net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
  echo Запрашиваю права администратора...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%\install.ps1" -Src "%SRC%"
if %ERRORLEVEL% NEQ 0 (
  echo.
  echo [ОШИБКА] Установка не удалась. Подробности: %TEMP%\magercam-install.log
  pause
  exit /b 1
)
echo.
echo Готово. Запускаю MagerCam...
start "" "%ProgramFiles%\MagerCam\MagerCam.exe"
timeout /t 3 >nul
