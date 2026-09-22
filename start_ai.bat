@echo off
chcp 65001 > nul
title ESP32 Mini OS - YouTube Audio Proxy
echo ========================================================
echo   ESP32 Mini OS - YouTube Audio Proxy (Local LAN)
echo ========================================================
echo.

cd /d "%~dp0backend"
set "HOST=0.0.0.0"
set "PORT=8787"

if not defined YT_DLP_PATH (
    if exist "%USERPROFILE%\.platformio\penv\Scripts\yt-dlp.exe" (
        set "YT_DLP_PATH=%USERPROFILE%\.platformio\penv\Scripts\yt-dlp.exe"
    )
)

echo Backend: 0.0.0.0:%PORT% (ESP32 can connect over LAN)
echo YOUTUBE_STREAM_ENDPOINT must use this PC's LAN IPv4, for example:
echo   http://192.168.1.28:%PORT%/youtube/stream
echo.
if defined YT_DLP_PATH (
    echo yt-dlp: %YT_DLP_PATH%
) else (
    where yt-dlp >nul 2>&1
    if errorlevel 1 (
        echo [WARNING] yt-dlp not found. Install: py -m pip install -U yt-dlp
    ) else (
        echo yt-dlp: PATH
    )
)
echo.
echo Starting YouTube proxy...
node server.js
pause
