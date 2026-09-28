#!/usr/bin/env bash
echo "======================================================================"
echo "   FRIDE SMART DASHBOARD AGENT - COMPILE & FLASH FROM SOURCE"
echo "======================================================================"
echo ""

if command -v pio &> /dev/null; then
    PIO_CMD="pio"
elif command -v platformio &> /dev/null; then
    PIO_CMD="platformio"
elif command -v python3 &> /dev/null; then
    PIO_CMD="python3 -m platformio"
else
    echo "[ERROR] PlatformIO or Python 3 not found!"
    exit 1
fi

echo "Available serial ports:"
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || echo "No serial ports found."
echo ""

read -p "Enter device port (or press Enter for auto-detect): " COMPORT

if [ -n "$COMPORT" ]; then
    UPLOAD_OPT="--upload-port $COMPORT"
else
    UPLOAD_OPT=""
fi

echo ""
echo "[1/2] Compiling source code..."
$PIO_CMD run
if [ $? -ne 0 ]; then
    echo "[ERROR] Build failed!"
    exit 1
fi

echo ""
echo "[2/2] Uploading firmware to ESP32-S3..."
$PIO_CMD run -t upload $UPLOAD_OPT
if [ $? -eq 0 ]; then
    echo "======================================================================"
    echo "   UPLOAD SUCCESSFUL! DEVICE IS REBOOTING..."
    echo "======================================================================"
else
    echo "[ERROR] Upload failed!"
    exit 1
fi

