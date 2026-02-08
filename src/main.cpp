#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "ble_client.h"
#include "config.h"
#include "usb_hid.h"
#include "web_server.h"
#include "ha_mqtt.h"

static const char *TAG = "main";

// Seeeduino XIAO ESP32S3 antenna switch GPIO
// Some XIAO ESP32S3 use GPIO14, others GPIO3
// Try GPIO14 first (most common for XIAO Sense), change to GPIO3 if needed
#define ANTENNA_SWITCH_GPIO GPIO_NUM_14
#define USE_EXTERNAL_ANTENNA 1 // Set to 1 for external antenna, 0 for internal

// MAC address formatting macros (if not defined by esp_wifi)
#ifndef MACSTR
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#endif
#ifndef MAC2STR
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#endif

// WiFi credentials defined in config.h
// #define WIFI_SSID "YourWiFiSSID"
// #define WIFI_PASS "YourWiFiPassword"

// Model data
const unsigned char *model_data = nullptr;
size_t model_size = 0;

// BLE Client
WandBLEClient wandClient;

// WiFi event handler for AP mode
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_AP_STACONNECTED:
        {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "==============================================");
            ESP_LOGI(TAG, "✓✓✓ CLIENT CONNECTED ✓✓✓");
            ESP_LOGI(TAG, "  MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     event->mac[0], event->mac[1], event->mac[2],
                     event->mac[3], event->mac[4], event->mac[5]);
            ESP_LOGI(TAG, "  AID: %d", event->aid);
            ESP_LOGI(TAG, "  DHCP will assign IP: 192.168.4.x");
            ESP_LOGI(TAG, "  Open browser: http://192.168.4.1/");
            ESP_LOGI(TAG, "==============================================");
            ESP_LOGI(TAG, "");
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED:
        {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "✗✗✗ CLIENT DISCONNECTED ✗✗✗");
            ESP_LOGI(TAG, "  MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     event->mac[0], event->mac[1], event->mac[2],
                     event->mac[3], event->mac[4], event->mac[5]);
            ESP_LOGI(TAG, "  AID: %d", event->aid);
            ESP_LOGI(TAG, "  Reason: Android may auto-disconnect (no internet)");
            ESP_LOGI(TAG, "");
            break;
        }
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "✓ WiFi AP started successfully");
            break;
        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG, "✗ WiFi AP stopped");
            break;
        case WIFI_EVENT_AP_PROBEREQRECVED:
            ESP_LOGI(TAG, "→ Probe request received (device scanning)");
            break;
        default:
            ESP_LOGI(TAG, "WiFi event: %ld", event_id);
            break;
        }
    }
}

#if USE_USB_HID_DEVICE
// USB HID (mouse + keyboard) - only for ESP32-S2/S3/P4
USBHIDManager usbHID;
#endif

// Web Server
WebServer webServer;

// Home Assistant MQTT Client
HAMqttClient mqttClient;

// Function to load model from filesystem
bool loadModel()
{
    ESP_LOGI(TAG, "Loading TFLite model from flash partition...");

    // Find the model partition
    const esp_partition_t *model_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "model");

    if (!model_partition)
    {
        ESP_LOGE(TAG, "Model partition not found!");
        return false;
    }

    ESP_LOGI(TAG, "Model partition found: size=%lu bytes at offset=0x%lx",
             model_partition->size, model_partition->address);

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Free heap: %lu bytes, Free PSRAM: %lu bytes", free_heap, free_psram);

    // Allocate model in PSRAM (ESP32-S3 cannot use memory-mapped flash for TFLite)
    model_size = model_partition->size;
    unsigned char *buffer = (unsigned char *)heap_caps_malloc(model_size, MALLOC_CAP_SPIRAM);

    if (!buffer)
    {
        ESP_LOGE(TAG, "Failed to allocate %lu bytes in PSRAM!", model_size);
        ESP_LOGE(TAG, "Free PSRAM: %lu bytes, Free heap: %lu bytes", free_psram, free_heap);
        return false;
    }

    ESP_LOGI(TAG, "✓ Allocated %lu bytes in PSRAM at %p", model_size, buffer);

    // Read model from flash into PSRAM
    esp_err_t err = esp_partition_read(model_partition, 0, buffer, model_size);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read model: %s", esp_err_to_name(err));
        heap_caps_free(buffer);
        return false;
    }

    model_data = buffer;

    ESP_LOGI(TAG, "✓ Model loaded into PSRAM!");
    ESP_LOGI(TAG, "   Model pointer: %p", model_data);
    ESP_LOGI(TAG, "   Model size: %zu bytes", model_size);
    ESP_LOGI(TAG, "   Free PSRAM: %lu bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    return true;
}

// Callback when spell is detected
void onSpellDetected(const char *spell_name, float confidence)
{
    if (!spell_name)
    {
        ESP_LOGW(TAG, "Spell detected with NULL name!");
        return;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "🪄 SPELL DETECTED: %s", spell_name);
    ESP_LOGI(TAG, "   Confidence: %.2f%%", confidence * 100.0f);
    ESP_LOGI(TAG, "========================================");

    // Play spell effect using macro system
    wandClient.playSpellEffect(spell_name);

#if USE_USB_HID_DEVICE
    // Send spell as keyboard input
    usbHID.sendSpellKeyboard(spell_name);
#endif

#if ENABLE_HOME_ASSISTANT
    ESP_LOGI(TAG, "🎯 Spell detected in callback - processing...");

    // Broadcast to web clients
    ESP_LOGI(TAG, "  → Broadcasting to web clients");
    webServer.broadcastSpell(spell_name, confidence);

    // Send to Home Assistant via MQTT (only if connected)
    ESP_LOGI(TAG, "  → Checking MQTT connection (isConnected=%d)", mqttClient.isConnected());
    if (mqttClient.isConnected())
    {
        ESP_LOGI(TAG, "  → Calling mqttClient.publishSpell()");
        mqttClient.publishSpell(spell_name, confidence);
    }
    else
    {
        ESP_LOGW(TAG, "  ⚠ MQTT not connected - skipping MQTT publish");
    }
#endif
}

