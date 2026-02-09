#include "ha_mqtt.h"
#include "config.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_netif.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include <string.h>
#include <stdio.h>

#define FIRMWARE_VERSION "1.0.0"

static const char *TAG = "ha_mqtt";

HAMqttClient::HAMqttClient()
    : mqtt_client(nullptr), connected(false), on_connected_callback(nullptr)
{
}

HAMqttClient::~HAMqttClient()
{
    stop();
}

void HAMqttClient::mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    HAMqttClient *client = (HAMqttClient *)handler_args;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "✓ Connected to MQTT broker");
        client->connected = true;

        // Publish Home Assistant MQTT discovery configuration
        {
            // Get unique chip ID for this ESP32
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            char chip_id[16];
            snprintf(chip_id, sizeof(chip_id), "%02X%02X%02X%02X%02X%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

            // Store chip_id in class for later use
            strncpy(client->chip_id, chip_id, sizeof(client->chip_id) - 1);
            client->chip_id[sizeof(client->chip_id) - 1] = '\0';

            // Get ESP32 IP address
            char ip_str[16] = "unknown";
            esp_netif_ip_info_t ip_info;
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
            {
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            }

            // Build ESP-IDF version string
            char idf_version[32];
            snprintf(idf_version, sizeof(idf_version), "v%d.%d.%d",
                     ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH);

            ESP_LOGI(TAG, "Device Chip ID: %s", chip_id);
            ESP_LOGI(TAG, "Device IP: %s", ip_str);
            ESP_LOGI(TAG, "Firmware: %s (ESP-IDF %s)", FIRMWARE_VERSION, idf_version);

            // Reusable buffer for all discovery payloads (reduce stack usage)
            char *discovery_buffer = (char *)malloc(1200);
            if (!discovery_buffer)
            {
                ESP_LOGE(TAG, "Failed to allocate memory for discovery messages");
                break;
            }

            // Common device info JSON block (used by all sensors)
            char device_info[400];
            snprintf(device_info, sizeof(device_info),
                     "\"device\":{"
                     "\"identifiers\":[\"wand_%s\"],"
                     "\"name\":\"Wand Gateway %s\","
                     "\"manufacturer\":\"DIY\","
                     "\"model\":\"ESP32-S3\","
                     "\"sw_version\":\"%s\","
                     "\"hw_version\":\"ESP-IDF %s\","
                     "\"configuration_url\":\"http://%s\","
                     "\"connections\":[[\"mac\",\"%s\"]]"
                     "}",
                     chip_id, chip_id, FIRMWARE_VERSION, idf_version, ip_str, chip_id);

            // Spell sensor discovery - tracks last detected spell
            char discovery_topic[128];
            snprintf(discovery_topic, sizeof(discovery_topic), "homeassistant/sensor/wand_%s_spell/config", chip_id);
            snprintf(discovery_buffer, 1200,
                     "{"
                     "\"name\":\"Last Spell Cast\","
                     "\"unique_id\":\"wand_%s_spell\","
                     "\"object_id\":\"wand_%s_spell\","
                     "\"state_topic\":\"wand/%s/spell\","
                     "\"value_template\":\"{{ value_json.spell }}\","
                     "\"json_attributes_topic\":\"wand/%s/spell\","
                     "\"icon\":\"mdi:magic-staff\","
                     "%s"
                     "}",
                     chip_id, chip_id, chip_id, chip_id, device_info);

            ESP_LOGI(TAG, "📤 Publishing discovery to: %s", discovery_topic);
            ESP_LOGD(TAG, "📤 Discovery payload: %s", discovery_buffer);
            int msg_id1 = esp_mqtt_client_publish((esp_mqtt_client_handle_t)client->mqtt_client,
                                                  discovery_topic, discovery_buffer, 0, 1, true);
            ESP_LOGI(TAG, "   Spell discovery msg_id: %d", msg_id1);

            // Battery sensor discovery
            snprintf(discovery_topic, sizeof(discovery_topic), "homeassistant/sensor/wand_%s_battery/config", chip_id);
            snprintf(discovery_buffer, 1200,
                     "{"
                     "\"name\":\"Wand Battery\","
                     "\"unique_id\":\"wand_%s_battery\","
                     "\"object_id\":\"wand_%s_battery\","
                     "\"state_topic\":\"wand/%s/battery\","
                     "\"unit_of_measurement\":\"%%\","
                     "\"device_class\":\"battery\","
                     "\"state_class\":\"measurement\","
                     "\"value_template\":\"{{ value_json.level }}\","
                     "\"icon\":\"mdi:battery\","
                     "%s"
                     "}",
                     chip_id, chip_id, chip_id, device_info);

            ESP_LOGI(TAG, "📤 Publishing discovery to: %s", discovery_topic);
            ESP_LOGD(TAG, "📤 Discovery payload: %s", discovery_buffer);
            int msg_id2 = esp_mqtt_client_publish((esp_mqtt_client_handle_t)client->mqtt_client,
                                                  discovery_topic, discovery_buffer, 0, 1, true);
            ESP_LOGI(TAG, "   Battery discovery msg_id: %d", msg_id2);

            // Spell confidence sensor discovery
            snprintf(discovery_topic, sizeof(discovery_topic), "homeassistant/sensor/wand_%s_confidence/config", chip_id);
            snprintf(discovery_buffer, 1200,
                     "{"
                     "\"name\":\"Spell Confidence\","
                     "\"unique_id\":\"wand_%s_confidence\","
                     "\"object_id\":\"wand_%s_confidence\","
                     "\"state_topic\":\"wand/%s/spell\","
                     "\"unit_of_measurement\":\"%%\","
                     "\"value_template\":\"{{ (value_json.confidence * 100) | round(1) }}\","
                     "\"icon\":\"mdi:gauge\","
                     "\"state_class\":\"measurement\","
                     "%s"
                     "}",
                     chip_id, chip_id, chip_id, device_info);

            ESP_LOGI(TAG, "📤 Publishing discovery to: %s", discovery_topic);
            ESP_LOGD(TAG, "📤 Discovery payload: %s", discovery_buffer);
            int msg_id3 = esp_mqtt_client_publish((esp_mqtt_client_handle_t)client->mqtt_client,
                                                  discovery_topic, discovery_buffer, 0, 1, true);
            ESP_LOGI(TAG, "   Confidence discovery msg_id: %d", msg_id3);

            // Wand connection status sensor
            snprintf(discovery_topic, sizeof(discovery_topic), "homeassistant/binary_sensor/wand_%s_connected/config", chip_id);
            snprintf(discovery_buffer, 1200,
                     "{"
                     "\"name\":\"Wand Connected\","
                     "\"unique_id\":\"wand_%s_connected\","
                     "\"object_id\":\"wand_%s_connected\","
                     "\"state_topic\":\"wand/%s/info\","
                     "\"value_template\":\"{{ value_json.connected }}\","
                     "\"payload_on\":\"True\","
                     "\"payload_off\":\"False\","
                     "\"device_class\":\"connectivity\","
                     "\"icon\":\"mdi:magic-staff\","
                     "%s"
                     "}",
                     chip_id, chip_id, chip_id, device_info);

            ESP_LOGI(TAG, "📤 Publishing discovery to: %s", discovery_topic);
            int msg_id4 = esp_mqtt_client_publish((esp_mqtt_client_handle_t)client->mqtt_client,
                                                  discovery_topic, discovery_buffer, 0, 1, true);
            ESP_LOGI(TAG, "   Wand status discovery msg_id: %d", msg_id4);

            // Wand firmware version sensor
            snprintf(discovery_topic, sizeof(discovery_topic), "homeassistant/sensor/wand_%s_firmware/config", chip_id);
            snprintf(discovery_buffer, 1200,
                     "{"
                     "\"name\":\"Wand Firmware\","
                     "\"unique_id\":\"wand_%s_firmware\","
                     "\"object_id\":\"wand_%s_firmware\","
                     "\"state_topic\":\"wand/%s/info\","
                     "\"value_template\":\"{{ value_json.firmware }}\","
                     "\"icon\":\"mdi:chip\","
                     "\"entity_category\":\"diagnostic\","
                     "%s"
                     "}",
                     chip_id, chip_id, chip_id, device_info);

            ESP_LOGI(TAG, "📤 Publishing discovery to: %s", discovery_topic);
            int msg_id5 = esp_mqtt_client_publish((esp_mqtt_client_handle_t)client->mqtt_client,
                                                  discovery_topic, discovery_buffer, 0, 1, true);
            ESP_LOGI(TAG, "   Wand firmware discovery msg_id: %d", msg_id5);

            // Wand serial number sensor
            snprintf(discovery_topic, sizeof(discovery_topic), "homeassistant/sensor/wand_%s_serial/config", chip_id);
            snprintf(discovery_buffer, 1200,
                     "{"
                     "\"name\":\"Wand Serial Number\","
                     "\"unique_id\":\"wand_%s_serial\","
                     "\"object_id\":\"wand_%s_serial\","
                     "\"state_topic\":\"wand/%s/info\","
                     "\"value_template\":\"{{ value_json.serial }}\","
                     "\"icon\":\"mdi:identifier\","
                     "\"entity_category\":\"diagnostic\","
                     "%s"
                     "}",
                     chip_id, chip_id, chip_id, device_info);

            ESP_LOGI(TAG, "📤 Publishing discovery to: %s", discovery_topic);
            int msg_id6 = esp_mqtt_client_publish((esp_mqtt_client_handle_t)client->mqtt_client,
                                                  discovery_topic, discovery_buffer, 0, 1, true);
            ESP_LOGI(TAG, "   Wand serial discovery msg_id: %d", msg_id6);

            // Wand MAC address sensor
            snprintf(discovery_topic, sizeof(discovery_topic), "homeassistant/sensor/wand_%s_mac/config", chip_id);
            snprintf(discovery_buffer, 1200,
                     "{"
                     "\"name\":\"Wand MAC Address\","
                     "\"unique_id\":\"wand_%s_mac\","
                     "\"object_id\":\"wand_%s_mac\","
                     "\"state_topic\":\"wand/%s/info\","
                     "\"value_template\":\"{{ value_json.wand_mac }}\","
                     "\"icon\":\"mdi:bluetooth\","
                     "\"entity_category\":\"diagnostic\","
                     "%s"
                     "}",
                     chip_id, chip_id, chip_id, device_info);

            ESP_LOGI(TAG, "📤 Publishing discovery to: %s", discovery_topic);
            int msg_id7 = esp_mqtt_client_publish((esp_mqtt_client_handle_t)client->mqtt_client,
                                                  discovery_topic, discovery_buffer, 0, 1, true);
            ESP_LOGI(TAG, "   Wand MAC discovery msg_id: %d", msg_id7);

            // Free the discovery buffer
            free(discovery_buffer);

            ESP_LOGI(TAG, "✓ Published Home Assistant discovery config (7 sensors + 1 binary_sensor)");

            // Publish initial state for wand info (no wand connected yet)
            char wand_info_topic[64];
            snprintf(wand_info_topic, sizeof(wand_info_topic), "wand/%s/info", chip_id);
            const char *initial_wand_info = "{\"firmware\":\"unknown\",\"serial\":\"unknown\",\"sku\":\"unknown\",\"device_id\":\"unknown\",\"wand_type\":\"unknown\",\"wand_mac\":\"unknown\",\"connected\":false}";
            int init_msg_id = esp_mqtt_client_publish((esp_mqtt_client_handle_t)client->mqtt_client,
                                                      wand_info_topic,
                                                      initial_wand_info,
                                                      strlen(initial_wand_info),
                                                      1,
                                                      true); // retain=true
            ESP_LOGI(TAG, "📤 Published initial wand state (no wand connected) [msg_id=%d]", init_msg_id);

            // Publish initial battery state (unavailable until wand connects)
            char battery_topic[64];
            snprintf(battery_topic, sizeof(battery_topic), "wand/%s/battery", chip_id);
            const char *initial_battery = "{\"level\":0}";
            int battery_msg_id = esp_mqtt_client_publish((esp_mqtt_client_handle_t)client->mqtt_client,
                                                         battery_topic,
                                                         initial_battery,
                                                         strlen(initial_battery),
                                                         1,
                                                         true); // retain=true
            ESP_LOGI(TAG, "📤 Published initial battery state [msg_id=%d]", battery_msg_id);

            // Publish initial spell state (no spell detected yet)
            char spell_topic[64];
            snprintf(spell_topic, sizeof(spell_topic), "wand/%s/spell", chip_id);
            const char *initial_spell = "{\"spell\":\"No spell yet\",\"confidence\":0.0}";
            int spell_msg_id = esp_mqtt_client_publish((esp_mqtt_client_handle_t)client->mqtt_client,
                                                       spell_topic,
                                                       initial_spell,
                                                       strlen(initial_spell),
                                                       1,
                                                       true); // retain=true
            ESP_LOGI(TAG, "📤 Published initial spell state [msg_id=%d]", spell_msg_id);

            // Subscribe to a test topic to verify bidirectional MQTT connectivity
            int sub_id = esp_mqtt_client_subscribe((esp_mqtt_client_handle_t)client->mqtt_client,
                                                   "wand/test", 0);
            ESP_LOGI(TAG, "📥 Subscribed to wand/test for connectivity verification [msg_id=%d]", sub_id);

            // Call onConnected callback if registered
            if (client->on_connected_callback)
            {
                ESP_LOGI(TAG, "Calling MQTT connected callback...");
                client->on_connected_callback();
            }
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from MQTT broker");
        client->connected = false;
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "✓ MQTT message published successfully [msg_id=%d]", event->msg_id);
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "✓ MQTT subscription successful [msg_id=%d]", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "📥 MQTT data received on topic: %.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "   Payload: %.*s", event->data_len, event->data);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error occurred");
        if (event->error_handle && event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            ESP_LOGE(TAG, "TCP transport error: %d", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGE(TAG, "→ Check MQTT broker address/port and network connectivity");
            ESP_LOGE(TAG, "→ Disable MQTT via web GUI if not using Home Assistant");
        }
        break;

    default:
        ESP_LOGD(TAG, "MQTT event: %d", event_id);
        break;
    }
}

