@echo off
chcp 65001 >nul
rem Ставит MagerCam.apk на телефон по USB (нужна включённая "Отладка по USB").
echo Установка MagerCam на телефон через USB...
set "APK=%~dp0dist\MagerCam.apk"
if not exist "%APK%" set "APK=%~dp0MagerCam.apk"
set "ADB=adb"
where adb >nul 2>nul && goto run
set "ADB=%ProgramFiles%\MagerCam\adb\adb.exe"
if exist "%ADB%" goto run
set "ADB=%ProgramData%\obs-studio\plugins\magercam\data\adb\adb.exe"
if exist "%ADB%" goto run
echo adb не найден. Установите MagerCam для Windows или просто скопируйте MagerCam.apk на телефон и откройте его.
pause
exit /b 1
:run
"%ADB%" devices
"%ADB%" install -r "%APK%"
if %ERRORLEVEL% NEQ 0 (
  echo.
  echo Не получилось. Можно просто скопировать MagerCam.apk на телефон и открыть его.
) else (
  "%ADB%" shell am start -n com.magercam.app/.MainActivity >nul
  echo Готово, приложение запущено на телефоне.
)
pause