// Callback when MQTT connects - check if wand is already connected and publish its info
void onMQTTConnected()
{
    ESP_LOGI(TAG, "MQTT connected callback triggered");

    // Check if wand is already connected
    if (wandClient.isConnected())
    {
        ESP_LOGI(TAG, "Wand already connected - publishing info to Home Assistant...");

        // Request fresh wand info
        if (wandClient.requestWandInfo())
        {
            vTaskDelay(pdMS_TO_TICKS(300));

            mqttClient.publishWandInfo(
                wandClient.getFirmwareVersion(),
                wandClient.getSerialNumber(),
                wandClient.getSKU(),
                wandClient.getDeviceId(),
                wandClient.getWandType(),
                wandClient.getWandMacAddress());
        }
    }
    else
    {
        ESP_LOGI(TAG, "No wand connected yet");
    }
}

// Callback when connection state changes
void onConnectionChange(bool connected)
{
    if (connected)
    {
        ESP_LOGI(TAG, "✓ Connected to wand");

        // Request wand information (firmware, serial, etc.)
        if (wandClient.requestWandInfo())
        {
            vTaskDelay(pdMS_TO_TICKS(500)); // Give time for info to be retrieved

            // Publish wand info to Home Assistant
#if ENABLE_HOME_ASSISTANT
            if (mqttClient.isConnected())
            {
                ESP_LOGI(TAG, "Publishing wand information to Home Assistant...");
                mqttClient.publishWandInfo(
                    wandClient.getFirmwareVersion(),
                    wandClient.getSerialNumber(),
                    wandClient.getSKU(),
                    wandClient.getDeviceId(),
                    wandClient.getWandType(),
                    wandClient.getWandMacAddress());
            }
#endif
        }

        // Notify web GUI
        webServer.broadcastWandStatus(true);
    }
    else
    {
        ESP_LOGI(TAG, "✗ Disconnected from wand");

        // Publish disconnected status to Home Assistant
#if ENABLE_HOME_ASSISTANT
        if (mqttClient.isConnected())
        {
            ESP_LOGI(TAG, "Publishing wand disconnection to Home Assistant...");
            mqttClient.publishWandDisconnected();
        }
#endif

        // Check if this was a user-initiated disconnect
        if (wandClient.isUserDisconnectRequested())
        {
            ESP_LOGI(TAG, "User-initiated disconnect - auto-reconnect disabled");
            ESP_LOGI(TAG, "To reconnect, use the web interface scan and connect");
        }
        else
        {
            ESP_LOGI(TAG, "Unexpected disconnect - auto-reconnect may be needed");
        }
        // Notify web GUI
        webServer.broadcastWandStatus(false);
    }
}

// Callback when IMU data is received
void onIMUData(float ax, float ay, float az, float gx, float gy, float gz)
{
    // Update AHRS tracker with IMU data (moved here from BLE callback to avoid mbuf corruption)
    // IMUSample struct order is {gyro_x, gyro_y, gyro_z, accel_x, accel_y, accel_z}
    IMUSample sample = {gx, gy, gz, ax, ay, az};
    wandClient.updateAHRS(sample);

#if USE_USB_HID_DEVICE
    // Mouse movement is handled via AHRS gesture path in updateAHRS()
#endif

#if ENABLE_HOME_ASSISTANT
    // Broadcast to web clients - rate limited to ~60 Hz (every 4th sample at 234 Hz)
    static uint8_t web_update_counter = 0;
    if (++web_update_counter >= 4)
    {
        webServer.broadcastIMU(ax, ay, az, gx, gy, gz);
        web_update_counter = 0;
    }
#endif
}

extern "C" void app_main()
{
    // Wait 3 seconds for serial monitor to connect and catch all startup logs
    vTaskDelay(30000 / portTICK_PERIOD_MS);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  ESP32-S3 Magic Wand Gateway Starting...");
    ESP_LOGI(TAG, "  Seeeduino XIAO ESP32S3");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "");

    // Configure antenna switch for Seeeduino XIAO ESP32S3
    ESP_LOGI(TAG, "Configuring RF antenna...");
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << ANTENNA_SWITCH_GPIO);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

#if USE_EXTERNAL_ANTENNA
    gpio_set_level(ANTENNA_SWITCH_GPIO, 1); // HIGH = external U.FL antenna
    ESP_LOGI(TAG, "✓ Using EXTERNAL antenna (U.FL connector on GPIO3)");
    ESP_LOGI(TAG, "  Make sure antenna is properly attached!");
#else
    gpio_set_level(ANTENNA_SWITCH_GPIO, 0); // LOW = internal PCB antenna
    ESP_LOGI(TAG, "✓ Using INTERNAL PCB antenna");
