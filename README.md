# ESP32-s3 Wand Spell Gateway

Edge ML inference gateway for Harry Potter Magic Caster Wand using ESP32-s3.

## Based on ##

This project is based on the hard work of: https://github.com/eigger/hass-magic-caster-wand

I started with a fork and then I thought of creating my own repository because it's very different (other platform, other usecase).



Note this is only working at the moment for ESP32-S3 device. It could work for C6, but there were disconnect issues in the bluetooth stack. C6 will not support USB hid device. And S3 needs addional PSRAM for loading the tensorflow model.
All other ESP32 devices are not supported at the moment (some of them could work..)


## Build and flash

First source espressif:
source ~/esp/esp-idf/export.sh 


idf.py set-target esp32s3
idf.py build
idf.py flash

(monitor CDC device, mixed with USB HID)
idf.py -p /dev/ttyACM0 monitor --no-reset 

Flash model on correct partition (See partitions-s3)

esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 460800 write_flash 0x410000 model.tflite


## Overview

This project runs TensorFlow Lite spell detection **directly on the ESP32-s3**, eliminating HTTP-based inference. It connects to the wand via BLE, processes IMU data locally using AHRS quaternion fusion, and detects spells on-device from 71 classes.

## Architecture

```
Magic Wand (BLE) → ESP32-s3 Gateway → Home Assistant
   235 bytes/pkt      (TFLite local)     10 bytes/result
   @ 234 Hz           15-50ms latency     @ ~1 Hz
   IMU Data    →  AHRS + Position  →  Preprocessor  →  TFLite Model
``` 

## Benefits

- **99.9% less network traffic**: 11.75 KB/s → 10 bytes/s
- **3-5x lower latency**: 15-50ms vs 50-150ms (HTTP)
- **95% less HA CPU usage**: No Python IMU processing
- **Fully local**: No external server required
- **More reliable**: Standalone operation

## Hardware Requirements

- ESP32-s3 DevKit (recommended) or ESP32-C3/ESP32-S3
- 512 KB RAM minimum
- 4 MB Flash minimum
- BLE 5.0 support

## Project Structure

```
esp32-wand-gateway/
├── README.md                    # This file
├── platformio.ini              # PlatformIO configuration
├── include/
│   ├── spell_tracker.h         # AHRS + position tracking
│   ├── spell_detector.h        # TFLite inference wrapper
│   └── config.h                # Configuration constants
├── src/
│   ├── main.cpp                # Main application loop
│   ├── spell_tracker.cpp       # SpellTracker implementation
│   ├── spell_detector.cpp      # TFLite detector implementation
│   └── ble_client.cpp          # BLE wand communication
└── models/
    └── spell_model.tflite      # Trained TFLite model (float32)
```

## Getting Started

See individual component READMEs for implementation details.

## Model Requirements

- Input: (1, 50, 2) float32 - normalized 2D positions
- Output: (1, 71) float32 - spell class probabilities
- Size: < 400 KB recommended for ESP32-s3
- Type: float32 (INT8 quantization optional for performance)

## Status

🚧 **In Development** - Core components being implemented