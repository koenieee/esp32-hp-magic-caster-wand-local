#include "wand_commands.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wand_commands";

WandCommands::WandCommands()
    : conn_handle(BLE_HS_CONN_HANDLE_NONE),
      command_char_handle(0)
{
}

void WandCommands::setHandles(uint16_t conn_handle, uint16_t command_handle)
{
    this->conn_handle = conn_handle;
    this->command_char_handle = command_handle;
}

bool WandCommands::isReady() const
{
    return (conn_handle != BLE_HS_CONN_HANDLE_NONE && command_char_handle != 0);
}

bool WandCommands::sendCommand(const uint8_t *data, size_t length)
{
    if (!isReady())
    {
        ESP_LOGW(TAG, "Cannot send command: not ready");
        return false;
    }

    if (!data || length == 0)
    {
        ESP_LOGW(TAG, "Invalid command data");
        return false;
    }

    int rc = ble_gattc_write_no_rsp_flat(conn_handle, command_char_handle, data, length);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to send command, rc=%d", rc);
        return false;
    }

    return true;
}

bool WandCommands::startIMUStreaming()
{
    if (!isReady())
    {
        return false;
    }

    ESP_LOGI(TAG, "Starting IMU streaming");

    // Reset IMU flags first
    uint8_t resetCmd[] = {MSG_IMUFLAG_RESET};
    if (!sendCommand(resetCmd, sizeof(resetCmd)))
    {
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    // Start IMU streaming
    uint8_t startCmd[] = {MSG_IMUFLAG_SET, 0x01, 0x01};
    return sendCommand(startCmd, sizeof(startCmd));
}

bool WandCommands::stopIMUStreaming()
{
    if (!isReady())
    {
        return false;
    }

    ESP_LOGI(TAG, "Stopping IMU streaming");
    uint8_t resetCmd[] = {MSG_IMUFLAG_RESET};
    return sendCommand(resetCmd, sizeof(resetCmd));
}

bool WandCommands::setButtonThreshold(uint8_t button_index, uint8_t threshold)
{
    if (button_index > 7)
    {
        ESP_LOGW(TAG, "Invalid button index: %d (must be 0-7)", button_index);
        return false;
    }

    uint8_t cmd[3] = {MSG_BUTTON_SET_THRESHOLD, button_index, threshold};
    return sendCommand(cmd, sizeof(cmd));
}

bool WandCommands::initButtonThresholds()
{
    if (!isReady())
    {
        ESP_LOGW(TAG, "Not ready to initialize button thresholds");
        return false;
    }

    ESP_LOGI(TAG, "Initializing button thresholds");

    bool success = true;

    // Set thresholds for buttons 0-3 to 0x05
    for (uint8_t i = 0; i < 4; i++)
    {
        if (!setButtonThreshold(i, 0x05))
        {
            ESP_LOGW(TAG, "Failed to set threshold for button %d", i);
            success = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50)); // Increased delay between commands
    }

    // Set thresholds for buttons 4-7 to 0x08
    for (uint8_t i = 4; i < 8; i++)
    {
        if (!setButtonThreshold(i, 0x08))
        {
            ESP_LOGW(TAG, "Failed to set threshold for button %d", i);
            success = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50)); // Increased delay between commands
    }

    if (success)
    {
        ESP_LOGI(TAG, "✓ Button thresholds initialized");
    }

    return success;
}

bool WandCommands::setLED(LedGroup group, uint8_t r, uint8_t g, uint8_t b)
{
    // LED group mapping: TIP=0, POMMEL=1, MID_LOWER=2, MID_UPPER=3
    // Send the group value directly as defined in LedGroup enum
    uint8_t wand_group = (uint8_t)group;

    uint8_t cmd[5] = {MSG_LIGHT_CONTROL_SET_LED, wand_group, r, g, b};
    return sendCommand(cmd, sizeof(cmd));
}

bool WandCommands::clearAllLEDs()
{
    uint8_t cmd[1] = {MSG_LIGHT_CONTROL_CLEAR_ALL};
    return sendCommand(cmd, sizeof(cmd));
}

bool WandCommands::sendKeepAlive()
{
    if (!isReady())
    {
        return false;
    }

    // Re-send IMU streaming enable command as keep-alive
    // This refreshes the wand's activity timer without stopping the stream
    uint8_t cmd[3] = {MSG_IMUFLAG_SET, 0x01, 0x01};
    return sendCommand(cmd, sizeof(cmd));
}

bool WandCommands::sendMacro(const uint8_t *macro_data, size_t length)
{
    if (!macro_data || length == 0 || length > 200)
    {
        ESP_LOGW(TAG, "Invalid macro length: %d", length);
        return false;
    }

    return sendCommand(macro_data, length);
}

bool WandCommands::requestBatteryLevel()
{
    // Battery is read via notification, not a command
    // This is a placeholder for future implementation
    return true;
}

bool WandCommands::requestFirmwareVersion()
{
    if (!isReady())
    {
        ESP_LOGW(TAG, "Cannot request firmware version: not ready");
        return false;
    }

    ESP_LOGI(TAG, "Requesting firmware version (cmd=0x%02X)", MSG_FIRMWARE_VERSION_READ);
    uint8_t cmd[1] = {MSG_FIRMWARE_VERSION_READ};
    return sendCommand(cmd, sizeof(cmd));
}

bool WandCommands::requestProductInfo()
{
    if (!isReady())
    {
        ESP_LOGW(TAG, "Cannot request product info: not ready");
        return false;
    }

    // Request serial number (info_type = 0x01)
    ESP_LOGI(TAG, "Requesting serial number (cmd=0x%02X, type=0x01)", MSG_WAND_PRODUCT_INFO_READ);
    uint8_t cmd1[2] = {MSG_WAND_PRODUCT_INFO_READ, 0x01};
    if (!sendCommand(cmd1, sizeof(cmd1)))
        return false;
    vTaskDelay(pdMS_TO_TICKS(50));

    // Request SKU (info_type = 0x02)
    ESP_LOGI(TAG, "Requesting SKU (cmd=0x%02X, type=0x02)", MSG_WAND_PRODUCT_INFO_READ);
    uint8_t cmd2[2] = {MSG_WAND_PRODUCT_INFO_READ, 0x02};
    if (!sendCommand(cmd2, sizeof(cmd2)))
        return false;
    vTaskDelay(pdMS_TO_TICKS(50));

    // Request device ID (info_type = 0x04)
    ESP_LOGI(TAG, "Requesting device ID (cmd=0x%02X, type=0x04)", MSG_WAND_PRODUCT_INFO_READ);
    uint8_t cmd3[2] = {MSG_WAND_PRODUCT_INFO_READ, 0x04};
    return sendCommand(cmd3, sizeof(cmd3));
}