#endif
    ESP_LOGI(TAG, "");

    // Check PSRAM status early
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=== PSRAM Diagnostic ===");
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM Total: %zu bytes (%zu KB)", psram_size, psram_size / 1024);
    ESP_LOGI(TAG, "PSRAM Free:  %zu bytes (%zu KB)", psram_free, psram_free / 1024);
    if (psram_size == 0)
    {
        ESP_LOGW(TAG, "PSRAM NOT DETECTED!");
        ESP_LOGW(TAG, "Check: CONFIG_SPIRAM=y, CONFIG_SPIRAM_MODE_QUAD=y or OCT");
    }
    else
    {
        ESP_LOGI(TAG, "✓ PSRAM available!");
    }
    ESP_LOGI(TAG, "Internal heap: %zu bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "========================");
    ESP_LOGI(TAG, "");

    // Initialize NVS (required for WiFi/BLE)
    ESP_LOGI(TAG, "Initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "✓ NVS initialized");

    // Read stored wand MAC address from NVS
    char stored_mac[18] = {0};
    bool mac_from_nvs = false;
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK)
    {
        size_t required_size = sizeof(stored_mac);
        err = nvs_get_str(nvs_handle, "wand_mac", stored_mac, &required_size);
        if (err == ESP_OK && strlen(stored_mac) > 0)
        {
            mac_from_nvs = true;
            ESP_LOGI(TAG, "✓ Using stored wand MAC: %s", stored_mac);
        }
        nvs_close(nvs_handle);
    }

    // Fall back to config.h MAC if not found in NVS
    const char *wand_mac = mac_from_nvs ? stored_mac : WAND_MAC_ADDRESS;

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ESP32 Magic Wand Gateway");
    ESP_LOGI(TAG, "  TensorFlow Lite Spell Detection");
    ESP_LOGI(TAG, "  Wand: %s %s", wand_mac, mac_from_nvs ? "(stored)" : "(config.h)");
    ESP_LOGI(TAG, "========================================");

#if ENABLE_HOME_ASSISTANT
    // Initialize network stack (required for web server even without WiFi)
    ESP_LOGI(TAG, "Initializing network stack...");
    esp_err_t net_err = esp_netif_init();
    if (net_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init netif: %s - continuing without network", esp_err_to_name(net_err));
    }
    else
    {
        net_err = esp_event_loop_create_default();
        if (net_err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to create event loop: %s - continuing without network", esp_err_to_name(net_err));
        }
    }

    if (net_err == ESP_OK)
    {
#if USE_WIFI_AP_MODE == 0
        // Access Point Mode - Create WiFi AP for direct connection
        ESP_LOGI(TAG, "Initializing WiFi Access Point...");

        // Register WiFi event handler
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

        esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
        (void)ap_netif; // Suppress unused variable warning

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        // Set country code for better compatibility
        wifi_country_t country = {
            .cc = "US",
            .schan = 1,
            .nchan = 11,
            .max_tx_power = 20,
            .policy = WIFI_COUNTRY_POLICY_AUTO};
        ESP_ERROR_CHECK(esp_wifi_set_country(&country));

        // Load AP settings from NVS (fallback to config.h defaults)
        char ap_ssid[32] = {0};
        char ap_password[64] = {0};
        uint8_t ap_channel = 6;       // Default channel
        bool hotspot_enabled = false; // By default use config.h values

        // Check for saved WiFi station credentials
        char wifi_sta_ssid[32] = {0};
        char wifi_sta_password[64] = {0};
        bool has_wifi_credentials = false;

        nvs_handle_t nvs_handle;
        esp_err_t nvs_err = nvs_open("storage", NVS_READONLY, &nvs_handle);
        if (nvs_err == ESP_OK)
        {
            // Load hotspot settings
            uint8_t hotspot_en = 0;
            nvs_get_u8(nvs_handle, "hotspot_enabled", &hotspot_en);
            hotspot_enabled = (hotspot_en != 0);

            size_t required_size;

            // Load hotspot SSID
            required_size = sizeof(ap_ssid);
            nvs_get_str(nvs_handle, "hotspot_ssid", ap_ssid, &required_size);

            // Load hotspot password
            required_size = sizeof(ap_password);
            nvs_get_str(nvs_handle, "hotspot_password", ap_password, &required_size);

            // Load hotspot channel
            uint8_t channel = 6;
            if (nvs_get_u8(nvs_handle, "hotspot_channel", &channel) == ESP_OK)
            {
                if (channel >= 1 && channel <= 13)
                {
                    ap_channel = channel;
                }
            }

            // Check for WiFi station credentials
            nvs_err = nvs_get_str(nvs_handle, "wifi_ssid", NULL, &required_size);
            if (nvs_err == ESP_OK && required_size > 0 && required_size <= sizeof(wifi_sta_ssid))
            {
                nvs_get_str(nvs_handle, "wifi_ssid", wifi_sta_ssid, &required_size);
                if (strlen(wifi_sta_ssid) > 0)
                {
                    has_wifi_credentials = true;
                    // Load password too
                    required_size = sizeof(wifi_sta_password);
                    nvs_get_str(nvs_handle, "wifi_password", wifi_sta_password, &required_size);
                }
            }

            nvs_close(nvs_handle);
        }

        // If WiFi credentials are saved, try to connect as station first
        if (has_wifi_credentials)
        {
            ESP_LOGI(TAG, "Found saved WiFi credentials for: %s", wifi_sta_ssid);
            ESP_LOGI(TAG, "Attempting to connect to WiFi network...");

            esp_netif_create_default_wifi_sta();

            wifi_config_t wifi_config = {};
            strncpy((char *)wifi_config.sta.ssid, wifi_sta_ssid, sizeof(wifi_config.sta.ssid) - 1);
            strncpy((char *)wifi_config.sta.password, wifi_sta_password, sizeof(wifi_sta_password) - 1);
            wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
            wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';

            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
            ESP_ERROR_CHECK(esp_wifi_start());
            ESP_ERROR_CHECK(esp_wifi_connect());

            ESP_LOGI(TAG, "Connecting to %s...", wifi_sta_ssid);

            // Wait for connection (timeout after 10 seconds)
            int wait_count = 0;
            while (wait_count < 20) // 20 * 500ms = 10 seconds
            {
                wifi_ap_record_t ap_info;
                if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
                {
                    ESP_LOGI(TAG, "✓ Connected to WiFi: %s", wifi_sta_ssid);
                    ESP_LOGI(TAG, "✓ WiFi Station mode active");
                    // Successfully connected - continue without falling back to AP
                    goto wifi_setup_complete;
                }
                vTaskDelay(pdMS_TO_TICKS(500));
                wait_count++;
            }

            // Connection failed - fall back to AP mode
            ESP_LOGW(TAG, "Failed to connect to %s, falling back to AP mode", wifi_sta_ssid);
            esp_wifi_stop();
            esp_wifi_deinit();

            // Reinitialize for AP mode
            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        }

        // Use config.h defaults for AP SSID if not set in NVS
        if (strlen(ap_ssid) == 0)
        {
            strncpy(ap_ssid, AP_SSID, sizeof(ap_ssid) - 1);
        }

        // Use config.h defaults for AP password if not set in NVS
        if (strlen(ap_password) == 0)
        {
            strncpy(ap_password, AP_PASSWORD, sizeof(ap_password) - 1);
        }

        // Configure AP with improved compatibility settings
        wifi_config_t wifi_config = {};
        strncpy((char *)wifi_config.ap.ssid, ap_ssid, sizeof(wifi_config.ap.ssid) - 1);
        wifi_config.ap.ssid[sizeof(wifi_config.ap.ssid) - 1] = '\0';
        wifi_config.ap.ssid_len = strlen((char *)wifi_config.ap.ssid);
        wifi_config.ap.channel = ap_channel;
        wifi_config.ap.max_connection = AP_MAX_CONNECTIONS;
        wifi_config.ap.beacon_interval = 100;

        ESP_LOGI(TAG, "Configuring AP: SSID=%s, password_len=%d", ap_ssid, strlen(ap_password));

        // Use WPA2 security (Android often refuses open networks)
        if (strlen(ap_password) >= 8)
        {
            strncpy((char *)wifi_config.ap.password, ap_password, sizeof(wifi_config.ap.password) - 1);
            wifi_config.ap.password[sizeof(wifi_config.ap.password) - 1] = '\0';
            wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
            wifi_config.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
            ESP_LOGI(TAG, "AP mode: WPA2-PSK with password");
        }
        else
        {
            wifi_config.ap.authmode = WIFI_AUTH_OPEN;
            wifi_config.ap.pairwise_cipher = WIFI_CIPHER_TYPE_NONE;
            ESP_LOGI(TAG, "AP mode: OPEN (no password - password too short)");
        }
        wifi_config.ap.ssid_hidden = 0; // Broadcast SSID

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

        // Set bandwidth to 20MHz for better compatibility
        ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20));

        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "✓ WiFi AP started: %s", ap_ssid);
        ESP_LOGI(TAG, "  Channel: %d (2.4GHz)", ap_channel);
        ESP_LOGI(TAG, "  Bandwidth: 20MHz");
        if (strlen(ap_password) >= 8)
        {
            ESP_LOGI(TAG, "  Security: WPA2-PSK");
            ESP_LOGI(TAG, "  Password: %s", ap_password);
        }
        else
        {
            ESP_LOGI(TAG, "  Security: Open (no password)");
        }
        ESP_LOGI(TAG, "  IP Address: 192.168.4.1");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Connect your device to '%s' WiFi network", ap_ssid);
        if (strlen(ap_password) >= 8)
        {
            ESP_LOGI(TAG, "Password: %s", ap_password);
        }
        ESP_LOGI(TAG, "Then open browser: http://192.168.4.1/");

    wifi_setup_complete:
        // WiFi setup complete (either STA or AP mode)
        ESP_LOGI(TAG, "WiFi initialization complete");
