# Web Visualizer Setup

## Configuration

1. Edit `src/main.cpp` and change WiFi credentials:
```cpp
#define WIFI_SSID "YourWiFiSSID"
#define WIFI_PASS "YourWiFiPassword"
```

2. Upload filesystem (contains index.html):
```bash
pio run --target uploadfs
```

3. Build and upload firmware:
```bash
pio run --target upload
```

## Usage

Once connected, open your browser to:
- `http://esp32.local/` (if mDNS works)
- Or use the IP address shown in serial monitor

The visualizer shows:
- Real-time accelerometer vector (cyan arrow)
- Gyroscope rotation indicator (magenta line)
- Live numerical values for all 6 DOF
- Acceleration magnitude and angular velocity

## IMU Data Format

The data is compatible with TensorFlow model input after AHRS processing:
- Accelerometer: G-forces (X, Y, Z)
- Gyroscope: rad/s (X, Y, Z)
- Sample rate: ~100 Hz
- Batch size: ~19 samples per packet

This matches the Python implementation used in the Home Assistant integration.
