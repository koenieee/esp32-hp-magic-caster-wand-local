#include "ble_client.h"
#include "web_server.h"
#include "config.h"
#include "usb_hid.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <string.h>
#include <stdlib.h>
#include "nvs_flash.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "host/ble_hs_adv.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_client";
#if USE_USB_HID_DEVICE
extern USBHIDManager usbHID;
#endif

// Global for scan callback
static WebServer *g_web_server = nullptr;

// ============================================================================
// ESPHome-inspired BLE connection parameters (esp32_ble_client/ble_client_base.cpp)
// ============================================================================
// Key learnings from ESPHome Bluetooth Proxy:
// 1. Fast connection intervals (7.5-11.25ms) for high-throughput streaming
// 2. Longer supervision timeout (8-10s) for WiFi+BLE coexistence stability
// 3. Handle connection parameter update requests to enforce our preferences
// 4. Log actual negotiated parameters for debugging
// ============================================================================

// BLE error codes
#ifndef BLE_HS_EDONE
#define BLE_HS_EDONE 14 // Procedure complete
#endif
// Forward declarations
static int subscribe_cb(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg);

static int notify_cb(uint16_t conn_handle,
                     const struct ble_gatt_error *error,
                     struct ble_gatt_attr *attr, void *arg);

// Seeed Xiao ESP32-C6 built-in LED
#define LED_GPIO GPIO_NUM_15

// Static pointer for callback context
static WandBLEClient *g_clientInstance = nullptr;

// Service discovery state
static uint16_t notify_char_val_handle = 0;
static uint16_t command_char_val_handle = 0;
static uint16_t battery_char_val_handle = 0;

// UUIDs
static const ble_uuid128_t wand_service_uuid =
    BLE_UUID128_INIT(0x77, 0xc5, 0x63, 0x61, 0x4d, 0x54, 0x4c, 0x97,
                     0xa0, 0x48, 0x7e, 0x58, 0x01, 0x00, 0x42, 0x57);

static const ble_uuid128_t wand_command_uuid =
    BLE_UUID128_INIT(0x77, 0xc5, 0x63, 0x61, 0x4d, 0x54, 0x4c, 0x97,
                     0xa0, 0x48, 0x7e, 0x58, 0x02, 0x00, 0x42, 0x57);

static const ble_uuid128_t wand_notify_uuid =
    BLE_UUID128_INIT(0x77, 0xc5, 0x63, 0x61, 0x4d, 0x54, 0x4c, 0x97,
                     0xa0, 0x48, 0x7e, 0x58, 0x03, 0x00, 0x42, 0x57);

static const ble_uuid16_t battery_uuid = BLE_UUID16_INIT(0x2A19);
static const ble_uuid16_t battery_service_uuid = BLE_UUID16_INIT(0x180F);

// Processing task - runs in separate FreeRTOS task to process buffered data
void WandBLEClient::processingTaskFunc(void *arg)
{
    WandBLEClient *client = static_cast<WandBLEClient *>(arg);
    ESP_LOGI(TAG, "Notification processing task started");

    while (true)
    {
        client->processBufferedData();
        vTaskDelay(1); // Small delay to yield
    }
}

// Process data from circular buffer
// Process data from circular buffer
void WandBLEClient::processBufferedData()
{
    while (readIndex != writeIndex)
    {
        NotificationBuffer &buffer = circularBuffer[readIndex];
        if (buffer.ready && buffer.length > 0)
        {
            // Dispatch based on opcode
            switch (buffer.data[0])
            {
            case RESP_IMU_PAYLOAD:
                processIMUPacket(buffer.data, buffer.length);
                break;
            case RESP_BUTTON_PAYLOAD:
                processButtonPacket(buffer.data, buffer.length);
                break;
            case RESP_FIRMWARE_VERSION:
                processFirmwareVersion(buffer.data, buffer.length);
                break;
            case RESP_WAND_PRODUCT_INFO:
                processProductInfo(buffer.data, buffer.length);
                break;
            default:
                break;
            }

            buffer.ready = false; // Mark as processed
            readIndex = (readIndex + 1) % BUFFER_COUNT;
        }
        else
        {
            break; // No more ready buffers
        }
    }
}

WandBLEClient::WandBLEClient()
    : conn_handle(BLE_HS_CONN_HANDLE_NONE),
      notify_char_handle(0),
      command_char_handle(0),
      battery_char_handle(0),
      connection_start_time_us(0),
      spellCallback(nullptr),
      connectionCallback(nullptr),
      imuCallback(nullptr),
      connected(false),
      imuStreaming(false),
      lastButtonState(0),
      last_battery_level(0),
      userDisconnectRequested(false),
      writeIndex(0),
      readIndex(0),
      processingTask(nullptr),
      scanning(false)
{
    g_clientInstance = this;

    // Initialize wand info strings
    firmware_version[0] = '\0';
    serial_number[0] = '\0';
    sku[0] = '\0';
    device_id[0] = '\0';
    wand_type[0] = '\0';

    // Initialize circular buffer
    for (int i = 0; i < BUFFER_COUNT; i++)
    {
        circularBuffer[i].ready = false;
        circularBuffer[i].length = 0;
    }

    // Create processing task
    xTaskCreate(processingTaskFunc, "ble_process", 4096, this, 5, &processingTask);

    // Initialize LED
    gpio_config_t led_conf = {};
    led_conf.pin_bit_mask = (1ULL << LED_GPIO);
    led_conf.mode = GPIO_MODE_OUTPUT;
    led_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    led_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    led_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&led_conf);
    gpio_set_level(LED_GPIO, 0);
    memset(&peer_addr, 0, sizeof(peer_addr));
}

