#!/bin/bash
# Flash TensorFlow Lite model directly to flash partition
# No filesystem needed - uses memory-mapped flash

MODEL_FILE="data/model.tflite"
PARTITION_OFFSET="0x310000"
SERIAL_PORT="${1:-/dev/ttyACM0}"

if [ ! -f "$MODEL_FILE" ]; then
    echo "❌ Error: Model file not found: $MODEL_FILE"
    exit 1
fi

MODEL_SIZE=$(stat -c%s "$MODEL_FILE")
echo "📊 Model size: $MODEL_SIZE bytes ($(echo "scale=1; $MODEL_SIZE/1024" | bc) KB)"
echo "📍 Flashing to partition 'model' at offset $PARTITION_OFFSET"
echo "🔌 Serial port: $SERIAL_PORT"
echo ""

# Flash the raw model data directly to the partition
esptool.py --chip esp32c6 --port "$SERIAL_PORT" write_flash "$PARTITION_OFFSET" "$MODEL_FILE"

if [ $? -eq 0 ]; then
    echo ""
    echo "✅ Model flashed successfully!"
    echo "   The ESP32 will now read it directly from flash (no RAM used)"
    echo ""
    echo "🚀 Next step: Build and flash firmware"
    echo "   idf.py build flash monitor"
else
    echo ""
    echo "❌ Flash failed!"
    exit 1
fi