bool HAMqttClient::begin(const char *broker_uri, const char *username, const char *password)
{
    if (mqtt_client)
    {
        ESP_LOGW(TAG, "MQTT client already initialized");
        return true;
    }

    ESP_LOGI(TAG, "Initializing MQTT client...");
    ESP_LOGI(TAG, "Broker: %s", broker_uri);
    ESP_LOGI(TAG, "Username: %s", username && strlen(username) > 0 ? username : "(none)");

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = broker_uri;
    mqtt_cfg.credentials.username = username;
    mqtt_cfg.credentials.authentication.password = password;
    mqtt_cfg.session.keepalive = 60;
    mqtt_cfg.network.timeout_ms = 5000;             // Shorter connection timeout: 5s
    mqtt_cfg.network.reconnect_timeout_ms = 30000;  // Longer reconnect interval: 30s
    mqtt_cfg.network.disable_auto_reconnect = true; // Disable aggressive reconnection

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!mqtt_client)
    {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return false;
    }

    esp_mqtt_client_register_event((esp_mqtt_client_handle_t)mqtt_client,
                                   MQTT_EVENT_ANY,
                                   mqtt_event_handler,
                                   this);

    esp_err_t err = esp_mqtt_client_start((esp_mqtt_client_handle_t)mqtt_client);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "MQTT client started");
    return true;
}

