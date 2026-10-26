@echo off
chcp 65001 >nul
setlocal
rem Плагин беру из папки проекта, а если её нет, то из распакованного архива релиза.
set "SRC=%~dp0dist\obs-plugin\magercam"
if not exist "%SRC%" set "SRC=%~dp0magercam"
set "DST=%ProgramData%\obs-studio\plugins\magercam"
echo Установка плагина MagerCam для OBS...
echo   из:  %SRC%
echo   в:   %DST%
tasklist /FI "IMAGENAME eq obs64.exe" | find /I "obs64.exe" >nul && (
  echo.
  echo [!] OBS сейчас запущен. Плагин загрузится после перезапуска OBS.
)
robocopy "%SRC%" "%DST%" /E /NFL /NDL /NJH /NJS /NP >nul
if %ERRORLEVEL% GEQ 8 (
  echo.
  echo [ОШИБКА] Не удалось скопировать файлы. Запустите этот файл от имени администратора.
  pause
  exit /b 1
)
rem adb для USB в комплект не кладу, скачиваю с официального сайта Google, если его ещё нет нигде
if exist "%DST%\data\adb\adb.exe" goto done
if exist "%ProgramFiles%\MagerCam\adb\adb.exe" goto done
where adb >nul 2>nul && goto done
echo Скачиваю adb (Android platform-tools) для режима USB...
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; [Net.ServicePointManager]::SecurityProtocol='Tls12'; $z=Join-Path $env:TEMP 'mc-pt.zip'; $t=Join-Path $env:TEMP 'mc-pt'; Invoke-WebRequest -UseBasicParsing 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip' -OutFile $z; Expand-Archive -Force $z $t; $d='%DST%\data\adb'; New-Item -ItemType Directory -Force $d | Out-Null; foreach ($f in 'adb.exe','AdbWinApi.dll','AdbWinUsbApi.dll') { Copy-Item -Force (Join-Path $t ('platform-tools\' + $f)) $d }; Remove-Item -Recurse -Force $t,$z" || echo [!] adb не скачан: Wi-Fi работает и без него.
:done
echo.
echo Готово. Перезапустите OBS и добавьте источник "MagerCam (телефон)".
pause
