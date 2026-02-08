#ifndef BLE_CLIENT_H
#define BLE_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_bt.h"
#include "spell_detector.h"
#include "wand_commands.h"
#include "wand_protocol.h"
#include "spell_effects.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// Forward declaration
class WebServer;

// Simple circular buffer for fast data copy
#define BUFFER_SIZE 256
#define BUFFER_COUNT 15 // Model is memory-mapped, can use more buffers for reliability

struct NotificationBuffer
{
    uint8_t data[BUFFER_SIZE];
    uint16_t length;
    volatile bool ready; // Ready for processing
};

// Callback types
typedef void (*SpellDetectedCallback)(const char *spell_name, float confidence);
typedef void (*ConnectionCallback)(bool connected);
typedef void (*IMUDataCallback)(float ax, float ay, float az, float gx, float gy, float gz);

class WandBLEClient
{
private:
    uint16_t conn_handle;
    uint16_t notify_char_handle;
    uint16_t command_char_handle;
    uint16_t battery_char_handle;
    int64_t connection_start_time_us; // Timestamp when connection established (microseconds)

    AHRSTracker ahrsTracker;
    SpellDetector spellDetector;
    WandCommands wandCommands;

    SpellDetectedCallback spellCallback;
    ConnectionCallback connectionCallback;
    IMUDataCallback imuCallback;
    WebServer *webServer; // For gesture visualization

    bool connected;
    bool imuStreaming;
    uint8_t lastButtonState;
    uint8_t last_battery_level;
    bool userDisconnectRequested; // Track user-initiated disconnects from web interface
    bool needsInitialization;     // Track if initialization is needed after web-initiated connection

    // Wand information
    char firmware_version[32];
    char serial_number[32];
    char sku[32];
    char device_id[32];
    char wand_type[32];

    // IMU processing buffer (model is memory-mapped, can use larger buffer)
    IMUSample imuBuffer[32];

    // BLE address
    ble_addr_t peer_addr;

    // Circular buffer for fast data copy from BLE callback
    NotificationBuffer circularBuffer[BUFFER_COUNT];
    volatile uint8_t writeIndex;
    volatile uint8_t readIndex;
    TaskHandle_t processingTask;

    // Static callbacks for NimBLE
    static int gap_event_handler(struct ble_gap_event *event, void *arg);

    // Static processing task
    static void processingTaskFunc(void *arg);

    // Internal processing methods
    void processBufferedData();

public:
    WandBLEClient();
    ~WandBLEClient();

    // Process packets (public for callback access)
    void processButtonPacket(const uint8_t *data, size_t length);
    void processIMUPacket(const uint8_t *data, size_t length);
    void processFirmwareVersion(const uint8_t *data, size_t length);
    void processProductInfo(const uint8_t *data, size_t length);

    // Update AHRS tracker (called from main loop, not BLE callback)
    void updateAHRS(const IMUSample &sample);

    // Initialize BLE and load model
    bool begin(const unsigned char *model_data, size_t model_size);

    // Connect to wand
    bool connect(const char *address);

    // Disconnect from wand
    void disconnect();

    // Control auto-reconnect behavior
    void setUserDisconnectRequested(bool requested) { userDisconnectRequested = requested; }
    bool isUserDisconnectRequested() const { return userDisconnectRequested; }

    // Track if initialization is needed after web-initiated connection
    void setNeedsInitialization(bool needs) { needsInitialization = needs; }
    bool getNeedsInitialization() const { return needsInitialization; }

    // IMU streaming control
    bool startIMUStreaming();
    bool stopIMUStreaming();

    // Button threshold configuration
    bool initButtonThresholds();

    // Keep-alive
    bool sendKeepAlive();

    // Play spell effect
    bool playSpellEffect(const char *spell_name);

    // Battery level
    uint8_t getBatteryLevel();
    uint8_t getLastBatteryLevel() const { return last_battery_level; }
    void updateBatteryLevel(uint8_t level) { last_battery_level = level; }

    // Callbacks
    void onSpellDetected(SpellDetectedCallback callback) { spellCallback = callback; }
    void onConnectionChange(ConnectionCallback callback) { connectionCallback = callback; }
    void onIMUData(IMUDataCallback callback) { imuCallback = callback; }

    // Set callbacks (alternative method)
    void setCallbacks(SpellDetectedCallback spell_cb, ConnectionCallback conn_cb, IMUDataCallback imu_cb);

    // Set web server for gesture visualization
    void setWebServer(WebServer *server);

    // BLE scanning
    bool startScan(int duration_seconds = 5);
    void stopScan();
    bool isScanning() const { return scanning; }

    // Wand information
    bool requestWandInfo();
    const char *getFirmwareVersion() const { return firmware_version; }
    const char *getSerialNumber() const { return serial_number; }
    const char *getSKU() const { return sku; }
    const char *getDeviceId() const { return device_id; }
    const char *getWandType() const { return wand_type; }
    const char *getWandMacAddress() const;

    // Status
    bool isStreaming() const { return imuStreaming; }
    bool isConnected() const { return connected; }

    // Internal setters for discovery callbacks
    void setCharHandles(uint16_t notify_handle, uint16_t command_handle);
    void setWandCommandHandles(uint16_t conn_handle, uint16_t command_handle);
    uint16_t getConnHandle() const { return conn_handle; }

private:
    bool scanning;
};

#endif // BLE_CLIENT_H