#ifndef WAND_COMMANDS_H
#define WAND_COMMANDS_H

#include <stdint.h>
#include <stddef.h>
#include "wand_protocol.h"

// Wand command builder and sender
class WandCommands
{
private:
    uint16_t conn_handle;
    uint16_t command_char_handle;

public:
    WandCommands();

    // Set BLE connection handles
    void setHandles(uint16_t conn_handle, uint16_t command_handle);

    // Check if ready to send commands
    bool isReady() const;

    // IMU streaming control
    bool startIMUStreaming();
    bool stopIMUStreaming();

    // Button threshold configuration
    bool setButtonThreshold(uint8_t button_index, uint8_t threshold);
    bool initButtonThresholds(); // Initialize all 8 button thresholds

    // LED control
    bool setLED(LedGroup group, uint8_t r, uint8_t g, uint8_t b);
    bool clearAllLEDs();

    // Keep-alive (prevents connection timeout)
    bool sendKeepAlive();

    // Macro system
    bool sendMacro(const uint8_t *macro_data, size_t length);

    // Battery reading
    bool requestBatteryLevel();


    // Wand information
    bool requestFirmwareVersion();
    bool requestProductInfo();
private:
    // Send raw command to wand
    bool sendCommand(const uint8_t *data, size_t length);
};

#endif // WAND_COMMANDS_H
