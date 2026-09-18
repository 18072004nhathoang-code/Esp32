@echo off
chcp 65001 > nul
title ESP32 Mini OS - AI Assistant Service
echo ========================================================
echo   ESP32 Mini OS - AI Backend Service (Local LAN)
echo ========================================================
echo.
echo Địa chỉ máy chủ nội bộ: http://192.168.1.28:8787
echo.

cd /d "%~dp0backend"

echo Đang khởi chạy AI Backend...
node server.js

pause
