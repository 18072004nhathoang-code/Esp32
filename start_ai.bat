@echo off
chcp 65001 > nul
title ESP32 Mini OS - AI Assistant Service
echo ========================================================
echo   ESP32 Mini OS - AI Backend & Cloudflare Tunnel
echo ========================================================
echo.

cd /d "%~dp0backend"

echo [1/2] Đang khởi chạy AI Backend Server trên cổng 8787...
start "AI Backend Server" cmd /k "node server.js"

echo [2/2] Đang khởi chạy Cloudflare Tunnel HTTPS...
start "Cloudflare Tunnel" cmd /k "npx -y cloudflared tunnel --url http://127.0.0.1:8787"

echo.
echo ========================================================
echo Backend đã được bật tại: http://127.0.0.1:8787
echo Cửa sổ Cloudflare Tunnel sẽ cung cấp đường link HTTPS.
echo ========================================================
echo.
pause
