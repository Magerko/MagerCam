@echo off
chcp 65001 >nul
rmdir /S /Q "%ProgramData%\obs-studio\plugins\magercam" && echo Плагин MagerCam удалён. || echo Не удалось удалить (закройте OBS или запустите от администратора).
pause