void HAMqttClient::stop()
{
    if (mqtt_client)
    {
        esp_mqtt_client_stop((esp_mqtt_client_handle_t)mqtt_client);
        esp_mqtt_client_destroy((esp_mqtt_client_handle_t)mqtt_client);
        mqtt_client = nullptr;
        connected = false;
    }
}

bool HAMqttClient::publishSpell(const char *spell_name, float confidence)
{
    ESP_LOGI(TAG, "publishSpell() called: spell_name='%s', confidence=%.3f",
             spell_name ? spell_name : "(null)", confidence);
    ESP_LOGI(TAG, "  Connection status: connected=%d, mqtt_client=%p",
             connected, mqtt_client);

    if (!connected)
    {
        ESP_LOGW(TAG, "  ❌ Cannot publish: Not connected to MQTT broker");
        return false;
    }

    if (!mqtt_client)
    {
        ESP_LOGW(TAG, "  ❌ Cannot publish: MQTT client is NULL");
        return false;
    }

    if (!spell_name)
    {
        ESP_LOGW(TAG, "  ❌ Cannot publish: spell_name is NULL");
        return false;
    }

    // Publish JSON payload with spell name and confidence
    char json[256];
    snprintf(json, sizeof(json),
             "{\"spell\":\"%s\",\"confidence\":%.3f}",
             spell_name, confidence);

    char topic[64];
    snprintf(topic, sizeof(topic), "wand/%s/spell", chip_id);

    ESP_LOGI(TAG, "  📤 Publishing to topic '%s'", topic);
    ESP_LOGI(TAG, "  📤 Payload: %s", json);
    ESP_LOGI(TAG, "  📤 QoS: 1, Retain: false");

    int msg_id = esp_mqtt_client_publish((esp_mqtt_client_handle_t)mqtt_client,
                                         topic,
                                         json,
                                         strlen(json), // Explicit length instead of 0
                                         1,            // QoS 1
                                         false);       // retain

    if (msg_id >= 0)
    {
        ESP_LOGI(TAG, "  ✓ Published spell: %s (%.1f%%) [msg_id=%d]",
                 spell_name, confidence * 100.0f, msg_id);
        return true;
    }
    else
    {
        ESP_LOGE(TAG, "  ❌ Failed to publish spell [msg_id=%d]", msg_id);
        return false;
    }
}