#else
        // Station Mode with AP fallback
        // Register WiFi event handler
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

        // Set country code for better compatibility
        wifi_country_t country = {
            .cc = "US",
            .schan = 1,
            .nchan = 11,
            .max_tx_power = 20,
            .policy = WIFI_COUNTRY_POLICY_AUTO};

        // Load WiFi credentials from NVS
        char wifi_sta_ssid[32] = {0};
        char wifi_sta_password[64] = {0};
        bool has_nvs_wifi = false;
        bool wifi_connected = false;
        bool force_ap_mode = false;

        nvs_handle_t nvs_wifi;
        esp_err_t nvs_err = nvs_open("storage", NVS_READONLY, &nvs_wifi);
        if (nvs_err == ESP_OK)
        {
            // Check if forced AP mode is enabled
            uint8_t ap_mode = 0;
            nvs_err = nvs_get_u8(nvs_wifi, "force_ap_mode", &ap_mode);
            if (nvs_err == ESP_OK && ap_mode == 1)
            {
                force_ap_mode = true;
                ESP_LOGI(TAG, "✓ Forced AP mode enabled via NVS");
            }

            // Load WiFi station credentials
            size_t required_size = sizeof(wifi_sta_ssid);
            nvs_err = nvs_get_str(nvs_wifi, "wifi_ssid", wifi_sta_ssid, &required_size);
            if (nvs_err == ESP_OK && strlen(wifi_sta_ssid) > 0)
            {
                has_nvs_wifi = true;
                required_size = sizeof(wifi_sta_password);
                nvs_get_str(nvs_wifi, "wifi_password", wifi_sta_password, &required_size);
                ESP_LOGI(TAG, "✓ Using WiFi credentials from NVS: %s", wifi_sta_ssid);
            }

            nvs_close(nvs_wifi);
        }

        // Fall back to config.h if no NVS credentials
        if (!has_nvs_wifi && strcmp(WIFI_SSID, "your_wifi_ssid") != 0)
        {
            strncpy(wifi_sta_ssid, WIFI_SSID, sizeof(wifi_sta_ssid) - 1);
            strncpy(wifi_sta_password, WIFI_PASS, sizeof(wifi_sta_password) - 1);
            ESP_LOGI(TAG, "Using WiFi credentials from config.h: %s", wifi_sta_ssid);
        }

        // Try to connect to WiFi if we have credentials AND not forced to AP mode
        if (strlen(wifi_sta_ssid) > 0 && !force_ap_mode)
        {
            ESP_LOGI(TAG, "Initializing WiFi Station for Home Assistant...");
            esp_netif_create_default_wifi_sta();

            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            ESP_ERROR_CHECK(esp_wifi_init(&cfg));
            ESP_ERROR_CHECK(esp_wifi_set_country(&country));

            wifi_config_t wifi_config = {};
            strncpy((char *)wifi_config.sta.ssid, wifi_sta_ssid, sizeof(wifi_config.sta.ssid) - 1);
            strncpy((char *)wifi_config.sta.password, wifi_sta_password, sizeof(wifi_config.sta.password) - 1);
            wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
            wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';

            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
            ESP_ERROR_CHECK(esp_wifi_start());
            ESP_ERROR_CHECK(esp_wifi_connect());

            ESP_LOGI(TAG, "WiFi connecting to %s...", wifi_sta_ssid);
            ESP_LOGI(TAG, "Waiting for WiFi connection and IP address...");

            // Wait for connection AND IP address (timeout after 15 seconds)
            int wait_count = 0;
            while (wait_count < 30) // 30 * 500ms = 15 seconds
            {
                wifi_ap_record_t ap_info;
                esp_netif_ip_info_t ip_info;
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

                // Check both WiFi connection AND IP address assignment
                if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK &&
                    netif &&
                    esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
                    ip_info.ip.addr != 0)
                {
                    ESP_LOGI(TAG, "✓ Connected to WiFi: %s", wifi_sta_ssid);
                    ESP_LOGI(TAG, "✓ IP Address: " IPSTR, IP2STR(&ip_info.ip));
                    ESP_LOGI(TAG, "✓ Gateway: " IPSTR, IP2STR(&ip_info.gw));
                    ESP_LOGI(TAG, "✓ WiFi Station mode active");
                    wifi_connected = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(500));
                wait_count++;
            }

            if (!wifi_connected)
            {
                ESP_LOGW(TAG, "Failed to connect to %s, falling back to AP mode", wifi_sta_ssid);
                esp_wifi_stop();
                esp_wifi_deinit();
            }
        }

        // Start AP mode if no WiFi credentials or WiFi connection failed
        if (strlen(wifi_sta_ssid) == 0 || !wifi_connected || force_ap_mode)
        {
            if (force_ap_mode)
            {
                ESP_LOGI(TAG, "Starting AP mode (forced via WiFi mode switcher)...");
            }
            else if (!wifi_connected && strlen(wifi_sta_ssid) > 0)
            {
                ESP_LOGI(TAG, "Starting AP mode as fallback...");
            }
            else
            {
                ESP_LOGI(TAG, "Starting AP mode (no WiFi credentials)...");
            }

            // Create AP netif if not already created
            if (!wifi_connected)
            {
                esp_netif_create_default_wifi_ap();
                wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
                ESP_ERROR_CHECK(esp_wifi_init(&cfg));
                ESP_ERROR_CHECK(esp_wifi_set_country(&country));
            }

            // Configure AP with default settings from config.h
            wifi_config_t wifi_config = {};
            strncpy((char *)wifi_config.ap.ssid, AP_SSID, sizeof(wifi_config.ap.ssid) - 1);
            wifi_config.ap.ssid[sizeof(wifi_config.ap.ssid) - 1] = '\0';
            wifi_config.ap.ssid_len = strlen((char *)wifi_config.ap.ssid);
            wifi_config.ap.channel = AP_CHANNEL;
            wifi_config.ap.max_connection = AP_MAX_CONNECTIONS;
            wifi_config.ap.beacon_interval = 100;

            // Use WPA2 security if password is set in config.h, otherwise open
            if (strlen(AP_PASSWORD) >= 8)
            {
                strncpy((char *)wifi_config.ap.password, AP_PASSWORD, sizeof(wifi_config.ap.password) - 1);
                wifi_config.ap.password[sizeof(wifi_config.ap.password) - 1] = '\0';
                wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
                wifi_config.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
                ESP_LOGI(TAG, "AP mode: WPA2-PSK with password");
            }
            else
            {
                wifi_config.ap.authmode = WIFI_AUTH_OPEN;
                wifi_config.ap.pairwise_cipher = WIFI_CIPHER_TYPE_NONE;
                ESP_LOGI(TAG, "AP mode: OPEN (no password)");
            }
            wifi_config.ap.ssid_hidden = 0;

            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
            ESP_ERROR_CHECK(esp_wifi_start());

            ESP_LOGI(TAG, "✓ WiFi AP started: %s", AP_SSID);
            ESP_LOGI(TAG, "  Channel: %d (2.4GHz)", AP_CHANNEL);
            if (strlen(AP_PASSWORD) >= 8)
            {
                ESP_LOGI(TAG, "  Security: WPA2-PSK");
            }
            else
            {
                ESP_LOGI(TAG, "  Security: Open (no password)");
            }
            ESP_LOGI(TAG, "  IP Address: 192.168.4.1");
            ESP_LOGI(TAG, "Connect your device to '%s' WiFi network", AP_SSID);
        }

        // Continue with MQTT setup if WiFi is connected
        if (wifi_connected)
        {

            // Check if MQTT is enabled in NVS settings
            bool ha_mqtt_enabled = true; // Default: enabled
            char mqtt_broker[128] = {0};
            char mqtt_username[64] = {0};
            char mqtt_password[64] = {0};

            nvs_handle_t nvs_handle;
            esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
            if (err == ESP_OK)
            {
                uint8_t ha_mqtt_u8 = 1;
                nvs_get_u8(nvs_handle, "ha_mqtt_enabled", &ha_mqtt_u8);
                ha_mqtt_enabled = (ha_mqtt_u8 != 0);

                // Load MQTT settings from NVS (fallback to config.h defaults)
                size_t required_size;
                err = nvs_get_str(nvs_handle, "mqtt_broker", NULL, &required_size);
                if (err == ESP_OK && required_size > 0 && required_size <= sizeof(mqtt_broker))
                {
                    nvs_get_str(nvs_handle, "mqtt_broker", mqtt_broker, &required_size);
                }

                err = nvs_get_str(nvs_handle, "mqtt_username", NULL, &required_size);
                if (err == ESP_OK && required_size > 0 && required_size <= sizeof(mqtt_username))
                {
                    nvs_get_str(nvs_handle, "mqtt_username", mqtt_username, &required_size);
                }

                err = nvs_get_str(nvs_handle, "mqtt_password", NULL, &required_size);
                if (err == ESP_OK && required_size > 0 && required_size <= sizeof(mqtt_password))
                {
                    nvs_get_str(nvs_handle, "mqtt_password", mqtt_password, &required_size);
                }

                nvs_close(nvs_handle);
            }

            // Use defaults from config.h if NVS values are empty
            if (strlen(mqtt_broker) == 0)
            {
                snprintf(mqtt_broker, sizeof(mqtt_broker), "mqtt://%s:%d", MQTT_SERVER, MQTT_PORT);
            }
            else if (strncmp(mqtt_broker, "mqtt://", 7) != 0)
            {
                // Add mqtt:// prefix if not present
                char temp[121];
                strncpy(temp, mqtt_broker, 120);
                temp[120] = '\0';
                snprintf(mqtt_broker, sizeof(mqtt_broker), "mqtt://%s", temp);
            }

            if (strlen(mqtt_username) == 0)
            {
                strncpy(mqtt_username, MQTT_USER, sizeof(mqtt_username) - 1);
            }

            if (strlen(mqtt_password) == 0)
            {
                strncpy(mqtt_password, MQTT_PASSWORD, sizeof(mqtt_password) - 1);
            }

            if (ha_mqtt_enabled)
            {
                // Give network stack time to fully initialize after getting IP
                ESP_LOGI(TAG, "Waiting 2 seconds for network stack to stabilize...");
                vTaskDelay(pdMS_TO_TICKS(2000));

                // Validate broker address before attempting connection
                bool broker_valid = (strlen(mqtt_broker) > 7 &&
                                     strstr(mqtt_broker, "mqtt://") != nullptr);

                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                ESP_LOGI(TAG, "Network Status:");

                // Show current IP address
                esp_netif_ip_info_t ip_info;
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
                {
                    ESP_LOGI(TAG, "  ESP32 IP: " IPSTR, IP2STR(&ip_info.ip));
                    ESP_LOGI(TAG, "  Gateway: " IPSTR, IP2STR(&ip_info.gw));
                    ESP_LOGI(TAG, "  Netmask: " IPSTR, IP2STR(&ip_info.netmask));
                }
                else
                {
                    ESP_LOGW(TAG, "  ESP32 IP: Not connected to WiFi (using AP mode?)");
                }

                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
                ESP_LOGI(TAG, "MQTT Configuration:");
                ESP_LOGI(TAG, "  Broker: %s", mqtt_broker);
                ESP_LOGI(TAG, "  Username: %s", mqtt_username);
                ESP_LOGI(TAG, "  Password: %s", strlen(mqtt_password) > 0 ? "***" : "(empty)");
                ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");

                if (!broker_valid)
                {
                    ESP_LOGW(TAG, "⚠️  Invalid MQTT broker URI: '%s'", mqtt_broker);
                    ESP_LOGW(TAG, "→ Expected format: mqtt://hostname:port or mqtt://IP:port");
                    ESP_LOGW(TAG, "→ MQTT disabled - configure via web interface");
                }
                else
                {
                    // Initialize MQTT client for Home Assistant with settings from NVS or config.h
                    ESP_LOGI(TAG, "🔌 Attempting connection to MQTT broker...");
                    ESP_LOGI(TAG, "   Timeout: 5s, Reconnect interval: 30s");
                    if (mqttClient.begin(mqtt_broker, mqtt_username, mqtt_password))
                    {
                        ESP_LOGI(TAG, "✓ MQTT client initialized for Home Assistant");
                        ESP_LOGI(TAG, "   Connection errors will retry every 30 seconds");

                        // Register callback to publish wand info if already connected
                        mqttClient.onConnected(onMQTTConnected);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "⚠️  MQTT initialization failed - continuing without Home Assistant");
                        ESP_LOGW(TAG, "→ Check broker IP address (should match your Home Assistant IP)");
                        ESP_LOGW(TAG, "→ Check username/password in web GUI Settings tab");
                        ESP_LOGW(TAG, "→ Or disable MQTT in Settings to stop connection attempts");
                    }
                }
            }
            else
            {
                ESP_LOGI(TAG, "Home Assistant MQTT disabled (configure via web interface)");
            }
        }
        else
        {
            ESP_LOGI(TAG, "Home Assistant disabled (no WiFi connection - using AP mode)");
        }
