# ESP32 Spell Detection Architecture

## System Flow

```
┌─────────────────────────────────────────────────────────────────────┐
│                     Magic Caster Wand (BLE)                         │
│  - 6-axis IMU (gyro + accel @ 234Hz)                               │
│  - 4 buttons for spell casting                                      │
│  - Battery monitoring                                               │
└────────────────────┬────────────────────────────────────────────────┘
                     │ BLE Notifications (0x2C = IMU, 0x0A = Button)
                     ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    ESP32-C6 Gateway (512KB RAM)                     │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │ WandBLEClient (ble_client.cpp)                               │  │
│  │  - NimBLE stack                                               │  │
│  │  - Connection management                                      │  │
│  │  - Notification handler                                       │  │
│  └────┬───────────────────────────────────┬─────────────────────┘  │
│       │ IMU Packets                       │ Button Events           │
│       ▼                                   ▼                         │
│  ┌──────────────────────┐     ┌──────────────────────────┐        │
│  │ IMUParser            │     │ Button State Detector    │        │
│  │ - Extract 6DOF data  │     │ - All buttons pressed?   │        │
│  │ - Apply scaling      │     │ - Start/Stop tracking    │        │
│  │ - Transform coords   │     └────────┬─────────────────┘        │
│  └────────┬─────────────┘              │                           │
│           │ IMUSample[]                │                           │
│           ▼                            ▼                           │
│  ┌──────────────────────────────────────────────────────┐         │
│  │ AHRSTracker (AHRS Quaternion Fusion)                 │         │
│  │ - Madgwick algorithm                                 │         │
│  │ - Sensor fusion (gyro + accel)                       │         │
│  │ - Maintains quaternion state (q0,q1,q2,q3)          │         │
│  │ - Tracks 3D position when button pressed             │         │
│  │ - Buffer: 4096 positions (32KB)                      │         │
│  └────────────────────────┬─────────────────────────────┘         │
│                           │ Position2D[] (raw trajectory)          │
│                           ▼                                        │
│  ┌──────────────────────────────────────────────────────┐         │
│  │ GesturePreprocessor                                  │         │
│  │ - Trim stationary segments                           │         │
│  │ - Resample to 50 points                              │         │
│  │ - Normalize to [0,1] bounding box                    │         │
│  │ - Flatten to (50, 2) array                           │         │
│  └────────────────────────┬─────────────────────────────┘         │
│                           │ float[100] (50 x,y pairs)              │
│                           ▼                                        │
│  ┌──────────────────────────────────────────────────────┐         │
│  │ SpellDetector (TensorFlow Lite Micro)                │         │
│  │ - Model: spell_model.tflite (~400KB)                 │         │
│  │ - Input: (1, 50, 2) float32                          │         │
│  │ - Output: (1, 71) float32 probabilities              │         │
│  │ - Tensor arena: 100KB                                │         │
│  │ - Confidence threshold: 0.99                         │         │
│  └────────────────────────┬─────────────────────────────┘         │
│                           │ Spell name + confidence                │
│                           ▼                                        │
│  ┌──────────────────────────────────────────────────────┐         │
│  │ Callback: onSpellDetected()                          │         │
│  │ - Print to serial                                    │         │
│  │ - (TODO) Send to Home Assistant via MQTT/HTTP        │         │
│  └──────────────────────────────────────────────────────┘         │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
                     │ MQTT/HTTP (Optional)
                     ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      Home Assistant                                 │
│  - Receive spell events                                            │
│  - Trigger automations                                             │
│  - Display in UI                                                   │
└─────────────────────────────────────────────────────────────────────┘
```

## Processing Pipeline Details

### Phase 1: IMU Data Acquisition
```
BLE Packet (0x2C) → IMUParser
├─ Parse: 6 × int16 (gyro_x,y,z, accel_x,y,z)
├─ Scale: raw → physical units (rad/s, G-forces)
└─ Transform: Android → standard coordinate frame
    Output: IMUSample { gyro(x,y,z), accel(x,y,z) }
```

### Phase 2: AHRS Quaternion Fusion
```
IMUSample → AHRSTracker.update()
├─ Normalize accelerometer vector
├─ Estimate gravity from current quaternion
├─ Compute error (cross product)
├─ Apply feedback to gyro rates (beta = 0.1)
├─ Integrate gyro → quaternion derivative
├─ Update quaternion (dt = 0.0042735s)
└─ Normalize quaternion
    State: Quaternion(q0, q1, q2, q3)

When tracking:
├─ Compute relative quaternion vs start
├─ Convert to Euler angles (roll, pitch, yaw)
└─ Project to 2D position (pitch, roll)
    Output: Position2D(x, y)
```

### Phase 3: Gesture Preprocessing
```
Position2D[N] → GesturePreprocessor.preprocess()
├─ Trim stationary segments
│   ├─ Remove tail: compare 40 samples apart, threshold = 8mm
│   └─ Remove head: compare 10 samples apart, threshold = 8mm
├─ Resample to 50 points
│   └─ Linear interpolation: idx = i * (N-1) / 49
├─ Find bounding box (min/max x,y)
├─ Normalize to [0,1]
│   └─ x_norm = (x - min_x) / max(width, height)
└─ Flatten to array
    Output: float[100] = [x0,y0, x1,y1, ..., x49,y49]
```

