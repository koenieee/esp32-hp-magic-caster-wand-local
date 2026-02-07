#!/bin/bash
# Upload gesture images to SPIFFS partition on ESP32-S3
# This creates a SPIFFS image and flashes it to the device

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GESTURES_DIR="$SCRIPT_DIR/gestures"
SPIFFS_IMAGE="$SCRIPT_DIR/build/spiffs.bin"

# ESP32-S3 SPIFFS configuration (from partitions-s3.csv)
# SPIFFS partition: 0x490000 to 0x800000 (3.6MB)
SPIFFS_OFFSET=0x490000
SPIFFS_SIZE=0x370000
BLOCK_SIZE=4096
PAGE_SIZE=256

echo "=== ESP32 Gesture Images SPIFFS Uploader ==="
echo "Gestures directory: $GESTURES_DIR"
echo "SPIFFS offset: $SPIFFS_OFFSET"
echo "SPIFFS size: $SPIFFS_SIZE"
echo ""

# Check if gestures directory exists
if [ ! -d "$GESTURES_DIR" ]; then
    echo "ERROR: Gestures directory not found: $GESTURES_DIR"
    exit 1
fi

# Count PNG files
PNG_COUNT=$(find "$GESTURES_DIR" -name "*.png" | wc -l)
echo "Found $PNG_COUNT gesture images"

if [ $PNG_COUNT -eq 0 ]; then
    echo "WARNING: No PNG files found in gestures directory"
    exit 1
fi

# Find spiffsgen.py
SPIFFSGEN_PATH=""
if [ -n "$IDF_PATH" ] && [ -f "$IDF_PATH/components/spiffs/spiffsgen.py" ]; then
    SPIFFSGEN_PATH="$IDF_PATH/components/spiffs/spiffsgen.py"
elif command -v idf.py &> /dev/null; then
    # Try to find ESP-IDF through idf.py
    IDF_PY_PATH=$(which idf.py)
    POSSIBLE_IDF_PATH=$(dirname $(dirname "$IDF_PY_PATH"))
    if [ -f "$POSSIBLE_IDF_PATH/components/spiffs/spiffsgen.py" ]; then
        SPIFFSGEN_PATH="$POSSIBLE_IDF_PATH/components/spiffs/spiffsgen.py"
    fi
fi

if [ -z "$SPIFFSGEN_PATH" ]; then
    echo "ERROR: spiffsgen.py not found."
    echo "Make sure ESP-IDF is properly installed and sourced:"
    echo "  source ~/esp/esp-idf/export.sh"
    echo "Or set IDF_PATH environment variable."
    exit 1
fi

echo "Using spiffsgen.py: $SPIFFSGEN_PATH"
echo ""

# Create build directory if it doesn't exist
mkdir -p "$(dirname "$SPIFFS_IMAGE")"

# Create SPIFFS image
echo "Creating SPIFFS image..."
python3 "$SPIFFSGEN_PATH" \
    $SPIFFS_SIZE \
    "$GESTURES_DIR" \
    "$SPIFFS_IMAGE" \
    --page-size $PAGE_SIZE \
    --block-size $BLOCK_SIZE

if [ ! -f "$SPIFFS_IMAGE" ]; then
    echo "ERROR: Failed to create SPIFFS image"
    exit 1
fi

echo "✓ SPIFFS image created: $SPIFFS_IMAGE"

# Flash SPIFFS image to device
PORT="${1:-/dev/ttyACM0}"
echo ""
echo "Flashing SPIFFS image to device..."
echo "Port: $PORT"
echo ""

# Check if port exists
if [ ! -e "$PORT" ]; then
    echo "WARNING: Port $PORT not found!"
    echo "Available ports:"
    ls -1 /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "  No serial ports found"
    echo ""
    read -p "Enter port (or press Enter to skip flashing): " MANUAL_PORT
    if [ -n "$MANUAL_PORT" ]; then
        PORT="$MANUAL_PORT"
    else
        echo "Skipping flash. You can flash manually with:"
        echo "  esptool.py --chip esp32s3 --port $PORT write_flash $SPIFFS_OFFSET $SPIFFS_IMAGE"
        exit 0
    fi
fi

echo "Put device in bootloader mode if needed:"
echo "  1. Hold BOOT button"
echo "  2. Press and release RESET button"
echo "  3. Release BOOT button"
echo ""
set +e  # Disable exit on error for interactive prompt
read -p "Press Enter to start flashing..." || true
set -e  # Re-enable exit on error

if command -v esptool.py &> /dev/null; then
    esptool.py --chip esp32s3 --port "$PORT" write_flash $SPIFFS_OFFSET "$SPIFFS_IMAGE"
else
    echo "ERROR: esptool.py not found. Install with: pip install esptool"
    exit 1
fi

echo ""
echo "=== Upload complete! ==="
echo "Gesture images are now available on the device."