WandBLEClient::~WandBLEClient()
{
    // Clean up task
    if (processingTask)
    {
        vTaskDelete(processingTask);
        processingTask = nullptr;
    }

    if (connected && conn_handle != BLE_HS_CONN_HANDLE_NONE)
    {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    g_clientInstance = nullptr;
}

static void ble_app_on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE host synchronized");
}

static void ble_app_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host reset, reason=%d", reason);
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static int chr_discovered(uint16_t conn_handle,
                          const struct ble_gatt_error *error,
                          const struct ble_gatt_chr *chr, void *arg)
{
    WandBLEClient *client = (WandBLEClient *)arg;

    // Handle errors (but not EDONE which signals completion)
    if (error->status != 0 && error->status != BLE_HS_EDONE)
    {
        ESP_LOGE(TAG, "Characteristic discovery error: %d", error->status);
        return 0;
    }

    // Process discovered characteristic
    if (chr)
    {
        if (ble_uuid_cmp(&chr->uuid.u, &wand_notify_uuid.u) == 0)
        {
            notify_char_val_handle = chr->val_handle;
            ESP_LOGI(TAG, "Found notify characteristic, handle=%d", chr->val_handle);
        }
        else if (ble_uuid_cmp(&chr->uuid.u, &wand_command_uuid.u) == 0)
        {
            command_char_val_handle = chr->val_handle;
            ESP_LOGI(TAG, "Found command characteristic, handle=%d", chr->val_handle);
        }
        else if (ble_uuid_cmp(&chr->uuid.u, &battery_uuid.u) == 0)
        {
            battery_char_val_handle = chr->val_handle;
            ESP_LOGI(TAG, "Found battery characteristic, handle=%d", chr->val_handle);
        }
    }

    // Check if discovery is complete - signaled by either:
    // 1) chr == NULL with error->status == 0, OR
    // 2) error->status == BLE_HS_EDONE (14)
    if ((chr == NULL && error->status == 0) || error->status == BLE_HS_EDONE)
    {
        ESP_LOGI(TAG, "Characteristic discovery complete (status=%d)", error->status);

        // Set up handles and subscribe if we have the required characteristics
        if (notify_char_val_handle && command_char_val_handle)
        {
            ESP_LOGI(TAG, "Setting up wand communication...");
            ESP_LOGI(TAG, "  Notify handle: %d (CCCD: %d)", notify_char_val_handle, notify_char_val_handle + 1);
            ESP_LOGI(TAG, "  Command handle: %d", command_char_val_handle);
            if (battery_char_val_handle)
            {
                ESP_LOGI(TAG, "  Battery handle: %d (CCCD: %d)", battery_char_val_handle, battery_char_val_handle + 1);
            }
            client->setCharHandles(notify_char_val_handle, command_char_val_handle);
            client->setWandCommandHandles(conn_handle, command_char_val_handle);

            // Subscribe to notifications by writing to CCCD
            uint8_t notify_enable[2] = {0x01, 0x00}; // Enable notifications
            int rc = ble_gattc_write_flat(conn_handle, notify_char_val_handle + 1,
                                          notify_enable, sizeof(notify_enable), subscribe_cb, NULL);
            if (rc != 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to notifications, rc=%d", rc);
            }
            else
            {
                ESP_LOGI(TAG, "✓ Subscribed to notifications on handle %d", notify_char_val_handle);
            }

            // Subscribe to battery notifications if available
            if (battery_char_val_handle)
            {
                rc = ble_gattc_write_flat(conn_handle, battery_char_val_handle + 1,
                                          notify_enable, sizeof(notify_enable), subscribe_cb, NULL);
                if (rc != 0)
                {
                    ESP_LOGW(TAG, "Failed to subscribe to battery notifications, rc=%d", rc);
                }
                else
                {
                    ESP_LOGI(TAG, "✓ Subscribed to battery notifications on handle %d", battery_char_val_handle);
                }
            }
        }
        else
        {
            ESP_LOGW(TAG, "Not all required characteristics found!");
        }
    }

    return 0;
}

static int svc_discovered(uint16_t conn_handle,
                          const struct ble_gatt_error *error,
                          const struct ble_gatt_svc *service, void *arg)
{
    if (error->status == 0 && service)
    {
        ESP_LOGI(TAG, "Service discovered");
        ble_gattc_disc_all_chrs(conn_handle, service->start_handle,
                                service->end_handle, chr_discovered, arg);
    }
    return 0;
}

static int subscribe_cb(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0)
    {
        ESP_LOGI(TAG, "Successfully subscribed to notifications");
    }
    else
    {
        ESP_LOGE(TAG, "Subscription failed: %d", error->status);
    }
    return 0;
}

static int notify_cb(uint16_t conn_handle,
                     const struct ble_gatt_error *error,
                     struct ble_gatt_attr *attr, void *arg)
{
    WandBLEClient *client = (WandBLEClient *)arg;