### Phase 4: TensorFlow Lite Inference
```
float[100] → SpellDetector.detect()
├─ Copy to input tensor (1, 50, 2)
├─ interpreter->Invoke()
├─ Read output tensor (1, 71)
├─ Find argmax(probabilities)
└─ Check confidence >= threshold
    Output: SPELL_NAMES[argmax] or nullptr
```

## Memory Map

```
Flash (4MB):
├─ 0x00000000 - 0x000FFFFF  Bootloader + Partition Table (1MB)
├─ 0x00100000 - 0x003FFFFF  Firmware (~800KB)
│   ├─ main.cpp entry point
│   ├─ spell_detector.cpp (core algorithms)
│   ├─ ble_client.cpp (BLE stack)
│   └─ TFLite Micro library
├─ 0x00400000 - 0x0047FFFF  LittleFS Filesystem (512KB)
│   └─ model.tflite (~400KB)
└─ 0x00480000 - 0x003FFFFF  Reserved

RAM (512KB):
├─ 0x00000000 - 0x00018FFF  Tensor Arena (100KB)
├─ 0x00019000 - 0x00020FFF  Position Buffer (32KB, 4096×8B)
├─ 0x00021000 - 0x00034FFF  BLE Stack (80KB, NimBLE)
├─ 0x00035000 - 0x00040000  Code/Heap/Stack (~300KB)
│   ├─ AHRSTracker quaternion state
│   ├─ IMU sample buffers
│   ├─ BLE characteristics
│   └─ Function call stack
└─ Total Used: ~212KB / 512KB (41%)
```

## File Structure

```
esp32-wand-gateway/
├── include/
│   ├── config.h              # User configuration (MAC, WiFi, MQTT)
│   ├── spell_detector.h      # All class definitions (450 lines)
│   └── ble_client.h          # BLE client interface (80 lines)
├── src/
│   ├── main.cpp              # Entry point & setup (160 lines)
│   ├── spell_detector.cpp    # Full pipeline implementation (650 lines)
│   │   ├── SPELL_NAMES[71]
│   │   ├── IMUParser::parse()
│   │   ├── AHRSTracker (Madgwick AHRS)
│   │   ├── GesturePreprocessor
│   │   └── SpellDetector (TFLite)
│   └── ble_client.cpp        # BLE orchestration (250 lines)
├── data/
│   └── model.tflite          # TFLite model (copied here)
├── model.tflite              # Original model file
├── platformio.ini            # Build configuration
├── partitions.csv            # Flash partitions
├── README.md                 # Main documentation
├── BUILD.md                  # Build instructions
├── ARCHITECTURE.md           # This file
├── setup_model.bat/.sh       # Model copy scripts
└── upload_model.py           # Filesystem upload helper
```

## Timing Analysis

| Component | Frequency | Time per Call | CPU Time |
|-----------|-----------|---------------|----------|
| BLE Notification | ~234 Hz | - | Interrupt |
| IMU Parse | ~234 Hz | <1ms | ~234ms/s |
| AHRS Update | ~234 Hz | ~2ms | ~468ms/s |
| Button Check | ~50 Hz | <1ms | ~50ms/s |
| Preprocessing | ~1 Hz | ~5ms | ~5ms/s |
| TFLite Inference | ~1 Hz | 10-30ms | 10-30ms/s |
| **Total CPU** | - | - | **~77%** |

Remaining 23% for BLE stack, WiFi, MQTT, etc.

## Key Algorithms

### Madgwick AHRS (Quaternion Fusion)
- **Purpose:** Fuse gyro + accel → stable 3D orientation
- **Input:** ω (gyro rad/s), a (accel G-forces)
- **Output:** q (quaternion)
- **Method:** 
  1. Integrate gyro → quaternion derivative
  2. Estimate gravity error from accel
  3. Apply feedback correction (β = 0.1)
  4. Normalize quaternion
- **Frequency:** 234 Hz
- **Accuracy:** ±2° typical

### Fast Inverse Square Root
```cpp
float invSqrt(float x) {
    float y = x;
    long i = *(long*)&y;
    i = 0x5f3759df - (i >> 1);  // Magic constant
    y = *(float*)&i;
    y = y * (1.5f - (0.5f * x * y * y));  // Newton iteration
    return y;
}
```
Used for vector normalization (10x faster than sqrt)

## 71 Spell Classes

See SPELL_NAMES array in `spell_detector.cpp`:
- Index 0: "The_Force_Spell"
- Index 18: "Alohomora"
- Index 26: "Expelliarmus"
- Index 27: "Expecto_Patronum"
- Index 52: "Wingardium_Leviosa"
- Index 55: "Lumos"
- Index 56: "Stupefy"
- ... and 64 more!

## Performance Comparison

| Metric | ESP32 Local | Python HTTP |
|--------|-------------|-------------|
| **Latency** | 15-50ms | 50-150ms |
| **Network** | 10 B/s | 11.75 KB/s |
| **HA CPU** | ~5% | ~25% |
| **Power** | 150mW | 500mW+ |
| **Offline** | ✅ Yes | ❌ No |

## Future Optimizations

1. **Quantization:** Convert model to INT8 (4x smaller, 2x faster)
2. **PSRAM:** Use external RAM for larger buffers
3. **Multi-core:** Run TFLite on Core 1, BLE on Core 0
4. **DMA:** Use DMA for memory copies
5. **Custom Ops:** Optimize critical TFLite ops in assembly
