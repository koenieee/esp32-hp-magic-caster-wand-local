#!/bin/bash
# Build and flash ESP32-C6 (no USB HID support)
# Usage: ./build-c6.sh [PORT]
#   PORT: Serial port (default: /dev/ttyACM0)

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Serial port (first argument or default)
PORT="${1:-/dev/ttyACM0}"

echo "========================================="
echo "  ESP32-C6 Build & Flash Script"
echo "  Magic Wand Gateway"
echo "========================================="
echo ""
echo "Target: ESP32-C6 (no USB HID)"
echo "Port: $PORT"
echo ""

# Check if model.tflite exists
if [ ! -f "model.tflite" ]; then
    echo "WARNING: model.tflite not found!"
    echo "Spell detection will be disabled."
    echo "Place model.tflite in this directory to enable it."
    echo ""
fi

# Ensure data directory exists with model
if [ -f "model.tflite" ]; then
    echo "Setting up model file..."
    mkdir -p data
    cp -f model.tflite data/model.tflite
    echo "✓ Model file copied to data/"
    echo ""
fi

# Set target to ESP32-C6
echo "Setting target to ESP32-C6..."
#idf.py set-target esp32c6

# Build the project
echo ""
echo "Building firmware..."
#idf.py build

# Check if build succeeded
if [ $? -ne 0 ]; then
    echo ""
    echo "ERROR: Build failed!"
    exit 1
fi

echo ""
echo "Build completed successfully!"
echo ""
echo "NOTE: USB HID (mouse/keyboard) is NOT available on ESP32-C6"
echo "      Only BLE and web server features will work."
echo ""

# Ask if user wants to flash
read -p "Flash firmware to $PORT? (y/n) " -n 1 -r
echo ""

if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo ""
    echo "Flashing firmware and SPIFFS partition..."
    echo "(Using slower speed for VMware USB compatibility)"
    echo ""
    
    # Flash everything: bootloader, partition table, app, and SPIFFS
    # Use --no-stub and slower baud for VMware USB passthrough compatibility
    idf.py -p "$PORT" -b 115200 flash #--no-stub
    
    # Flash SPIFFS partition with model if it exists
    if [ -f "data/model.tflite" ]; then
        echo ""
        echo "Creating and flashing SPIFFS partition..."
        
        esptool -p /dev/ttyACM0 -b 115200 write_flash 0x310000 model.tflite
        echo "✓ SPIFFS partition flashed with model"
    fi
    
    echo ""
    echo "========================================="
    echo "  Flash Complete!"
    echo "========================================="
    echo ""
    
    # Ask if user wants to monitor
    read -p "Start serial monitor? (y/n) " -n 1 -r
    echo ""
    
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo ""
        echo "Starting serial monitor..."
        echo "Press Ctrl+] to exit"
        echo ""
        idf.py -p "$PORT" monitor
    else
        echo "To monitor serial output later:"
        echo "  idf.py -p $PORT monitor"
        echo ""
        echo "Or use screen:"
        echo "  screen $PORT 115200"
        echo ""
    fi
else
    echo ""
    echo "Skipping flash. To flash later, run:"
    echo "  idf.py -p $PORT flash"
    echo ""
fi

echo "Done!"