    if (error->status == 0 && attr && attr->om)
    {
        struct os_mbuf *om = attr->om;
        uint16_t om_len = OS_MBUF_PKTLEN(om);
        uint8_t data[256];
        if (om_len > sizeof(data))
        {
            om_len = sizeof(data);
        }

        int rc = os_mbuf_copydata(om, 0, om_len, data);
        if (rc == 0 && om_len >= 1)
        {
            uint8_t opcode = WandProtocol::getPacketType(data, om_len);

            switch (opcode)
            {
            case RESP_IMU_PAYLOAD:
                client->processIMUPacket(data, om_len);
                break;

            case RESP_BUTTON_PAYLOAD:
                client->processButtonPacket(data, om_len);
                break;

            case RESP_FIRMWARE_VERSION:
                client->processFirmwareVersion(data, om_len);
                break;

            case RESP_WAND_PRODUCT_INFO:
                client->processProductInfo(data, om_len);
                break;

            default:
                ESP_LOGD(TAG, "Unknown packet type: 0x%02X", opcode);
                break;
            }
        }
    }
    return 0;
}
static int battery_read_cb(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg)
{
    WandBLEClient *client = (WandBLEClient *)arg;

    if (error->status == 0 && attr && attr->om)
    {
        uint8_t value;
        if (os_mbuf_copydata(attr->om, 0, 1, &value) == 0)
        {
            client->updateBatteryLevel(value);
            ESP_LOGI(TAG, "🔋 Battery: %d%%", value);
        }
    }

    return 0;
}

int WandBLEClient::gap_event_handler(struct ble_gap_event *event, void *arg)
{
    WandBLEClient *client = (WandBLEClient *)arg;

    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0)
        {
            client->connection_start_time_us = esp_timer_get_time();
            ESP_LOGI(TAG, "Connected to wand!");
            client->conn_handle = event->connect.conn_handle;
            client->connected = true;
            client->userDisconnectRequested = false; // Clear flag on successful connection

            if (client->connectionCallback)
            {
                client->connectionCallback(true);
            }

            gpio_set_level(LED_GPIO, 1);

            // ESPHome-inspired: Request preferred connection parameters
            // Use faster connection intervals (like ESPHome) for better throughput with 234Hz IMU streaming
            // Timeout increased to 10 seconds (ESPHome uses 8-10s) for WiFi+BLE coexistence
            struct ble_gap_upd_params preferred_params = {
                .itvl_min = 0x0006, // 7.5ms (BLE minimum, like ESPHome FAST)
                .itvl_max = 0x0009, // 11.25ms (like ESPHome MEDIUM)
                .latency = 0,
                .supervision_timeout = 0x03E8, // 10 seconds (1000 * 10ms)
                .min_ce_len = 0x0010,
                .max_ce_len = 0x0100,
            };
            int upd_rc = ble_gap_update_params(event->connect.conn_handle, &preferred_params);
            if (upd_rc == 0)
            {
                ESP_LOGI(TAG, "Requested ESPHome-style connection params (7.5-11.25ms interval, 10s timeout)");
            }
            else
            {
                ESP_LOGW(TAG, "Failed to request connection param update: %d", upd_rc);
            }

            // Discover services
            ESP_LOGI(TAG, "Discovering services...");
            ble_gattc_disc_svc_by_uuid(event->connect.conn_handle,
                                       &wand_service_uuid.u, svc_discovered, client);
            // Also discover battery service
            ble_gattc_disc_svc_by_uuid(event->connect.conn_handle,
                                       &battery_service_uuid.u, svc_discovered, client);
        }
        else
        {
            ESP_LOGE(TAG, "Connection failed, status=%d", event->connect.status);
            client->connected = false;
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
    {
        // Calculate connection duration
        int64_t disconnect_time_us = esp_timer_get_time();
        int64_t duration_us = disconnect_time_us - client->connection_start_time_us;
        float duration_seconds = duration_us / 1000000.0f;

        // ESPHome-inspired: Decode and log disconnect reason for debugging
        const char *reason_str;
        switch (event->disconnect.reason)
        {
        case BLE_ERR_CONN_TERM_LOCAL:
            reason_str = "local termination";
            break;
        case BLE_ERR_REM_USER_CONN_TERM:
            reason_str = "remote user termination";
            break;
        case BLE_ERR_CONN_SPVN_TMO:
            reason_str = "supervision timeout";
            break;
        case BLE_ERR_CONN_TERM_MIC:
            reason_str = "MIC failure";
            break;
        case BLE_ERR_LMP_LL_RSP_TMO:
            reason_str = "LMP response timeout";
            break;
        default:
            reason_str = "unknown";
            break;
        }

        ESP_LOGW(TAG, "Disconnected from wand, reason=%d (%s), connection duration=%.2f seconds",
                 event->disconnect.reason, reason_str, duration_seconds);

        client->conn_handle = BLE_HS_CONN_HANDLE_NONE;
        client->connected = false;
        client->imuStreaming = false;
        client->connection_start_time_us = 0;

        if (client->connectionCallback)
        {
            client->connectionCallback(false);
        }

        gpio_set_level(LED_GPIO, 0);

        notify_char_val_handle = 0;
        command_char_val_handle = 0;
        battery_char_val_handle = 0;
    }
    break;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "Discovery complete");

        if (notify_char_val_handle && command_char_val_handle)
        {
            client->setCharHandles(notify_char_val_handle, command_char_val_handle);
            client->wandCommands.setHandles(client->conn_handle, command_char_val_handle);
        }
        break;

    case BLE_GAP_EVENT_NOTIFY_RX:
    {
        WandBLEClient *client = static_cast<WandBLEClient *>(arg);
        struct os_mbuf *om = event->notify_rx.om;

        // Handle battery notifications
        if (client && event->notify_rx.attr_handle == battery_char_val_handle)
        {
            uint8_t value;
            if (os_mbuf_copydata(om, 0, 1, &value) == 0)
            {
                client->updateBatteryLevel(value);
                ESP_LOGI(TAG, "🔋 Battery notification: %d%%", value);
            }
            return 0;
        }

        if (client && event->notify_rx.attr_handle == notify_char_val_handle)
        {
            uint16_t len = OS_MBUF_PKTLEN(om);
            if (len > 0 && len <= BUFFER_SIZE)
            {
                // Calculate next write index
                uint8_t nextWrite = (client->writeIndex + 1) % BUFFER_COUNT;

                // Check if buffer is full (would overwrite unprocessed data)
                if (nextWrite != client->readIndex)
                {
                    NotificationBuffer &buffer = client->circularBuffer[client->writeIndex];

                    // Fast copy from mbuf to our buffer
                    int rc = os_mbuf_copydata(om, 0, len, buffer.data);
                    if (rc == 0)
                    {
                        buffer.length = len;
                        buffer.ready = true;            // Mark as ready for processing
                        client->writeIndex = nextWrite; // Advance write index
                    }
                }
                // If buffer full, drop packet silently
            }
        }

        // DO NOT free mbuf - stack manages it
        return 0;
    }

    case BLE_GAP_EVENT_CONN_UPDATE:
        // Connection parameters were updated - log the actual negotiated values
        if (event->conn_update.status == 0)
        {
            // Read actual connection parameters
            struct ble_gap_conn_desc desc;
            int rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
            if (rc == 0)
            {
                float interval_ms = desc.conn_itvl * 1.25f;
                float timeout_s = desc.supervision_timeout * 10.0f / 1000.0f;
                ESP_LOGI(TAG, "📶 Connection params updated: interval=%.2fms, latency=%d, timeout=%.1fs",
                         interval_ms, desc.conn_latency, timeout_s);
            }
            else
            {
                ESP_LOGI(TAG, "Connection parameters updated successfully");
            }
        }
        else
        {
            ESP_LOGW(TAG, "Connection parameter update failed: status=%d", event->conn_update.status);
        }
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
    {
        // The wand is requesting connection parameter changes
        // ESPHome-inspired: Accept interval/latency but enforce longer timeout
        ESP_LOGI(TAG, "Wand requesting conn params: itvl=%d-%d, latency=%d, timeout=%d",
                 event->conn_update_req.peer_params->itvl_min,
                 event->conn_update_req.peer_params->itvl_max,
                 event->conn_update_req.peer_params->latency,
                 event->conn_update_req.peer_params->supervision_timeout);

        // Accept the wand's interval and latency preferences
        event->conn_update_req.self_params->itvl_min = event->conn_update_req.peer_params->itvl_min;
        event->conn_update_req.self_params->itvl_max = event->conn_update_req.peer_params->itvl_max;
        event->conn_update_req.self_params->latency = event->conn_update_req.peer_params->latency;

        // But enforce minimum 10 second supervision timeout for stability
        // The wand requests 4 seconds which is too aggressive
        uint16_t requested_timeout = event->conn_update_req.peer_params->supervision_timeout;
        uint16_t min_timeout = 1000; // 10 seconds (1000 * 10ms)
        if (requested_timeout < min_timeout)
        {
            event->conn_update_req.self_params->supervision_timeout = min_timeout;
            ESP_LOGI(TAG, "  Enforcing longer timeout: %d (was %d)", min_timeout, requested_timeout);
        }
        else
        {
            event->conn_update_req.self_params->supervision_timeout = requested_timeout;
        }

        return 0; // Accept the (possibly modified) parameters
    }

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: %d", event->mtu.value);
        return 0;

    default:
        break;
    }

    return 0;
}