#endif
    } // End of net_err == ESP_OK check

    // Start web server (now starts before wand connection to allow BLE scanning)
    ESP_LOGI(TAG, "Starting web server...");
    if (webServer.begin())
    {
        ESP_LOGI(TAG, "✓ Web server ready: http://esp32.local/");

        // Set wand client reference for web server HTTP handlers
        webServer.setWandClient(&wandClient);
        ESP_LOGI(TAG, "✓ Web server linked to wand client");
    }
    else
    {
        ESP_LOGW(TAG, "WARNING: Web server initialization failed");
    }
#else
    ESP_LOGI(TAG, "Home Assistant and Web Server disabled to save RAM for model");
#endif

#if USE_USB_HID_DEVICE
    // Initialize USB HID
    ESP_LOGI(TAG, "Initializing USB HID...");
    if (usbHID.begin())
    {
        ESP_LOGI(TAG, "✓ USB HID ready (Mouse + Keyboard)");
        usbHID.setMouseSensitivity(1.5f); // Adjust as needed
    }
    else
    {
        ESP_LOGW(TAG, "WARNING: USB HID initialization failed");
    }
#else
    ESP_LOGI(TAG, "USB HID not available on this chip (needs ESP32-S2/S3/P4)");
#endif

    // Load model from filesystem
    bool model_loaded = loadModel();
    if (!model_loaded)
    {
        ESP_LOGW(TAG, "WARNING: Failed to load model!");
        ESP_LOGW(TAG, "Continuing without spell detection (model not found)");
        ESP_LOGW(TAG, "To flash model: esptool.py --chip esp32s3 --port /dev/ttyACM0 write_flash 0x410000 model.tflite");
        // Set model pointers to NULL so wand client knows to skip spell detection
        model_data = NULL;
        model_size = 0;
    }

    // Initialize BLE client and spell detector (can work without model)
    if (!wandClient.begin(model_data, model_size))
    {
        ESP_LOGE(TAG, "ERROR: Failed to initialize wand client!");
        if (model_loaded)
        {
            ESP_LOGE(TAG, "System halted.");
            while (1)
            {
                vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
        }
        else
        {
            ESP_LOGW(TAG, "Continuing without spell detection (BLE connection only)");
        }
    }

    // Set callbacks
    wandClient.onSpellDetected(onSpellDetected);
    wandClient.onConnectionChange(onConnectionChange);
    wandClient.onIMUData(onIMUData);

    // Set web server for gesture visualization
    wandClient.setWebServer(&webServer);

    // Validate configuration - only skip auto-connect if NO stored MAC and using default config.h MAC
    if (!mac_from_nvs && strcmp(WAND_MAC_ADDRESS, "C2:BD:5D:3C:67:4E") == 0)
    {
        ESP_LOGW(TAG, "WARNING: Using default MAC address!");
        ESP_LOGW(TAG, "Please update WAND_MAC_ADDRESS in config.h or use the web interface to set a MAC");
        ESP_LOGW(TAG, "WiFi hotspot is available for configuration: http://192.168.4.1/");
        ESP_LOGW(TAG, "Skipping automatic connection - use web interface to scan and connect");

        // Skip connection when using default MAC
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "✓ System ready! (waiting for wand configuration)");
        ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        ESP_LOGI(TAG, "  Free heap: %lu bytes", esp_get_free_heap_size());
        ESP_LOGI(TAG, "  Min free heap: %lu bytes", esp_get_minimum_free_heap_size());
#if CONFIG_SPIRAM
        ESP_LOGI(TAG, "  Free PSRAM: %lu bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif
        ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        ESP_LOGI(TAG, "CONFIGURATION STEPS:");
        ESP_LOGI(TAG, "  1. Connect to WiFi: %s", AP_SSID);
        ESP_LOGI(TAG, "  2. Open browser: http://192.168.4.1/");
        ESP_LOGI(TAG, "  3. Use 'Scan for Wands' to find your wand");
        ESP_LOGI(TAG, "  4. Click 'Connect' to establish connection");
        ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        ESP_LOGI(TAG, "");
    }
    else
    {
        // Connect to wand with stored/configured MAC (either from NVS or config.h with non-default value)
        ESP_LOGI(TAG, "Connecting to wand at %s%s...", wand_mac, mac_from_nvs ? " (from NVS)" : " (from config.h)");
        int connect_attempts = 0;
        const int MAX_CONNECT_ATTEMPTS = 3;

        while (!wandClient.connect(wand_mac) && connect_attempts < MAX_CONNECT_ATTEMPTS)
        {
            connect_attempts++;
            ESP_LOGW(TAG, "Connection attempt %d/%d failed, retrying in 5 seconds...",
                     connect_attempts, MAX_CONNECT_ATTEMPTS);
            vTaskDelay(5000 / portTICK_PERIOD_MS);
        }

        if (connect_attempts >= MAX_CONNECT_ATTEMPTS)
        {
            ESP_LOGE(TAG, "ERROR: Failed to connect to wand after %d attempts!", MAX_CONNECT_ATTEMPTS);
            ESP_LOGE(TAG, "Please check:");
            ESP_LOGE(TAG, "  1. Wand is powered on and nearby");
            ESP_LOGE(TAG, "  2. MAC address is correct: %s", wand_mac);
            ESP_LOGE(TAG, "  3. No other device is connected to the wand");
            ESP_LOGE(TAG, "System will keep retrying in main loop...");
            ESP_LOGE(TAG, "You can also use the web interface to scan and connect");
            // Don't restart - let main loop handle reconnection
        }
        else
        {
            // Wait longer for service discovery to complete
            ESP_LOGI(TAG, "Waiting for service discovery...");
            vTaskDelay(5000 / portTICK_PERIOD_MS);

            // Initialize button thresholds
            ESP_LOGI(TAG, "Initializing button thresholds...");
            if (!wandClient.initButtonThresholds())
            {
                ESP_LOGW(TAG, "WARNING: Failed to initialize button thresholds");
            }

            // Request wand information (firmware, product ID, name)
            ESP_LOGI(TAG, "Requesting wand information...");
            if (!wandClient.requestWandInfo())
            {
                ESP_LOGW(TAG, "WARNING: Failed to request wand information");
            }
            else
            {
                // Wait for info to be retrieved
                vTaskDelay(300 / portTICK_PERIOD_MS);

                // Publish wand info to Home Assistant
#if ENABLE_HOME_ASSISTANT
                if (mqttClient.isConnected())
                {
                    ESP_LOGI(TAG, "Publishing wand info to Home Assistant...");
                    mqttClient.publishWandInfo(
                        wandClient.getFirmwareVersion(),
                        wandClient.getSerialNumber(),
                        wandClient.getSKU(),
                        wandClient.getDeviceId(),
                        wandClient.getWandType(),
                        wandClient.getWandMacAddress());
                }
                else
                {
                    ESP_LOGW(TAG, "MQTT not connected - wand info not published");
                }
#endif
            }

            // Wait before starting IMU streaming
            vTaskDelay(500 / portTICK_PERIOD_MS);

            if (!wandClient.startIMUStreaming())
            {
                ESP_LOGW(TAG, "WARNING: Failed to start IMU streaming");
            }
            else
            {
                ESP_LOGI(TAG, "✓ IMU streaming started");
            }

            // Print battery level
            uint8_t battery = wandClient.getBatteryLevel();
            if (battery > 0)
            {
                ESP_LOGI(TAG, "Battery level: %d%%", battery);
#if ENABLE_HOME_ASSISTANT
                webServer.broadcastBattery(battery);
#endif
            }
        }

        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "✓ System ready!");
        ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        ESP_LOGI(TAG, "  Free heap: %lu bytes", esp_get_free_heap_size());
        ESP_LOGI(TAG, "  Min free heap: %lu bytes", esp_get_minimum_free_heap_size());
#if CONFIG_SPIRAM
        ESP_LOGI(TAG, "  Free PSRAM: %lu bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif
        ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        ESP_LOGI(TAG, "HOW TO CAST A SPELL:");
        ESP_LOGI(TAG, "  1. Press and HOLD all 4 wand buttons");
        ESP_LOGI(TAG, "  2. Draw your spell gesture in the air");
        ESP_LOGI(TAG, "  3. Release all buttons to detect spell");
        ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        ESP_LOGI(TAG, "");
    }

    // Main loop
    uint32_t battery_check_counter = 0;
    const uint32_t BATTERY_CHECK_INTERVAL = 100; // Check every 10 seconds (100 * 100ms)
    uint32_t keepalive_counter = 0;
    const uint32_t KEEPALIVE_INTERVAL = 30; // Send keep-alive every 3 seconds (30 * 100ms)
    uint32_t reconnect_attempts = 0;
    const uint32_t MAX_RECONNECT_ATTEMPTS = 3; // Try 3 times then pause

    while (1)
    {
        // Check connection status (only try to reconnect if we have a valid configured MAC and not user-initiated disconnect)
        if (!wandClient.isConnected() &&
            !wandClient.isUserDisconnectRequested() &&
            !wandClient.getNeedsInitialization() &&
            (mac_from_nvs || strcmp(WAND_MAC_ADDRESS, "C2:BD:5D:3C:67:4E") != 0))
        {
            // After 3 failed attempts, wait much longer to give WiFi priority
            if (reconnect_attempts >= MAX_RECONNECT_ATTEMPTS)
            {
                ESP_LOGW(TAG, "Connection lost after %d attempts. Pausing reconnects for 5 minutes to prioritize WiFi...", MAX_RECONNECT_ATTEMPTS);
                vTaskDelay(300000 / portTICK_PERIOD_MS); // Wait 5 minutes
                reconnect_attempts = 0;                  // Reset counter
            }

            ESP_LOGW(TAG, "Connection lost, attempting reconnect... (attempt %d/%d)", reconnect_attempts + 1, MAX_RECONNECT_ATTEMPTS);

            // Wait 30 seconds before reconnect attempt, but check periodically if user connected via web
            for (int i = 0; i < 300; i++) // 300 * 100ms = 30 seconds
            {
                vTaskDelay(100 / portTICK_PERIOD_MS);
                // If user connected via web during wait, abort auto-reconnect
                if (wandClient.isConnected() || wandClient.getNeedsInitialization())
                {
                    ESP_LOGI(TAG, "Web connection detected, aborting auto-reconnect");
                    reconnect_attempts = 0; // Reset counter
                    break;
                }
            }

            // Only attempt reconnect if still disconnected
            if (!wandClient.isConnected())
            {
                // Reload MAC from NVS in case it changed via web interface
                char reconnect_mac[18] = {0};
                nvs_handle_t nvs_reconnect;
                if (nvs_open("storage", NVS_READONLY, &nvs_reconnect) == ESP_OK)
                {
                    size_t mac_len = sizeof(reconnect_mac);
                    if (nvs_get_str(nvs_reconnect, "wand_mac", reconnect_mac, &mac_len) != ESP_OK || reconnect_mac[0] == '\0')
                    {
                        // Fall back to original wand_mac if NVS read fails
                        strncpy(reconnect_mac, wand_mac, sizeof(reconnect_mac) - 1);
                    }
                    nvs_close(nvs_reconnect);
                }
                else
                {
                    strncpy(reconnect_mac, wand_mac, sizeof(reconnect_mac) - 1);
                }

                // Attempt to connect with current MAC
                wandClient.connect(reconnect_mac);
                reconnect_attempts++;

                // Wait for connection to establish
                ESP_LOGI(TAG, "Waiting for connection...");
                vTaskDelay(10000 / portTICK_PERIOD_MS); // Wait 10 seconds for connection

                // Check if connection succeeded
                if (wandClient.isConnected())
                {
                    reconnect_attempts = 0; // Reset on successful connection
                    ESP_LOGI(TAG, "Reconnected! Waiting for service discovery...");
                    vTaskDelay(5000 / portTICK_PERIOD_MS);

                    // Re-initialize button thresholds on reconnect
                    if (!wandClient.initButtonThresholds())
                    {
                        ESP_LOGW(TAG, "WARNING: Failed to initialize button thresholds after reconnect");
                    }

                    // Request wand information
                    ESP_LOGI(TAG, "Requesting wand information...");
                    if (!wandClient.requestWandInfo())
                    {
                        ESP_LOGW(TAG, "WARNING: Failed to request wand information");
                    }
                    else
                    {
                        // Wait for info to be retrieved
                        vTaskDelay(300 / portTICK_PERIOD_MS);

                        // Publish wand info to Home Assistant
#if ENABLE_HOME_ASSISTANT
                        if (mqttClient.isConnected())
                        {
                            ESP_LOGI(TAG, "Publishing wand info to Home Assistant...");
                            mqttClient.publishWandInfo(
                                wandClient.getFirmwareVersion(),
                                wandClient.getSerialNumber(),
                                wandClient.getSKU(),
                                wandClient.getDeviceId(),
                                wandClient.getWandType(),
                                wandClient.getWandMacAddress());
                        }
#endif
                    }

                    // Wait before starting IMU streaming
                    vTaskDelay(500 / portTICK_PERIOD_MS);

                    if (wandClient.startIMUStreaming())
                    {
                        ESP_LOGI(TAG, "IMU streaming restarted");
                    }

                    // Clear needsInitialization flag if it was set (web-initiated connection that got auto-reconnected)
                    wandClient.setNeedsInitialization(false);
                    battery_check_counter = 0; // Reset battery check on reconnect
                    keepalive_counter = 0;     // Reset keep-alive on reconnect
                }
                else
                {
                    ESP_LOGW(TAG, "Reconnection failed, will retry...");
                }
            }
        }
        else
        {
            // Connected - check if initialization is needed (after web interface connection)
            if (wandClient.getNeedsInitialization())
            {
                ESP_LOGI(TAG, "Wand connected via web interface - running initialization...");
                vTaskDelay(3000 / portTICK_PERIOD_MS); // Wait for service discovery

                // Initialize button thresholds
                if (!wandClient.initButtonThresholds())
                {
                    ESP_LOGW(TAG, "WARNING: Failed to initialize button thresholds");
                }

                // Request wand information
                if (!wandClient.requestWandInfo())
                {
                    ESP_LOGW(TAG, "WARNING: Failed to request wand information");
                }

                // Wait before starting IMU streaming
                vTaskDelay(500 / portTICK_PERIOD_MS);

                if (!wandClient.startIMUStreaming())
                {
                    ESP_LOGW(TAG, "WARNING: Failed to start IMU streaming");
                }
                else
                {
                    ESP_LOGI(TAG, "\u2713 Wand initialized successfully");
                }

                // Clear the flag
                wandClient.setNeedsInitialization(false);
                battery_check_counter = 0;
                keepalive_counter = 0;
            }

            // Send keep-alive to prevent connection timeout
            keepalive_counter++;
            if (keepalive_counter >= KEEPALIVE_INTERVAL)
            {
                if (!wandClient.sendKeepAlive())
                {
                    ESP_LOGW(TAG, "Keep-alive failed to send");
                }
                keepalive_counter = 0;
            }

            // Periodically read battery level
            battery_check_counter++;
            if (battery_check_counter >= BATTERY_CHECK_INTERVAL)
            {
                uint8_t battery = wandClient.getBatteryLevel();
                ESP_LOGI(TAG, "🔋 Battery check: level=%d%%", battery);
                if (battery > 0)
                {
#if ENABLE_HOME_ASSISTANT
                    ESP_LOGI(TAG, "  → Broadcasting battery to web clients");
                    webServer.broadcastBattery(battery);

                    // Publish to Home Assistant (only if connected)
                    ESP_LOGI(TAG, "  → Checking MQTT connection (isConnected=%d)", mqttClient.isConnected());
                    if (mqttClient.isConnected())
                    {
                        ESP_LOGI(TAG, "  → Calling mqttClient.publishBattery()");
                        mqttClient.publishBattery(battery);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "  ⚠ MQTT not connected - skipping battery publish");
                    }
#endif
                }
                battery_check_counter = 0;
            }
        }

        // Main loop - BLE events are handled in callbacks
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}
