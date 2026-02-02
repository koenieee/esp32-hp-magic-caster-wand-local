#include "wand_protocol.h"
#include "spell_detector.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wand_protocol";

namespace WandProtocol
{
    uint8_t getPacketType(const uint8_t *data, size_t length)
    {
        if (!data || length < 1)
        {
            return 0xFF; // Invalid
        }
        return data[0];
    }

    size_t parseIMUPacket(const uint8_t *data, size_t length,
                          IMUSample *samples, size_t max_samples)
    {
        if (!data || !samples || length < 4)
        {
            return 0;
        }

        // Validate packet type
        if (data[0] != RESP_IMU_PAYLOAD)
        {
            ESP_LOGW(TAG, "Not an IMU packet: 0x%02X", data[0]);
            return 0;
        }

        uint8_t sample_count = data[3];
        size_t expected_length = 4 + (sample_count * 12);

        if (length < expected_length)
        {
            ESP_LOGW(TAG, "IMU packet too short. Expected %d, got %d",
                     expected_length, length);
            return 0;
        }

        // Parse samples (delegate to existing IMUParser)
        return IMUParser::parse(data, length, samples, max_samples);
    }

    bool parseButtonPacket(const uint8_t *data, size_t length, uint8_t *button_state)
    {
        if (!data || !button_state || length < 2)
        {
            return false;
        }

        // Validate packet type
        if (data[0] != RESP_BUTTON_PAYLOAD)
        {
            ESP_LOGW(TAG, "Not a button packet: 0x%02X", data[0]);
            return false;
        }

        *button_state = data[1];
        return true;
    }

    bool parseBatteryPacket(const uint8_t *data, size_t length, uint8_t *battery_level)
    {
        if (!data || !battery_level || length < 1)
        {
            return false;
        }

        // Battery notification is just a single byte
        *battery_level = data[0];
        return true;
    }
}