bool WandBLEClient::begin(const unsigned char *model_data, size_t model_size)
{
    if (!spellDetector.begin(model_data, model_size))
    {
        ESP_LOGE(TAG, "Failed to initialize spell detector");
        return false;
    }

    ESP_LOGI(TAG, "Initializing NimBLE...");
    nimble_port_init();

    ble_hs_cfg.sync_cb = ble_app_on_sync;
    ble_hs_cfg.reset_cb = ble_app_on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_svc_gap_device_name_set("ESP32-Wand");
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to set device name");
        return false;
    }

    // Boost BLE TX power for Seeeduino XIAO's weaker PCB antenna
    ESP_LOGI(TAG, "Setting BLE TX power to maximum for better range...");
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, ESP_PWR_LVL_P9);

    nimble_port_freertos_init(ble_host_task);
    return true;
}

bool WandBLEClient::connect(const char *address)
{
    if (!address)
    {
        ESP_LOGE(TAG, "Invalid address");
        return false;
    }

    // Cancel any pending connection first
    int rc = ble_gap_conn_cancel();
    if (rc == 0)
    {
        ESP_LOGI(TAG, "Cancelled pending connection");
        vTaskDelay(100 / portTICK_PERIOD_MS); // Small delay after cancel
    }

    ble_addr_t addr;
    // Try random address first (most common for BLE devices)
    addr.type = BLE_ADDR_RANDOM;

    // Parse MAC address string (format: XX:XX:XX:XX:XX:XX)
    rc = sscanf(address, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                &addr.val[5], &addr.val[4], &addr.val[3],
                &addr.val[2], &addr.val[1], &addr.val[0]);
    if (rc != 6)
    {
        ESP_LOGE(TAG, "Invalid MAC address format");
        return false;
    }

    peer_addr = addr;

    // Configure connection parameters - use standard values that NimBLE accepts
    // Values must be in valid ranges per Bluetooth spec
    struct ble_gap_conn_params conn_params;
    conn_params.scan_itvl = 0x0010;           // 10ms (16 * 0.625ms) - standard
    conn_params.scan_window = 0x0010;         // 10ms (16 * 0.625ms) - must be <= scan_itvl
    conn_params.itvl_min = 0x0018;            // 30ms (24 * 1.25ms) - standard range
    conn_params.itvl_max = 0x0028;            // 50ms (40 * 1.25ms) - standard range
    conn_params.latency = 0;                  // No slave latency
    conn_params.supervision_timeout = 0x0C80; // 32 seconds (3200 * 10ms)
    conn_params.min_ce_len = 0x0010;          // 10ms (16 * 0.625ms)
    conn_params.max_ce_len = 0x0300;          // 480ms (768 * 0.625ms)

    ESP_LOGI(TAG, "Attempting connection to %s (random address type) with 32s supervision timeout", address);
    rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, 30000, &conn_params,
                         gap_event_handler, this);

    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to initiate connection with random address (rc=%d), trying public address", rc);

        // Try public address as fallback
        addr.type = BLE_ADDR_PUBLIC;
        peer_addr = addr;

        ESP_LOGI(TAG, "Trying public address with 32s supervision timeout");
        rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, 30000, &conn_params,
                             gap_event_handler, this);

        if (rc != 0)
        {
            ESP_LOGE(TAG, "Failed to initiate connection with public address, rc=%d", rc);
            return false;
        }
    }

    return true;
}