bool HAMqttClient::publishBattery(uint8_t level)
{
    ESP_LOGI(TAG, "publishBattery() called: level=%d%%", level);
    ESP_LOGI(TAG, "  Connection status: connected=%d, mqtt_client=%p",
             connected, mqtt_client);

    if (!connected)
    {
        ESP_LOGW(TAG, "  ❌ Cannot publish: Not connected to MQTT broker");
        return false;
    }

    if (!mqtt_client)
    {
        ESP_LOGW(TAG, "  ❌ Cannot publish: MQTT client is NULL");
        return false;
    }

    char json[64];
    snprintf(json, sizeof(json), "{\"level\":%d}", level);

    char topic[64];
    snprintf(topic, sizeof(topic), "wand/%s/battery", chip_id);

    ESP_LOGI(TAG, "  📤 Publishing to topic '%s'", topic);
    ESP_LOGI(TAG, "  📤 Payload: %s", json);
    ESP_LOGI(TAG, "  📤 QoS: 1, Retain: false");

    int msg_id = esp_mqtt_client_publish((esp_mqtt_client_handle_t)mqtt_client,
                                         topic,
                                         json,
                                         0,
                                         1,
                                         false);

    if (msg_id >= 0)
    {
        ESP_LOGI(TAG, "  ✓ Published battery: %d%% [msg_id=%d]", level, msg_id);
        return true;
    }
    else
    {
        ESP_LOGE(TAG, "  ❌ Failed to publish battery [msg_id=%d]", msg_id);
        return false;
    }
}

