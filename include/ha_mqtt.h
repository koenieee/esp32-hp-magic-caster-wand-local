#ifndef HA_MQTT_H
#define HA_MQTT_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_event.h"

// Home Assistant MQTT Client
class HAMqttClient
{
private:
    void *mqtt_client; // esp_mqtt_client_handle_t
    bool connected;

    // MQTT event handler
    static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

public:
    HAMqttClient();
    ~HAMqttClient();

    // Initialize MQTT client and connect to broker
    bool begin(const char *broker_uri, const char *username, const char *password);

    // Stop MQTT client
    void stop();

    // Publish spell detection to Home Assistant
    bool publishSpell(const char *spell_name, float confidence);

    // Publish battery level to Home Assistant
    bool publishBattery(uint8_t level);

    // Check if connected to MQTT broker
    bool isConnected() const { return connected; }
};

#endif // HA_MQTT_H