void WandBLEClient::disconnect()
{
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE)
    {
        userDisconnectRequested = true;
        ESP_LOGI(TAG, "User-initiated disconnect from wand (conn_handle=%d)", conn_handle);
        int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0)
        {
            ESP_LOGW(TAG, "Failed to terminate connection: rc=%d", rc);
        }
        // Note: Connection state will be updated in the gap_event_handler
    }
    else
    {
        ESP_LOGW(TAG, "Disconnect called but no active connection");
    }
}

bool WandBLEClient::startIMUStreaming()
{
    bool success = wandCommands.startIMUStreaming();
    if (success)
    {
        imuStreaming = true;
    }
    return success;
}

bool WandBLEClient::stopIMUStreaming()
{
    bool success = wandCommands.stopIMUStreaming();
    if (success)
    {
        imuStreaming = false;
    }
    return success;
}

bool WandBLEClient::initButtonThresholds()
{
    return wandCommands.initButtonThresholds();
}

bool WandBLEClient::sendKeepAlive()
{
    return wandCommands.sendKeepAlive();
}

bool WandBLEClient::playSpellEffect(const char *spell_name)
{
    uint8_t macro_buffer[64];
    size_t macro_len = SpellEffects::buildEffect(spell_name, macro_buffer, sizeof(macro_buffer));

    if (macro_len > 0)
    {
        return wandCommands.sendMacro(macro_buffer, macro_len);
    }

    return false;
}

void WandBLEClient::setCallbacks(SpellDetectedCallback spell_cb, ConnectionCallback conn_cb, IMUDataCallback imu_cb)
{
    spellCallback = spell_cb;
    connectionCallback = conn_cb;
    imuCallback = imu_cb;
}

void WandBLEClient::setWebServer(WebServer *server)
{
    webServer = server;
    g_web_server = server;
}

uint8_t WandBLEClient::getBatteryLevel()
{
    if (connected && battery_char_val_handle != 0)
    {
        ble_gattc_read(conn_handle, battery_char_val_handle, battery_read_cb, this);
    }
    return last_battery_level;
}

void WandBLEClient::setCharHandles(uint16_t notify_handle, uint16_t command_handle)
{
    notify_char_handle = notify_handle;
    command_char_handle = command_handle;
}

void WandBLEClient::setWandCommandHandles(uint16_t conn_handle_param, uint16_t command_handle)
{
    wandCommands.setHandles(conn_handle_param, command_handle);
}

