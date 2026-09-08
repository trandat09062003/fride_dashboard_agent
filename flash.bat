@echo off
chcp 65001 > nul
echo ======================================================================
echo    SMART FRIDGE DASHBOARD AGENT - ESP32-S3 FIRMWARE FLASH TOOL
echo ======================================================================
echo.

where python >nul 2>nul
if %errorlevel% neq 0 (
    echo [ERROR] Không tìm thấy Python trên máy tính của bạn!
    echo Vui lòng cài đặt Python (https://www.python.org/downloads/)
    echo Hoặc nạp trực tiếp qua trình duyệt web tại: https://esp.huhn.me/
    pause
    exit /b 1
)

python -c "import esptool" >nul 2>nul
if %errorlevel% neq 0 (
    echo [INFO] Đang cài đặt thư viện esptool...
    pip install esptool
)

echo Danh sách các cổng COM khả dụng:
powershell -Command "[System.IO.Ports.SerialPort]::getportnames()"
echo.

set /p COMPORT="Nhập cổng COM của mạch ESP32-S3 (ví dụ: COM3, COM8): "

if "%COMPORT%"=="" (
    echo [ERROR] Bạn chưa nhập cổng COM!
    pause
    exit /b 1
)

echo.
echo [1/1] Đang nạp firmware vào ESP32-S3 (%COMPORT%)...
python -m esptool --chip esp32s3 --port %COMPORT% --baud 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 firmware/bootloader.bin 0x8000 firmware/partitions.bin 0xe000 firmware/boot_app0.bin 0x10000 firmware/firmware.bin

if %errorlevel% equ 0 (
    echo.
    echo ======================================================================
    echo    NẠP FIRMWARE THÀNH CÔNG! HỆ THỐNG ĐANG KHỞI ĐỘNG...
    echo ======================================================================
) else (
    echo.
    echo [ERROR] Nạp thất bại! Vui lòng nhấn giữ nút BOOT trên mạch và thử lại.
)

pause
