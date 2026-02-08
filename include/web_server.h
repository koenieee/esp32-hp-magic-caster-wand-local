#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Forward declaration
class WandBLEClient;

class WebServer
{
public:
    WebServer();
    ~WebServer();

    bool begin(uint16_t port = 80);
    void setWandClient(WandBLEClient *client);
    void stop();

    // Broadcast IMU data to all WebSocket clients
    void broadcastIMU(float ax, float ay, float az, float gx, float gy, float gz);

    // Broadcast spell detection to all WebSocket clients
    void broadcastSpell(const char *spell_name, float confidence);

    // Broadcast low confidence prediction to all WebSocket clients
    void broadcastLowConfidence(const char *spell_name, float confidence);

    // Broadcast battery level to all WebSocket clients
    void broadcastBattery(uint8_t level);

    // Broadcast wand connection status to all WebSocket clients
    void broadcastWandStatus(bool connected);

    // Broadcast wand information
    void broadcastWandInfo(const char *firmware_version, const char *serial_number,
                           const char *sku, const char *device_id, const char *wand_type);

    // Broadcast button state
    void broadcastButtonPress(bool b1, bool b2, bool b3, bool b4);

    // Broadcast gesture tracking events
    void broadcastGestureStart();
    void broadcastGesturePoint(float x, float y);
    void broadcastGestureEnd();

    // BLE scan results broadcasting
    void broadcastScanResult(const char *address, const char *name, int rssi);
    void broadcastScanComplete();

private:
    httpd_handle_t server;
    bool running;

    // HTTP handlers
    static esp_err_t root_handler(httpd_req_t *req);
    static esp_err_t ws_handler(httpd_req_t *req);   // WebSocket handler
    static esp_err_t data_handler(httpd_req_t *req); // Polling endpoint
    static esp_err_t captive_portal_handler(httpd_req_t *req);
    static esp_err_t scan_handler(httpd_req_t *req);                                // Start BLE scan
    static esp_err_t set_mac_handler(httpd_req_t *req);                             // Set wand MAC address
    static esp_err_t get_stored_mac_handler(httpd_req_t *req);                      // Get stored MAC address
    static esp_err_t connect_handler(httpd_req_t *req);                             // Connect to stored wand
    static esp_err_t disconnect_handler(httpd_req_t *req);                          // Disconnect from wand
    static esp_err_t settings_get_handler(httpd_req_t *req);                        // Get USB HID settings
    static esp_err_t settings_save_handler(httpd_req_t *req);                       // Save USB HID settings
    static esp_err_t settings_reset_handler(httpd_req_t *req);                      // Reset USB HID settings
    static esp_err_t wifi_scan_handler(httpd_req_t *req);                           // Scan WiFi networks
    static esp_err_t wifi_connect_handler(httpd_req_t *req);                        // Connect to WiFi
    static esp_err_t hotspot_settings_handler(httpd_req_t *req);                    // Save hotspot settings
    static esp_err_t hotspot_get_handler(httpd_req_t *req);                         // Get hotspot settings
    static esp_err_t system_reboot_handler(httpd_req_t *req);                       // Reboot device
    static esp_err_t system_wifi_mode_handler(httpd_req_t *req);                    // Switch WiFi mode (client/AP)
    static esp_err_t system_reset_nvs_handler(httpd_req_t *req);                    // Factory reset (clear NVS)
    static esp_err_t gesture_404_handler(httpd_req_t *req, httpd_err_code_t error); // Intercept 404s for gesture images
    static esp_err_t gesture_image_handler(httpd_req_t *req);                       // Serve gesture images from SPIFFS

    void addWebSocketClient(int fd);
    void removeWebSocketClient(int fd);

    // WebSocket client tracking
    int ws_clients[10];
    int ws_client_count;
    SemaphoreHandle_t client_mutex;

    // Cached data for polling
    struct
    {
        float ax, ay, az, gx, gy, gz;
        char spell[64];
        float confidence;
        uint8_t battery;
        bool has_spell;
        bool wand_connected;
        uint32_t timestamp;
        char firmware_version[32];
        char serial_number[32];
        char sku[32];
        char device_id[32];
        char wand_type[32];
    } cached_data;
    SemaphoreHandle_t data_mutex;
};