void WandBLEClient::processButtonPacket(const uint8_t *data, size_t length)
{
    uint8_t buttonState;
    if (!WandProtocol::parseButtonPacket(data, length, &buttonState))
    {
        return;
    }

    // Count pressed buttons for easier tracking
    uint8_t buttonsPressed = __builtin_popcount(buttonState & 0x0F);
    bool enoughButtonsPressed = (buttonsPressed >= BUTTON_MIN_FOR_TRACKING);
    bool wasEnoughPressed = (__builtin_popcount(lastButtonState & 0x0F) >= BUTTON_MIN_FOR_TRACKING);

#if USE_USB_HID_DEVICE
    if (usbHID.getHidMode() != HID_MODE_GAMEPAD)
    {
        usbHID.setGamepadButtons(buttonState & 0x0F);
    }
#endif

    if (buttonState != lastButtonState)
    {
        bool b1 = buttonState & 0x01, b2 = buttonState & 0x02,
             b3 = buttonState & 0x04, b4 = buttonState & 0x08;
        ESP_LOGI(TAG, "🔘 Buttons: [1]=%s [2]=%s [3]=%s [4]=%s (%d/4 pressed)",
                 b1 ? "●" : "○", b2 ? "●" : "○", b3 ? "●" : "○", b4 ? "●" : "○", buttonsPressed);

        // Broadcast button state to web GUI
        if (webServer)
        {
            webServer->broadcastButtonPress(b1, b2, b3, b4);
        }
    }

    // 3+ buttons pressed - start tracking (was 4 buttons required)
    if (enoughButtonsPressed && !wasEnoughPressed)
    {
        // Log heap status before starting tracking
        ESP_LOGI(TAG, "Free heap before tracking: %lu bytes", esp_get_free_heap_size());

        // Enable purple LED on wand tip to indicate tracking
        wandCommands.setLED(LedGroup::TIP, 255, 0, 255);
        if (!ahrsTracker.isTracking())
        {
            ahrsTracker.startTracking();
            ESP_LOGI(TAG, "Started spell tracking (%d buttons pressed)", buttonsPressed);

            // Disable mouse movement during spell tracking
#if USE_USB_HID_DEVICE
            usbHID.setInSpellMode(true);
#endif

            // Notify web visualizer (don't broadcast position[0], wait for position[1])
            if (webServer)
            {
                webServer->broadcastGestureStart();
            }
        }
    }
    // Buttons released - detect spell
    else if (!enoughButtonsPressed && wasEnoughPressed)
    {
        // Clear wand LEDs after spell tracking
        wandCommands.clearAllLEDs();
        if (ahrsTracker.isTracking())
        {
            Position2D *positions = nullptr;
            size_t position_count = 0;

            if (ahrsTracker.stopTracking(&positions, &position_count))
            {
                float normalized_positions[SPELL_INPUT_SIZE];
                if (GesturePreprocessor::preprocess(positions, position_count,
                                                    normalized_positions, SPELL_INPUT_SIZE))
                {
                    const char *spell_name = spellDetector.detect(normalized_positions);
                    if (spell_name && spellCallback)
                    {
                        spellCallback(spell_name, spellDetector.getConfidence());

                        // Send mapped keyboard key for detected spell
#if USE_USB_HID_DEVICE
                        usbHID.sendSpellKeyboardForSpell(spell_name);
                        usbHID.sendSpellGamepadForSpell(spell_name);
#endif
                    }
                    else if (!spell_name)
                    {
                        // Low confidence - get the prediction anyway for GUI display
                        const char *predicted = spellDetector.getLastPrediction();
                        float conf = spellDetector.getConfidence();

                        // Broadcast to web GUI even though confidence is too low
                        if (webServer && predicted)
                        {
                            webServer->broadcastLowConfidence(predicted, conf);
                        }
                    }
                }
                // Do NOT free positions - it's owned by ahrsTracker
            }

            // Re-enable mouse movement after spell tracking
#if USE_USB_HID_DEVICE
            usbHID.setInSpellMode(false);
#endif

            // Notify web visualizer
            if (webServer)
            {
                webServer->broadcastGestureEnd();
            }
        }
    }

    lastButtonState = buttonState;
}

void WandBLEClient::processIMUPacket(const uint8_t *data, size_t length)
{
    size_t sample_count = WandProtocol::parseIMUPacket(data, length, imuBuffer, 32);

    for (size_t i = 0; i < sample_count; i++)
    {
        // IMPORTANT: Don't do heavy processing (like AHRS update) in BLE callback
        // to avoid mbuf corruption. Only forward data to main loop via callback.

        // Always call IMU callback if registered - this defers to main loop
        if (imuCallback)
        {
            imuCallback(imuBuffer[i].accel_x, imuBuffer[i].accel_y, imuBuffer[i].accel_z,
                        imuBuffer[i].gyro_x, imuBuffer[i].gyro_y, imuBuffer[i].gyro_z);
        }
    }
}

