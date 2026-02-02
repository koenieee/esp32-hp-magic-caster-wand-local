#include "ha_mqtt.h"
#include "config.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ha_mqtt";

HAMqttClient::HAMqttClient()
    : mqtt_client(nullptr), connected(false)
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
            // Discovery topic: homeassistant/event/wand/config
            const char *discovery_topic = "homeassistant/event/wand_spell/config";
            char config_json[512];
            snprintf(config_json, sizeof(config_json),
                     "{\"name\":\"Magic Wand Spell\","
                     "\"state_topic\":\"wand/spell\","
                     "\"value_template\":\"{{ value_json.spell }}\","
                     "\"json_attributes_topic\":\"wand/spell\","
                     "\"device\":{\"identifiers\":[\"esp32_wand\"],"
                     "\"name\":\"Magic Wand Gateway\","
                     "\"manufacturer\":\"DIY\","
                     "\"model\":\"ESP32 Wand Gateway\"}}");

            esp_mqtt_client_publish((esp_mqtt_client_handle_t)client->mqtt_client,
                                    discovery_topic, config_json, 0, 1, true);

            // Battery sensor discovery
            const char *battery_discovery = "homeassistant/sensor/wand_battery/config";
            char battery_json[512];
            snprintf(battery_json, sizeof(battery_json),
                     "{\"name\":\"Wand Battery\","
                     "\"state_topic\":\"wand/battery\","
                     "\"unit_of_measurement\":\"%%\","
                     "\"device_class\":\"battery\","
                     "\"value_template\":\"{{ value_json.level }}\","
                     "\"device\":{\"identifiers\":[\"esp32_wand\"]}}");

            esp_mqtt_client_publish((esp_mqtt_client_handle_t)client->mqtt_client,
                                    battery_discovery, battery_json, 0, 1, true);

            ESP_LOGI(TAG, "Published Home Assistant discovery config");
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from MQTT broker");
        client->connected = false;
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error occurred");
        break;

    default:
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

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = broker_uri;
    mqtt_cfg.credentials.username = username;
    mqtt_cfg.credentials.authentication.password = password;
    mqtt_cfg.session.keepalive = 60;
    mqtt_cfg.network.reconnect_timeout_ms = 10000;
    mqtt_cfg.network.disable_auto_reconnect = false;

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
    if (!connected || !mqtt_client || !spell_name)
    {
        return false;
    }

    // Publish JSON payload with spell name and confidence
    char json[256];
    snprintf(json, sizeof(json),
             "{\"spell\":\"%s\",\"confidence\":%.3f}",
             spell_name, confidence);

    int msg_id = esp_mqtt_client_publish((esp_mqtt_client_handle_t)mqtt_client,
                                         MQTT_TOPIC_SPELL,
                                         json,
                                         0,      // length (0 = use strlen)
                                         1,      // QoS 1
                                         false); // retain

    if (msg_id >= 0)
    {
        ESP_LOGI(TAG, "Published spell: %s (%.1f%%) [msg_id=%d]",
                 spell_name, confidence * 100.0f, msg_id);
        return true;
    }
    else
    {
        ESP_LOGW(TAG, "Failed to publish spell");
        return false;
    }
}

bool HAMqttClient::publishBattery(uint8_t level)
{
    if (!connected || !mqtt_client)
    {
        return false;
    }

    char json[64];
    snprintf(json, sizeof(json), "{\"level\":%d}", level);

    int msg_id = esp_mqtt_client_publish((esp_mqtt_client_handle_t)mqtt_client,
                                         "wand/battery",
                                         json,
                                         0,
                                         1,
                                         false);

    if (msg_id >= 0)
    {
        ESP_LOGD(TAG, "Published battery: %d%% [msg_id=%d]", level, msg_id);
        return true;
    }

    return false;
}