bool HAMqttClient::publishWandInfo(const char *firmware_version, const char *serial_number,
                                   const char *sku, const char *device_id, const char *wand_type,
                                   const char *wand_mac)
{
    ESP_LOGI(TAG, "publishWandInfo() called");
    ESP_LOGI(TAG, "  Connection status: connected=%d, mqtt_client=%p",
             connected, mqtt_client);

    if (!connected || !mqtt_client)
    {
        ESP_LOGW(TAG, "  ❌ Cannot publish: Not connected to MQTT broker");
        return false;
    }

    // Build JSON with wand information
    // Check for empty strings, not just NULL
    const char *fw = (firmware_version && firmware_version[0]) ? firmware_version : "unknown";
    const char *sn = (serial_number && serial_number[0]) ? serial_number : "unknown";
    const char *sk = (sku && sku[0]) ? sku : "unknown";
    const char *did = (device_id && device_id[0]) ? device_id : "unknown";
    const char *wt = (wand_type && wand_type[0]) ? wand_type : "unknown";
    const char *mac = (wand_mac && wand_mac[0]) ? wand_mac : "unknown";

    char json[512];
    snprintf(json, sizeof(json),
             "{"
             "\"firmware\":\"%s\","
             "\"serial\":\"%s\","
             "\"sku\":\"%s\","
             "\"device_id\":\"%s\","
             "\"wand_type\":\"%s\","
             "\"wand_mac\":\"%s\","
             "\"connected\":true"
             "}",
             fw, sn, sk, did, wt, mac);

    char topic[64];
    snprintf(topic, sizeof(topic), "wand/%s/info", chip_id);

    ESP_LOGI(TAG, "  📤 Publishing to topic '%s'", topic);
    ESP_LOGI(TAG, "  📤 Wand FW: %s, Serial: %s, Type: %s, MAC: %s",
             fw, sn, wt, mac);
    ESP_LOGI(TAG, "  📤 Payload: %s", json);

    int msg_id = esp_mqtt_client_publish((esp_mqtt_client_handle_t)mqtt_client,
                                         topic,
                                         json,
                                         strlen(json),
                                         1,
                                         true); // retain=true so HA always has latest

    if (msg_id >= 0)
    {
        ESP_LOGI(TAG, "  ✓ Published wand info [msg_id=%d]", msg_id);
        return true;
    }
    else
    {
        ESP_LOGE(TAG, "  ❌ Failed to publish wand info [msg_id=%d]", msg_id);
        return false;
    }
}

