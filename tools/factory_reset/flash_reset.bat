@echo off
:: ============================================================
:: ESP32-S3 Factory Reset Flash Script
:: Board : ESP32-S3 N16R8 (16MB Flash)
:: Tool  : esptool (bundled với PlatformIO)
:: ============================================================
:: Cách dùng:
::   1. Kết nối ESP32-S3 vào máy tính
::   2. Đổi COM_PORT bên dưới cho đúng cổng COM của bạn
::   3. Chạy file này (double-click hoặc cmd)
:: ============================================================

setlocal

:: ── CẤU HÌNH: đổi cổng COM cho đúng ──────────────────────────
set COM_PORT=COM3
:: ──────────────────────────────────────────────────────────────

set SCRIPT_DIR=%~dp0
set ESPTOOL=esptool.py

:: Kiểm tra esptool có trong PATH không (PlatformIO đã cài sẵn)
where esptool.py >nul 2>&1
if errorlevel 1 (
    echo [WARN] esptool.py khong co trong PATH.
    echo        Thu tim trong PlatformIO packages...
    :: Fallback: dung esptool tu Python pip
    set ESPTOOL=python -m esptool
)

echo.
echo ============================================================
echo   ESP32-S3 Factory Reset - Flash Blank Partitions
echo   Port : %COM_PORT%
echo ============================================================
echo.

:: Kiểm tra file .bin tồn tại
if not exist "%SCRIPT_DIR%nvs_blank.bin" (
    echo [ERROR] Khong tim thay nvs_blank.bin
    echo         Hay chay generate_reset_bins.py truoc!
    pause & exit /b 1
)
if not exist "%SCRIPT_DIR%otadata_blank.bin" (
    echo [ERROR] Khong tim thay otadata_blank.bin
    echo         Hay chay generate_reset_bins.py truoc!
    pause & exit /b 1
)

echo [STEP 1] Flash NVS blank  (offset 0x9000, 20KB)...
%ESPTOOL% --chip esp32s3 --port %COM_PORT% --baud 921600 ^
    write_flash 0x9000 "%SCRIPT_DIR%nvs_blank.bin"
if errorlevel 1 goto :error

echo.
echo [STEP 2] Flash OTA data blank  (offset 0xE000, 8KB)...
%ESPTOOL% --chip esp32s3 --port %COM_PORT% --baud 921600 ^
    write_flash 0xE000 "%SCRIPT_DIR%otadata_blank.bin"
if errorlevel 1 goto :error

:: Tùy chọn: flash SPIFFS blank
if exist "%SCRIPT_DIR%spiffs_blank.bin" (
    echo.
    set /p FLASH_SPIFFS=[STEP 3] Flash SPIFFS blank ~7MB? [y/N]: 
    if /i "%FLASH_SPIFFS%"=="y" (
        %ESPTOOL% --chip esp32s3 --port %COM_PORT% --baud 921600 ^
            write_flash 0x910000 "%SCRIPT_DIR%spiffs_blank.bin"
        if errorlevel 1 goto :error
    )
)

echo.
echo ============================================================
echo   HOAN TAT! ESP32-S3 da duoc reset ve mac dinh.
echo   Board se tu dong khoi dong lai.
echo ============================================================
goto :end

:error
echo.
echo [ERROR] Flash that bai! Kiem tra:
echo   - Dung cong COM: %COM_PORT%  ^(Device Manager de xem^)
echo   - ESP32-S3 o che do DOWNLOAD: giu BOOT + nhan RESET
echo   - Driver USB da cai ^(CP2102 / CH340 / USB-CDC^)
echo.

:end
pause
endlocal
