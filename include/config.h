#ifndef CONFIG_H
#define CONFIG_H

// USB HID Support - disabled to reduce interference with BLE/WiFi
#define USE_USB_HID_DEVICE 0

// Wand BLE UUIDs
#define WAND_SERVICE_UUID "57420001-587e-48a0-974c-544d6163c577"
#define WAND_COMMAND_UUID "57420002-587e-48a0-974c-544d6163c577"
#define WAND_NOTIFY_UUID "57420003-587e-48a0-974c-544d6163c577"
#define WAND_BATTERY_UUID "00002a19-0000-1000-8000-00805f9b34fb"

// Wand MAC address (set your wand's MAC here)
#define WAND_MAC_ADDRESS "C2:BD:5D:3C:67:4E"

// Home Assistant Integration (set to 1 to enable WiFi/MQTT)
#define ENABLE_HOME_ASSISTANT 0 // Disabled by default - enable in config_custom.h

// WiFi Mode: 0 = Access Point (default), 1 = Station (connect to existing WiFi for HA)
#define USE_WIFI_AP_MODE 0 // 0 = standalone AP mode, 1 = connect to existing WiFi

// WiFi Access Point Configuration (when USE_WIFI_AP_MODE = 0)
#define AP_SSID "MagicWand-ESP32"
#define AP_PASSWORD "" // Empty = open network (no password required)
#define AP_CHANNEL 1
#define AP_MAX_CONNECTIONS 4

// WiFi Station Configuration (when USE_WIFI_AP_MODE = 1 - for Home Assistant)
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASSWORD "your_wifi_password"
#define WIFI_PASS WIFI_PASSWORD // Alias for compatibility

// MQTT Configuration (optional - for HA integration)
#define MQTT_SERVER "192.168.1.100"
#define MQTT_PORT 1883
#define MQTT_USER "homeassistant"
#define MQTT_PASSWORD "your_mqtt_password"
#define MQTT_TOPIC_SPELL "wand/spell"
#define MQTT_TOPIC_CONFIDENCE "wand/confidence"

// Spell Detection Configuration
#define SPELL_CONFIDENCE_THRESHOLD 0.99f
// MAX_POSITIONS defined in spell_detector.h
#define SPELL_SAMPLE_COUNT 50

// IMU Sensor Scaling (from Android app)
#define ACCELEROMETER_SCALE 0.00048828125f // Scale to G-forces
#define GYROSCOPE_SCALE 0.0010908308f      // Scale to rad/s
#define GRAVITY_CONSTANT 9.8100004196167f
#define IMU_SAMPLE_PERIOD 0.0042735f // ~234 Hz

// Debug Configuration
#define DEBUG_SERIAL true
#define DEBUG_IMU_DATA false
#define DEBUG_SPELL_TRACKING true

// Include custom configuration if it exists (not version controlled)
// Copy config_custom.h.example to config_custom.h and customize as needed
#if __has_include("config_custom.h")
#include "config_custom.h"
#endif

#endif // CONFIG_H