bool HAMqttClient::publishWandDisconnected()
{
    ESP_LOGI(TAG, "publishWandDisconnected() called");

    if (!connected || !mqtt_client)
    {
        ESP_LOGW(TAG, "  ⚠ Not connected to MQTT broker (skipping)");
        return false;
    }

    // Publish empty wand info with connected=false
    const char *json = "{\"firmware\":\"unknown\",\"serial\":\"unknown\",\"sku\":\"unknown\",\"device_id\":\"unknown\",\"wand_type\":\"unknown\",\"wand_mac\":\"unknown\",\"connected\":false}";

    char topic[64];
    snprintf(topic, sizeof(topic), "wand/%s/info", chip_id);

    ESP_LOGI(TAG, "  📤 Publishing disconnection to topic '%s'", topic);

    int msg_id = esp_mqtt_client_publish((esp_mqtt_client_handle_t)mqtt_client,
                                         topic,
                                         json,
                                         strlen(json),
                                         1,
                                         true); // retain=true

    if (msg_id >= 0)
    {
        ESP_LOGI(TAG, "  ✓ Published wand disconnection [msg_id=%d]", msg_id);
        return true;
    }
    else
    {
        ESP_LOGE(TAG, "  ❌ Failed to publish wand disconnection [msg_id=%d]", msg_id);
        return false;
    }
}