void WandBLEClient::updateAHRS(const IMUSample &sample)
{
    // ALWAYS update AHRS to maintain orientation quaternion
    // Python also updates AHRS continuously, not just during tracking
    size_t old_count = ahrsTracker.getPositionCount();

    static bool was_tracking = false;
    static bool has_last_mouse_pos = false;
    static Position2D last_mouse_pos = {0.0f, 0.0f};
    static float accum_dx = 0.0f;
    static float accum_dy = 0.0f;
    static int mouse_counter = 0;

    ahrsTracker.update(sample);

    bool is_tracking = ahrsTracker.isTracking();

    if (is_tracking != was_tracking)
    {
        has_last_mouse_pos = false;
        accum_dx = 0.0f;
        accum_dy = 0.0f;
        mouse_counter = 0;
        was_tracking = is_tracking;
    }

    if (!is_tracking)
    {
        Position2D pos;
        if (ahrsTracker.getMousePosition(pos))
        {
            if (!has_last_mouse_pos)
            {
                last_mouse_pos = pos;
                has_last_mouse_pos = true;
            }
            else
            {
                float dx = pos.x - last_mouse_pos.x;
                float dy = pos.y - last_mouse_pos.y;
#if USE_USB_HID_DEVICE
                HIDMode current_mode = usbHID.getHidMode();
                if (current_mode == HID_MODE_MOUSE)
                {
                    dy = usbHID.getInvertMouseY() ? -dy : dy;
                }
                else if (current_mode == HID_MODE_GAMEPAD)
                {
                    dy = usbHID.getGamepadInvertY() ? -dy : dy;
                }
#else
                dy = -dy; // Default: inverted
#endif
                accum_dx += dx;
                accum_dy += dy;
                last_mouse_pos = pos;

                // Rate limit mouse updates to ~60 Hz (every 4th sample)
                if (++mouse_counter >= 4)
                {
#if USE_USB_HID_DEVICE
                    HIDMode hid_mode = usbHID.getHidMode();
                    if (hid_mode == HID_MODE_GAMEPAD)
                    {
                        usbHID.updateGamepadFromGesture(accum_dx, accum_dy);
                    }
                    else if (hid_mode == HID_MODE_MOUSE)
                    {
                        usbHID.updateMouseFromGesture(accum_dx, accum_dy);
                    }
#endif
                    accum_dx = 0.0f;
                    accum_dy = 0.0f;
                    mouse_counter = 0;
                }
            }
        }
    }

    // Only broadcast gesture points if tracking is active
    if (is_tracking)
    {
        size_t new_count = ahrsTracker.getPositionCount();

        if (new_count > old_count)
        {
            const Position2D *positions = ahrsTracker.getPositions();
            if (positions && new_count > 0)
            {
                const Position2D &pos = positions[new_count - 1];
                if (!has_last_mouse_pos)
                {
                    last_mouse_pos = pos;
                    has_last_mouse_pos = true;
                }
                else
                {
                    float dx = pos.x - last_mouse_pos.x;
                    float dy = pos.y - last_mouse_pos.y;
#if USE_USB_HID_DEVICE
                    HIDMode current_mode = usbHID.getHidMode();
                    if (current_mode == HID_MODE_MOUSE)
                    {
                        dy = usbHID.getInvertMouseY() ? -dy : dy;
                    }
                    else if (current_mode == HID_MODE_GAMEPAD)
                    {
                        dy = usbHID.getGamepadInvertY() ? -dy : dy;
                    }
#else
                    dy = -dy; // Default: inverted
#endif
                    accum_dx += dx;
                    accum_dy += dy;
                    last_mouse_pos = pos;

                    // Rate limit mouse updates to ~60 Hz (every 4th point)
                    if (new_count == 2 || ++mouse_counter >= 4)
                    {
#if USE_USB_HID_DEVICE
                        HIDMode hid_mode = usbHID.getHidMode();
                        if (hid_mode == HID_MODE_GAMEPAD)
                        {
                            usbHID.updateGamepadFromGesture(accum_dx, accum_dy);
                        }
                        else if (hid_mode == HID_MODE_MOUSE)
                        {
                            usbHID.updateMouseFromGesture(accum_dx, accum_dy);
                        }
#endif
                        accum_dx = 0.0f;
                        accum_dy = 0.0f;
                        mouse_counter = 0;
                    }
                }

                if (webServer)
                {
#if GESTURE_RATE_LIMIT_ENABLE
                    // Rate limit: Only broadcast every 4th position (~60 Hz instead of 234 Hz)
                    // This provides smooth visualization while preventing WebSocket overflow
                    static int broadcast_counter = 0;

                    // Always broadcast position[1] immediately after tracking starts
                    if (new_count == 2 || ++broadcast_counter >= 4)
                    {
                        const Position2D &pos = positions[new_count - 1];
                        webServer->broadcastGesturePoint(pos.x, pos.y);
                        broadcast_counter = 0;
                    }
#else
                    // Broadcast all gesture points at full IMU rate (~234 Hz)
                    const Position2D &pos = positions[new_count - 1];
                    webServer->broadcastGesturePoint(pos.x, pos.y);
#endif
                }
            }
        }
    }
}

// Global for scan callback
bool WandBLEClient::requestWandInfo()
{
    bool success = true;
    success &= wandCommands.requestFirmwareVersion();
    vTaskDelay(pdMS_TO_TICKS(50)); // Small delay between commands
    success &= wandCommands.requestProductInfo();
    return success;
}

void WandBLEClient::processFirmwareVersion(const uint8_t *data, size_t length)
{
    // Response format: [opcode][version_string...]
    ESP_LOGI(TAG, "processFirmwareVersion called: length=%d", length);

    if (length < 2)
    {
        ESP_LOGW(TAG, "Firmware version response too short: length=%d", length);
        return;
    }

    // Debug: print raw data
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, length, ESP_LOG_INFO);

    // Convert version bytes to dotted string format (e.g., [0, 3] -> "0.3")
    // Skip opcode byte
    size_t version_len = length - 1;
    if (version_len == 0)
    {
        strcpy(firmware_version, "unknown");
    }
    else
    {
        firmware_version[0] = '\0';
        char temp[8];
        for (size_t i = 0; i < version_len && i < 5; i++) // Max 5 version parts
        {
            if (i > 0)
                strcat(firmware_version, ".");
            snprintf(temp, sizeof(temp), "%u", data[1 + i]);
            strcat(firmware_version, temp);
        }
    }

    ESP_LOGI(TAG, "Firmware version: '%s'", firmware_version);

    // Don't broadcast yet - wait until we have all product info
}

