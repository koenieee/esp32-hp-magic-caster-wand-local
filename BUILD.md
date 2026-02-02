# ESP32 Spell Detection - Build Guide

## Quick Start

### 1. Install Dependencies

```powershell
# Install PlatformIO
pip install platformio

# Install LittleFS upload tool
pio pkg install --tool "platformio/tool-mklittlefs"
```

### 2. Configure Your Wand

Edit `include/config.h`:

```cpp
// Find your wand's MAC address by scanning BLE devices
#define WAND_MAC_ADDRESS "AA:BB:CC:DD:EE:FF"
```

### 3. Prepare Model File

```powershell
# The model.tflite already exists in the root directory
# Copy it to the data folder for filesystem upload
Copy-Item model.tflite -Destination data\model.tflite
```

### 4. Build & Flash

```powershell
# Build the project
pio run

# Upload filesystem (model file)
pio run --target uploadfs

# Upload firmware
pio run --target upload

# Monitor serial output
pio device monitor
```

## Finding Your Wand's MAC Address

### Option 1: Using nRF Connect App (Recommended)

1. Install **nRF Connect** on your phone (iOS/Android)
2. Turn on your Magic Caster Wand
3. Open nRF Connect and scan for devices
4. Look for "Wizard Wand" or similar device name
5. Note the MAC address (e.g., `A4:C1:38:XX:XX:XX`)

### Option 2: Using ESP32 BLE Scanner

Upload this simple scanner first:

```cpp
#include <NimBLEDevice.h>

void setup() {
  Serial.begin(115200);
  NimBLEDevice::init("");
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setActiveScan(true);
  NimBLEScanResults results = pScan->start(10);
  
  for(int i = 0; i < results.getCount(); i++) {
    NimBLEAdvertisedDevice device = results.getDevice(i);
    Serial.printf("%s | %s\n", 
                 device.getAddress().toString().c_str(),
                 device.getName().c_str());
  }
}

void loop() {}
```

## Expected Serial Output

```
========================================
  ESP32 Magic Wand Gateway
  TensorFlow Lite Spell Detection
========================================
Loading TFLite model from filesystem...
Model file size: 387456 bytes
Model loaded successfully!
Spell detector initialized successfully
Model loaded. Arena used: 87344 bytes
Connecting to wand at A4:C1:38:XX:XX:XX...
Connected to wand, discovering services...
Subscribed to notifications
✓ Connected to wand
IMU streaming started
Battery level: 87%

✓ System ready!
Cast a spell by pressing all 4 wand buttons...

Started spell tracking
Stopped tracking: 347 positions
========================================
🪄 SPELL DETECTED: Wingardium_Leviosa
   Confidence: 99.87%
========================================
```

## Troubleshooting

### Error: "Model file not found!"

```powershell
# Make sure model is in data folder
ls data\model.tflite

# If missing, copy it
Copy-Item model.tflite -Destination data\model.tflite

# Upload filesystem again
pio run --target uploadfs
```

### Error: "Failed to allocate tensor arena"

The model is too large or ESP32 is out of memory.

**Solution 1:** Reduce position buffer in `include/spell_detector.h`:
```cpp
#define MAX_POSITIONS 2048  // Changed from 4096
```

**Solution 2:** Use ESP32-S3 with PSRAM

### Error: "Failed to connect to wand"

1. **Check wand is on:** Press any button to wake it
2. **Check MAC address:** Use BLE scanner to verify
3. **Check range:** Move ESP32 closer to wand
4. **Check BLE:** Make sure wand isn't connected to another device

### Low Confidence Detections

If you see messages like:
```
No spell detected (confidence: 82.45%)
```

**Solution:** Lower the threshold in `include/config.h`:
```cpp
#define SPELL_CONFIDENCE_THRESHOLD 0.85f  // Changed from 0.99f
```

## Component Details

### Implemented Files

- **`include/spell_detector.h`** - All class definitions
- **`src/spell_detector.cpp`** - Full spell detection pipeline (600+ lines)
  - IMU Parser (coordinate transforms)
  - AHRS Tracker (quaternion fusion)
  - Gesture Preprocessor (normalization)
  - TFLite Spell Detector
- **`include/ble_client.h`** - BLE client interface
- **`src/ble_client.cpp`** - BLE communication & orchestration
- **`src/main.cpp`** - Main entry point
- **`include/config.h`** - User configuration

### Memory Layout

```
Flash (4MB):
├── Firmware        ~800 KB
├── Model           ~400 KB
└── LittleFS        Remaining

RAM (512KB):
├── Tensor Arena    100 KB
├── Positions       32 KB
├── BLE Stack       ~80 KB
├── Code/Heap       ~300 KB
```

## Testing Spells

1. **Press all 4 wand buttons simultaneously** to start tracking
2. **Draw your spell gesture** in the air
3. **Release buttons** to trigger detection
4. **Check serial monitor** for results

Try these easy spells first:
- **Lumos** - Simple circle
- **Wingardium Leviosa** - Swish and flick
- **Alohomora** - Vertical line

## Next Steps

### Add Home Assistant Integration

Edit `src/main.cpp` in the `onSpellDetected()` function:

```cpp
#include <WiFi.h>
#include <HTTPClient.h>

void onSpellDetected(const char* spell_name, float confidence) {
    // Send to Home Assistant REST API
    HTTPClient http;
    http.begin("http://homeassistant.local:8123/api/services/input_text/set_value");
    http.addHeader("Authorization", "Bearer YOUR_TOKEN");
    http.addHeader("Content-Type", "application/json");
    
    String payload = "{\"entity_id\":\"input_text.wand_spell\",\"value\":\"" + 
                     String(spell_name) + "\"}";
    http.POST(payload);
    http.end();
}
```

## Performance Metrics

| Component | Time |
|-----------|------|
| IMU Parse | <1ms |
| AHRS Update | ~2ms per sample |
| Preprocessing | ~5ms |
| TFLite Inference | ~10-30ms |
| **Total** | **15-50ms** |

Compare to Python HTTP: 50-150ms

## License

See [LICENSE](../LICENSE)
