@echo off
chcp 65001 > nul
echo ======================================================================
echo    FRIDE SMART DASHBOARD AGENT - BIÊN DỊCH ^& NẠP CODE TỪ NGUỒN
echo ======================================================================
echo.

where pio >nul 2>nul
if %errorlevel% equ 0 (
    set PIO_CMD=pio
) else (
    where python >nul 2>nul
    if %errorlevel% equ 0 (
        python -m platformio --version >nul 2>nul
        if %errorlevel% equ 0 (
            set PIO_CMD=python -m platformio
        ) else (
            echo [INFO] Đang cài đặt PlatformIO CLI...
            pip install platformio
            set PIO_CMD=python -m platformio
        )
    ) else (
        echo [ERROR] Không tìm thấy Python hoặc PlatformIO!
        echo Vui lòng cài đặt PlatformIO hoặc mở dự án trong VS Code.
        pause
        exit /b 1
    )
)

echo Danh sách các cổng COM khả dụng:
powershell -Command "[System.IO.Ports.SerialPort]::getportnames()"
echo.

set /p COMPORT="Nhập cổng COM của mạch ESP32-S3 (nhấn Enter để dùng COM8 mặc định / tự động): "

if "%COMPORT%"=="" (
    set UPLOAD_OPT=
) else (
    set UPLOAD_OPT=--upload-port %COMPORT%
)

echo.
echo [1/2] Đang biên dịch mã nguồn dự án...
%PIO_CMD% run
if %errorlevel% neq 0 (
    echo.
    echo [ERROR] Biên dịch thất bại! Vui lòng kiểm tra thông báo lỗi ở trên.
    pause
    exit /b 1
)

echo.
echo [2/2] Đang nạp firmware trực tiếp vào ESP32-S3...
%PIO_CMD% run -t upload %UPLOAD_OPT%
if %errorlevel% neq 0 (
    echo.
    echo [ERROR] Nạp code thất bại! Hãy kiểm tra cáp USB hoặc nhấn giữ nút BOOT khi cắm cáp.
    pause
    exit /b 1
)

echo.
echo ======================================================================
echo    NẠP CODE THÀNH CÔNG! THIẾT BỊ ĐANG KHỞI ĐỘNG...
echo ======================================================================
echo.
pause

