#!/usr/bin/env bash
echo "======================================================================"
echo "   SMART FRIDGE DASHBOARD AGENT - ESP32-S3 FIRMWARE FLASH TOOL"
echo "======================================================================"
echo ""

if ! command -v python3 &> /dev/null; then
    echo "[ERROR] Python 3 not found! Please install Python 3 or use Web Flasher."
    exit 1
fi

python3 -c "import esptool" &> /dev/null
if [ $? -ne 0 ]; then
    echo "[INFO] Installing esptool..."
    pip3 install esptool
fi

echo "Available serial ports:"
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || echo "No /dev/ttyUSB* or /dev/ttyACM* found."
echo ""

read -p "Enter device port (e.g., /dev/ttyUSB0): " COMPORT

if [ -z "$COMPORT" ]; then
    echo "[ERROR] Port cannot be empty!"
    exit 1
fi

echo "[1/1] Flashing firmware to ESP32-S3 ($COMPORT)..."
python3 -m esptool --chip esp32s3 --port "$COMPORT" --baud 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 firmware/bootloader.bin 0x8000 firmware/partitions.bin 0xe000 firmware/boot_app0.bin 0x10000 firmware/firmware.bin

if [ $? -eq 0 ]; then
    echo "======================================================================"
    echo "   FLASH COMPLETED SUCCESSFULLY!"
    echo "======================================================================"
else
    echo "[ERROR] Flash failed! Please hold BOOT button and try again."
fi
