#ifndef WAND_PROTOCOL_H
#define WAND_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

// Forward declaration
struct IMUSample;

// Forward declaration
struct IMUSample;

// Wand BLE Protocol Constants
// Service and characteristic UUIDs
#define WAND_SERVICE_UUID "57420001-587e-48a0-974c-544d6163c577"
#define WAND_COMMAND_UUID "57420002-587e-48a0-974c-544d6163c577"
#define WAND_NOTIFY_UUID "57420003-587e-48a0-974c-544d6163c577"
#define BATTERY_UUID "00002a19-0000-1000-8000-00805f9b34fb"

// Message IDs (Commands sent to wand)
#define MSG_FIRMWARE_VERSION_READ 0x00
#define MSG_CHALLENGE 0x01
#define MSG_PAIR_WITH_ME 0x03
#define MSG_BOX_ADDRESS_READ 0x09
#define MSG_WAND_PRODUCT_INFO_READ 0x0E
#define MSG_IMUFLAG_SET 0x30
#define MSG_IMUFLAG_RESET 0x31
#define MSG_LIGHT_CONTROL_CLEAR_ALL 0x40
#define MSG_LIGHT_CONTROL_SET_LED 0x42
#define MSG_BUTTON_SET_THRESHOLD 0xDC
#define MSG_BUTTON_READ_THRESHOLD 0xDD
#define MSG_BUTTON_CALIBRATION_BASELINE 0xFB
#define MSG_IMU_CALIBRATION 0xFC
#define MSG_FACTORY_UNLOCK 0xFE

// Response IDs (Received from wand)
#define RESP_FIRMWARE_VERSION 0x00
#define RESP_CHALLENGE 0x01
#define RESP_PONG 0x02
#define RESP_BOX_ADDRESS 0x09
#define RESP_BUTTON_PAYLOAD 0x10
#define RESP_WAND_PRODUCT_INFO 0x0E
#define RESP_SPELL_CAST 0x24
#define RESP_IMU_PAYLOAD 0x2C
#define RESP_BUTTON_READ_THRESHOLD 0xDD
#define RESP_BUTTON_CALIBRATION 0xFB
#define RESP_IMU_CALIBRATION 0xFC

// Button state flags
#define BUTTON_ALL_PRESSED 0x0F
#define BUTTON_MIN_FOR_TRACKING 4 // Minimum buttons pressed to start tracking (all 4 buttons)

// Macro system opcodes
#define MACRO_CONTROL 0x68
#define MACRO_DELAY 0x10
#define MACRO_WAIT_BUSY 0x11
#define MACRO_LIGHT_CLEAR 0x20
#define MACRO_LIGHT_TRANSITION 0x22
#define MACRO_HAP_BUZZ 0x50
#define MACRO_FLUSH 0x60
#define MACRO_SET_LOOPS 0x80
#define MACRO_SET_LOOP 0x81

// LED groups
enum class LedGroup : uint8_t
{
    TIP = 0,
    POMMEL = 1,
    MID_LOWER = 2,
    MID_UPPER = 3
};

// Packet parsing functions
namespace WandProtocol
{
    // Parse IMU data packet (returns number of samples parsed)
    size_t parseIMUPacket(const uint8_t *data, size_t length,
                          IMUSample *samples, size_t max_samples);

    // Parse button state packet
    bool parseButtonPacket(const uint8_t *data, size_t length, uint8_t *button_state);

    // Parse battery level packet
    bool parseBatteryPacket(const uint8_t *data, size_t length, uint8_t *battery_level);

    // Get packet type (opcode)
    uint8_t getPacketType(const uint8_t *data, size_t length);
}

#endif // WAND_PROTOCOL_H