void WandBLEClient::processProductInfo(const uint8_t *data, size_t length)
{
    // Response format: [opcode][info_type][data...]
    ESP_LOGI(TAG, "processProductInfo called: length=%d", length);

    if (length < 3)
    {
        ESP_LOGW(TAG, "Product info response too short: length=%d", length);
        return;
    }

    // Debug: print raw data
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, length, ESP_LOG_INFO);

    uint8_t info_type = data[1];

    if (info_type == 0x01)
    {
        // Serial number (4 bytes, little-endian uint32)
        if (length >= 6)
        {
            uint32_t serial = (uint32_t)data[2] | ((uint32_t)data[3] << 8) |
                              ((uint32_t)data[4] << 16) | ((uint32_t)data[5] << 24);
            snprintf(serial_number, sizeof(serial_number), "%lu", serial);
            ESP_LOGI(TAG, "Serial number: %s", serial_number);
        }
    }
    else if (info_type == 0x02)
    {
        // SKU (ASCII string)
        size_t str_len = length - 2;
        if (str_len >= sizeof(sku))
            str_len = sizeof(sku) - 1;
        memcpy(sku, data + 2, str_len);
        sku[str_len] = '\0';
        // Remove null bytes
        for (size_t i = 0; i < str_len; i++)
        {
            if (sku[i] == '\0')
                sku[i] = ' ';
        }
        ESP_LOGI(TAG, "SKU: %s", sku);
    }
    else if (info_type == 0x04)
    {
        // Device ID (ASCII string)
        size_t str_len = length - 2;
        if (str_len >= sizeof(device_id))
            str_len = sizeof(device_id) - 1;
        memcpy(device_id, data + 2, str_len);
        device_id[str_len] = '\0';
        // Remove null bytes
        for (size_t i = 0; i < str_len; i++)
        {
            if (device_id[i] == '\0')
                device_id[i] = ' ';
        }
        ESP_LOGI(TAG, "Device ID: %s", device_id);

        // Extract wand type from device ID
        // Format: "WBMC22G1SHNW" -> drop last char -> take last 2 chars -> "HN"
        size_t dev_len = strlen(device_id);
        if (dev_len >= 3)
        {
            char type_suffix[3];
            type_suffix[0] = device_id[dev_len - 3];
            type_suffix[1] = device_id[dev_len - 2];
            type_suffix[2] = '\0';

            if (strcmp(type_suffix, "DF") == 0)
                strcpy(wand_type, "DEFIANT");
            else if (strcmp(type_suffix, "LY") == 0)
                strcpy(wand_type, "LOYAL");
            else if (strcmp(type_suffix, "HR") == 0)
                strcpy(wand_type, "HEROIC");
            else if (strcmp(type_suffix, "HN") == 0)
                strcpy(wand_type, "HONOURABLE");
            else if (strcmp(type_suffix, "AV") == 0)
                strcpy(wand_type, "ADVENTUROUS");
            else if (strcmp(type_suffix, "WS") == 0)
                strcpy(wand_type, "WISE");
            else
                strcpy(wand_type, "UNKNOWN");

            ESP_LOGI(TAG, "Wand type: %s (from suffix: %s)", wand_type, type_suffix);
        }

        // Broadcast complete wand info to web GUI
        if (webServer)
        {
            webServer->broadcastWandInfo(firmware_version, serial_number, sku, device_id, wand_type);
        }
    }
}
// BLE scan callback
static int ble_scan_callback(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC)
    {
        struct ble_gap_disc_desc *desc = &event->disc;

        // Convert address to string
        char addr_str[18];
        snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 desc->addr.val[5], desc->addr.val[4], desc->addr.val[3],
                 desc->addr.val[2], desc->addr.val[1], desc->addr.val[0]);

        // Try to get device name from advertisement data
        char name[32] = "Unknown";
        struct ble_hs_adv_fields fields;
        memset(&fields, 0, sizeof(fields));
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) == 0)
        {
            // Try complete name first
            if (fields.name != NULL && fields.name_len > 0)
            {
                size_t len = fields.name_len;
                if (len >= sizeof(name))
                {
                    len = sizeof(name) - 1;
                }
                memcpy(name, fields.name, len);
                name[len] = '\0';
            }
        }

        ESP_LOGI(TAG, "Discovered device: %s | %s | RSSI: %d", addr_str, name, desc->rssi);

        // Broadcast to web GUI
        if (g_web_server)
        {
            g_web_server->broadcastScanResult(addr_str, name, desc->rssi);
        }
    }
    else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE)
    {
        ESP_LOGI(TAG, "BLE scan complete");
        if (g_web_server)
        {
            g_web_server->broadcastScanComplete();
        }
    }
    return 0;
}

bool WandBLEClient::startScan(int duration_seconds)
{
    if (connected)
    {
        ESP_LOGW(TAG, "Cannot scan while connected to wand");
        return false;
    }

    if (scanning)
    {
        ESP_LOGW(TAG, "Already scanning");
        return false;
    }

    struct ble_gap_disc_params disc_params;
    memset(&disc_params, 0, sizeof(disc_params));
    disc_params.filter_duplicates = 1;
    disc_params.passive = 0;
    disc_params.itvl = 0;
    disc_params.window = 0;
    disc_params.filter_policy = 0;
    disc_params.limited = 0;

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, duration_seconds * 1000, &disc_params, ble_scan_callback, NULL);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to start BLE scan: %d", rc);
        return false;
    }

    scanning = true;
    ESP_LOGI(TAG, "BLE scan started for %d seconds", duration_seconds);
    return true;
}

void WandBLEClient::stopScan()
{
    if (scanning)
    {
        ble_gap_disc_cancel();
        scanning = false;
        if (g_web_server)
        {
            g_web_server->broadcastScanComplete();
        }
        ESP_LOGI(TAG, "BLE scan stopped");
    }
}
