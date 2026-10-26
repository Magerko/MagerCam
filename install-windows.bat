@echo off
chcp 65001 >nul
rem Нужны права администратора: пишу в Program Files и регистрирую камеру в HKLM.
rem install.ps1 ищу и в папке проекта (dist\windows), и рядом с собой в распакованном релизе.
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
