#include "web_server.h"
#include "config.h"
#include "esp_log.h"
#include "ble_client.h"
#include "usb_hid.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_spiffs.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dirent.h>

// Forward declaration from main.cpp
#if USE_USB_HID_DEVICE
extern USBHIDManager usbHID;
#endif

static const char *TAG = "web_server";

// Helper to sanitize strings for JSON output (removes non-printable and non-UTF-8 chars)
static void sanitize_for_json(char *dest, const char *src, size_t max_len)
{
    if (!src || !dest || max_len == 0)
    {
        if (dest && max_len > 0)
            dest[0] = '\0';
        return;
    }

    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j < max_len - 1; i++)
    {
        unsigned char c = src[i];
        // Allow printable ASCII and basic UTF-8 continuation bytes
        // Skip control characters, NULL, and invalid UTF-8
        if (c >= 32 && c < 127) // Printable ASCII
        {
            // Escape JSON special characters
            if (c == '"' || c == '\\' || c == '/')
            {
                if (j + 1 < max_len - 1)
                {
                    dest[j++] = '\\';
                    dest[j++] = c;
                }
            }
            else
            {
                dest[j++] = c;
            }
        }
        else if (c >= 128) // Potential UTF-8 multi-byte character
        {
            // Simple UTF-8 validation: just accept bytes >= 128 as-is
            // for proper UTF-8 sequences (conservative approach)
            dest[j++] = c;
        }
        // Skip control characters (0-31, 127)
    }
    dest[j] = '\0';
}

// Helper for broadcasting WebSocket messages with auto-disconnect detection
static void broadcast_to_clients(httpd_handle_t server, int *clients, int *count,
                                 SemaphoreHandle_t mutex, const char *data)
{
    if (!data || !clients || !count || !server)
    {
        ESP_LOGW(TAG, "broadcast_to_clients called with NULL parameters");
        return;
    }

    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.payload = (uint8_t *)data;
        ws_pkt.len = strlen(data);
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;

        for (int i = 0; i < *count; i++)
        {
            esp_err_t ret = httpd_ws_send_frame_async(server, clients[i], &ws_pkt);
            if (ret != ESP_OK)
            {
                ESP_LOGW(TAG, "WebSocket send failed for fd=%d (error=%s), removing client",
                         clients[i], esp_err_to_name(ret));
                for (int j = i; j < *count - 1; j++)
                {
                    clients[j] = clients[j + 1];
                }
                (*count)--;
                i--;
            }
        }
        xSemaphoreGive(mutex);
    }
}

// HTML page with IMU visualization
static const char index_html[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Magic Wand Gateway</title>
    <style>
        body { 
            font-family: Arial, sans-serif; 
            margin: 0; 
            padding: 20px; 
            background: #1a1a1a; 
            color: #fff; 
        }
        h1 { 
            text-align: center; 
            color: #4CAF50; 
        }
        .container { 
            max-width: 1200px; 
            margin: 0 auto; 
        }
        .status { 
            padding: 10px; 
            margin: 10px 0; 
            border-radius: 5px; 
            background: #333; 
        }
        .status.connected { 
            background: #2d5016; 
        }
        .battery-box {
            text-align: center;
            padding: 15px;
            margin: 10px 0;
            background: #333;
            border-radius: 5px;
            font-size: 1.5em;
        }
        .battery-level {
            color: #4CAF50;
            font-weight: bold;
        }
        .battery-low {
            color: #ff4444;
        }
        .spell-box { 
            font-size: 2em; 
            text-align: center; 
            padding: 20px; 
            margin: 20px 0; 
            background: #333; 
            border-radius: 10px; 
            min-height: 80px; 
        }
        .spell-name { 
            color: #FFD700; 
            font-weight: bold; 
        }
        canvas { 
            border: 2px solid #444; 
            border-radius: 5px; 
            background: #000; 
            display: block; 
            margin: 20px auto; 
        }
        .data-grid { 
            display: grid; 
            grid-template-columns: repeat(2, 1fr); 
            gap: 10px; 
            margin: 20px 0; 
        }
        .data-item { 
            background: #333; 
            padding: 15px; 
            border-radius: 5px; 
        }
        .data-label { 
            color: #888; 
            font-size: 0.9em; 
        }
        .data-value { 
            font-size: 1.5em; 
            font-weight: bold; 
            color: #4CAF50; 
        }
        .ble-controls {
            background: #333;
            padding: 20px;
            margin: 20px 0;
            border-radius: 5px;
        }
        .ble-controls h3 {
            margin-top: 0;
            color: #4CAF50;
        }
        .button {
            background: #4CAF50;
            color: white;
            border: none;
            padding: 12px 24px;
            margin: 5px;
            border-radius: 5px;
            cursor: pointer;
            font-size: 1em;
            transition: background 0.3s;
        }
        .button:hover {
            background: #45a049;
        }
        .button:disabled {
            background: #666;
            cursor: not-allowed;
        }
        .button.secondary {
            background: #666;
        }
        .button.secondary:hover {
            background: #555;
        }
        .button.danger {
            background: #d32f2f;
        }
        .button.danger:hover {
            background: #b71c1c;
        }
        .scan-results {
            margin-top: 15px;
            max-height: 300px;
            overflow-y: auto;
        }
        .scan-item {
            background: #222;
            padding: 10px;
            margin: 5px 0;
            border-radius: 3px;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        .scan-item:hover {
            background: #2a2a2a;
        }
        .scan-info {
            flex-grow: 1;
        }
        .mac-address {
            font-family: monospace;
            color: #4CAF50;
        }
        .rssi {
            color: #888;
            font-size: 0.9em;
        }
        .input-group {
            margin: 10px 0;
        }
        .input-group input {
            width: 250px;
            padding: 10px;
            border: 1px solid #555;
            background: #222;
            color: #fff;
            border-radius: 5px;
            font-family: monospace;
        }
        .settings-grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 20px;
        }
        .spell-mappings-container {
            background: #222;
            padding: 10px;
            border-radius: 5px;
            max-height: 400px;
            overflow-y: auto;
        }
        .spell-mappings-grid {
            display: grid;
            grid-template-columns: repeat(2, minmax(0, 1fr));
            gap: 8px;
        }
        .spell-mapping-item {
            display: flex;
            flex-direction: column;
            gap: 4px;
        }
        .spell-mapping-item select {
            width: 100%;
            padding: 10px;
            background: #333;
            color: #fff;
            border: 1px solid #555;
            border-radius: 5px;
            font-size: 14px;
        }
        .spell-mapping-search {
            width: 100%;
            padding: 10px;
            margin-bottom: 10px;
            border: 1px solid #555;
            background: #222;
            color: #fff;
            border-radius: 5px;
        }
        @media (max-width: 900px) {
            .settings-grid {
                grid-template-columns: 1fr;
            }
        }
        @media (max-width: 700px) {
            .spell-mappings-grid {
                grid-template-columns: 1fr;
            }
            .button {
                width: 100%;
            }
            .input-group input {
                width: 100%;
            }
        }
        /* Toast notification styles */
        .toast {
            position: fixed;
            bottom: 20px;
            right: 20px;
            background: #333;
            color: #fff;
            padding: 16px 24px;
            border-radius: 8px;
            box-shadow: 0 4px 12px rgba(0,0,0,0.5);
            display: flex;
            align-items: center;
            gap: 12px;
            font-size: 1em;
            z-index: 10000;
            animation: slideIn 0.3s ease-out, slideOut 0.3s ease-in 2.7s;
            opacity: 0;
        }
        .toast.success {
            background: #2d5016;
            border-left: 4px solid #4CAF50;
        }
        .toast.error {
            background: #5a1a1a;
            border-left: 4px solid #f44336;
        }
        @keyframes slideIn {
            from {
                transform: translateX(400px);
                opacity: 0;
            }
            to {
                transform: translateX(0);
                opacity: 1;
            }
        }
        @keyframes slideOut {
            from {
                transform: translateX(0);
                opacity: 1;
            }
            to {
                transform: translateX(400px);
                opacity: 0;
            }
        }
        /* Spell Learning Controls */
        .spell-learning-controls {
            background: #333;
            padding: 20px;
            margin: 20px 0;
            border-radius: 5px;
            display: flex;
            gap: 15px;
            align-items: center;
            flex-wrap: wrap;
        }
        .spell-learning-controls select {
            flex: 1;
            min-width: 250px;
            padding: 12px;
            background: #222;
            color: #fff;
            border: 1px solid #555;
            border-radius: 5px;
            font-size: 1em;
        }
        /* Desktop scaling - reduce everything by 30% for better overview */
        @media (min-width: 901px) {
            body {
                zoom: 0.7;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>🪄 Magic Wand Gateway</h1>
        
        <div id="status" class="status">
            WebSocket: <span id="status-text">Connecting...</span><br>
            Wand: <span id="wand-status">Unknown</span>
        </div>
        
        <div class="ble-controls">
            <h3>🔵 BLE Wand Management</h3>
            <div>
                <button class="button" id="scanBtn" onclick="startScan()">🔍 Scan for Wands</button>
                <button class="button secondary" id="connectBtn" onclick="connectWand()" disabled>🔗 Connect</button>
                <button class="button danger" id="disconnectBtn" onclick="disconnectWand()">✖ Disconnect</button>
            </div>
            <div class="input-group">
                <label>Stored MAC: </label>
                <input type="text" id="storedMac" placeholder="XX:XX:XX:XX:XX:XX" readonly>
                <button class="button secondary" onclick="loadStoredMac()">🔄 Refresh</button>
            </div>
            <div id="scanStatus" style="margin-top: 10px; color: #888;"></div>
            <div id="scanResults" class="scan-results"></div>
        </div>
        
        <div class="ble-controls">
            <h3>⚙️ Spell & Mouse Settings</h3>
            <div class="settings-grid">
                <div>
                    <h4 style="margin: 0 0 10px 0; color: #4CAF50;">Spell Mappings (Full Keyboard)</h4>
                    <input type="text" id="spell-filter" class="spell-mapping-search" placeholder="Filter spells..." oninput="filterSpellMappings()">
                    <div class="spell-mappings-container">
                        <div id="spell-mappings" class="spell-mappings-grid">
                            <!-- Spell mappings will be populated by JavaScript -->
                        </div>
                    </div>
                    <h4 style="margin: 20px 0 10px 0; color: #4CAF50;">Spell Mappings (Gamepad Buttons)</h4>
                    <input type="text" id="gamepad-spell-filter" class="spell-mapping-search" placeholder="Filter spells..." oninput="filterGamepadMappings()">
                    <div class="spell-mappings-container">
                        <div id="gamepad-mappings" class="spell-mappings-grid">
                            <!-- Gamepad mappings will be populated by JavaScript -->
                        </div>
                    </div>
                </div>
                <div>
                    <h4 style="margin: 0 0 10px 0; color: #4CAF50;">Mouse Settings</h4>
                    <div style="background: #222; padding: 10px; border-radius: 5px;">
                        <div style="margin: 10px 0;">
                            <label style="display: block; margin-bottom: 5px;">Mouse Sensitivity:</label>
                            <div style="display: flex; gap: 10px; align-items: center;">
                                <input type="range" id="mouse-sensitivity" min="0.1" max="5.0" step="0.1" value="1.0" style="flex-grow: 1;">
                                <span id="sens-value" style="width: 40px; text-align: right;">1.0x</span>
                            </div>
                            <div style="font-size: 0.8em; color: #888; margin-top: 5px;">Lower = less movement, Higher = more movement</div>
                        </div>
                        <div style="margin: 10px 0;">
                            <label style="display: flex; align-items: center; gap: 8px; cursor: pointer;">
                                <input type="checkbox" id="invert-mouse-y" style="width: 18px; height: 18px;">
                                <span>Invert Y-Axis (wand up = cursor up)</span>
                            </label>
                            <div style="font-size: 0.8em; color: #888; margin-top: 5px;">Checked = inverted (typical), Unchecked = natural</div>
                        </div>
                        <div style="margin: 10px 0; border-top: 1px solid #444; padding-top: 10px;">
                            <label style="display: block; margin-bottom: 5px;">HID Mode:</label>
                            <select id="hid-mode" style="width: 100%; padding: 8px; border-radius: 4px; background: #111; color: #eee; border: 1px solid #444;">
                                <option value="0">Mouse</option>
                                <option value="1">Keyboard</option>
                                <option value="2">Gamepad</option>
                                <option value="3">Disabled</option>
                            </select>
                            <div style="font-size: 0.8em; color: #888; margin-top: 5px;">Only one mode can be active at a time</div>
                        </div>
                        <div style="margin: 10px 0; border-top: 1px solid #444; padding-top: 10px;">
                            <label style="display: block; margin-bottom: 5px;">Gamepad Sensitivity:</label>
                            <div style="display: flex; gap: 10px; align-items: center;">
                                <input type="range" id="gamepad-sensitivity" min="0.1" max="5.0" step="0.1" value="1.0" style="flex-grow: 1;">
                                <span id="gpad-sens-value" style="width: 40px; text-align: right;">1.0x</span>
                            </div>
                        </div>
                        <div style="margin: 10px 0;">
                            <label style="display: block; margin-bottom: 5px;">Gamepad Dead Zone:</label>
                            <div style="display: flex; gap: 10px; align-items: center;">
                                <input type="range" id="gamepad-deadzone" min="0.0" max="0.5" step="0.01" value="0.05" style="flex-grow: 1;">
                                <span id="gpad-deadzone-value" style="width: 50px; text-align: right;">0.05</span>
                            </div>
                        </div>
                        <div style="margin: 10px 0;">
                            <label style="display: flex; align-items: center; gap: 8px; cursor: pointer;">
                                <input type="checkbox" id="invert-gamepad-y" style="width: 18px; height: 18px;">
                                <span>Invert Gamepad Y-Axis</span>
                            </label>
                        </div>
                    </div>
                    <div style="background: #222; padding: 10px; border-radius: 5px; margin-top: 10px;">
                        <h4 style="margin: 0 0 10px 0; color: #4CAF50;">Home Assistant MQTT Settings</h4>
                        <div style="margin: 10px 0;">
                            <label style="display: flex; align-items: center; gap: 8px; cursor: pointer;">
                                <input type="checkbox" id="ha-mqtt-enabled" style="width: 18px; height: 18px;">
                                <span>Enable MQTT</span>
                            </label>
                        </div>
                        <div style="margin: 10px 0;">
                            <label style="display: block; margin-bottom: 5px;">MQTT Broker URI:</label>
                            <input type="text" id="mqtt-broker" placeholder="mqtt://192.168.1.100:1883" style="width: 100%; padding: 8px; border-radius: 4px; background: #111; color: #eee; border: 1px solid #444;">
                        </div>
                        <div style="margin: 10px 0;">
                            <label style="display: block; margin-bottom: 5px;">MQTT Username:</label>
                            <input type="text" id="mqtt-username" placeholder="homeassistant" style="width: 100%; padding: 8px; border-radius: 4px; background: #111; color: #eee; border: 1px solid #444;">
                        </div>
                        <div style="margin: 10px 0;">
                            <label style="display: block; margin-bottom: 5px;">MQTT Password:</label>
                            <input type="password" id="mqtt-password" placeholder="password" style="width: 100%; padding: 8px; border-radius: 4px; background: #111; color: #eee; border: 1px solid #444;">
                        </div>
                        <div style="font-size: 0.8em; color: #888; margin-top: 5px;">Restart required after changing MQTT settings</div>
                    </div>
                </div>
            </div>
            <div style="margin-top: 15px;">
                <button class="button" onclick="saveSettings()">💾 Save Settings</button>
                <button class="button secondary" onclick="loadSettings()">🔄 Load Settings</button>
                <button class="button danger" onclick="resetSettings()">🔁 Reset to Defaults</button>
            </div>
        </div>
        
        <div class="ble-controls">
            <h3>📡 WiFi & Network Settings</h3>
            <div style="background: #222; padding: 15px; border-radius: 5px; margin-bottom: 10px;">
                <h4 style="margin: 0 0 10px 0; color: #4CAF50;">WiFi Client Mode</h4>
                <div style="margin: 10px 0;">
                    <button class="button" onclick="scanWifi()">🔍 Scan WiFi Networks</button>
                    <div id="wifiScanStatus" style="margin-top: 10px; color: #888;"></div>
                    <div id="wifiResults" class="scan-results" style="max-height: 200px;"></div>
                </div>
                <div style="margin: 10px 0;">
                    <label style="display: block; margin-bottom: 5px;">WiFi SSID:</label>
                    <input type="text" id="wifi-ssid" placeholder="Your WiFi Network" style="width: 100%; padding: 8px; border-radius: 4px; background: #111; color: #eee; border: 1px solid #444;">
                </div>
                <div style="margin: 10px 0;">
                    <label style="display: block; margin-bottom: 5px;">WiFi Password:</label>
                    <input type="password" id="wifi-password" placeholder="WiFi Password" style="width: 100%; padding: 8px; border-radius: 4px; background: #111; color: #eee; border: 1px solid #444;">
                </div>
                <button class="button" onclick="connectWifi()">🌐 Connect to WiFi</button>
                <div id="wifiConnectStatus" style="margin-top: 10px; color: #888;"></div>
            </div>
            <div style="background: #222; padding: 15px; border-radius: 5px; margin-bottom: 10px;">
                <h4 style="margin: 0 0 10px 0; color: #4CAF50;">📡 Hotspot / Access Point Info</h4>
                <div style="margin: 10px 0; padding: 10px; background: #333; border-radius: 4px;">
                    <div style="margin-bottom: 8px;">
                        <span style="color: #888;">Default Hotspot SSID:</span>
                        <span style="color: #4CAF50; margin-left: 8px; font-weight: bold;">HP-esp32-wand-gateway</span>
                    </div>
                    <div style="margin-bottom: 8px;">
                        <span style="color: #888;">Security:</span>
                        <span style="color: #4CAF50; margin-left: 8px;">Open (No Password)</span>
                    </div>
                    <div>
                        <span style="color: #888;">IP Address:</span>
                        <span style="color: #4CAF50; margin-left: 8px;">192.168.4.1</span>
                    </div>
                </div>
                <div style="font-size: 0.85em; color: #888; margin-top: 10px; padding: 8px; background: rgba(76, 175, 80, 0.1); border-left: 3px solid #4CAF50;">
                    💡 The device automatically creates this hotspot when no WiFi network is available.
                </div>
            </div>
            <div style="background: #222; padding: 15px; border-radius: 5px;">
                <h4 style="margin: 0 0 10px 0; color: #4CAF50;">System Control</h4>
                <div style="margin-bottom: 15px;">
                    <label style="display: block; margin-bottom: 5px;">WiFi Mode:</label>
                    <select id="wifi-mode" style="width: 100%; padding: 8px; border-radius: 4px; background: #111; color: #eee; border: 1px solid #444; margin-bottom: 5px;">
                        <option value="client">Client Mode (Connect to WiFi)</option>
                        <option value="ap">Hotspot Mode (Access Point)</option>
                    </select>
                    <button class="button" onclick="switchWifiMode()">🔄 Switch WiFi Mode</button>
                    <div style="font-size: 0.8em; color: #888; margin-top: 5px;">Device will restart to apply mode change</div>
                </div>
                <div style="margin-bottom: 15px; padding-top: 15px; border-top: 1px solid #444;">
                    <button class="button danger" onclick="resetToDefaults()">⚠️ Reset to Defaults</button>
                    <div style="font-size: 0.8em; color: #888; margin-top: 5px;">Clears all settings (WiFi, wand MAC, MQTT)</div>
                </div>
                <div style="border-top: 1px solid #444; padding-top: 15px;">
                    <button class="button danger" onclick="rebootDevice()">🔄 Reboot Device</button>
                    <div style="font-size: 0.8em; color: #888; margin-top: 5px;">Device will restart in 2 seconds</div>
                </div>
            </div>
        </div>
        
        <div class="battery-box">
            🔋 Battery: <span id="battery" class="battery-level">--</span>%
        </div>
        
        <div class="spell-box" style="background: rgba(76, 175, 80, 0.1); padding: 15px; border-radius: 8px; margin-bottom: 20px;">
            <h3 style="margin-top: 0; color: #4CAF50;">📱 Wand Information</h3>
            <div style="display: grid; grid-template-columns: 120px 1fr; gap: 10px; font-size: 14px;">
                <div><strong>Wand Type:</strong></div><div id="wand-type" style="color: #4CAF50; font-weight: bold;">-</div>
                <div><strong>Firmware:</strong></div><div id="wand-firmware">-</div>
                <div><strong>Serial Number:</strong></div><div id="wand-serial">-</div>
                <div><strong>SKU:</strong></div><div id="wand-sku">-</div>
                <div><strong>Device ID:</strong></div><div id="wand-device-id">-</div>
            </div>
        </div>
        
        <div class="spell-box" style="background: rgba(33, 150, 243, 0.1); padding: 15px; border-radius: 8px; margin-bottom: 20px;">
            <h3 style="margin-top: 0; color: #2196F3;">🔘 Button Presses</h3>
            <div style="display: flex; gap: 30px; justify-content: center; font-size: 32px;">
                <div style="text-align: center;">
                    <div id="btn1" style="color: #666;">○</div>
                    <div style="font-size: 12px; margin-top: 5px;">B1</div>
                </div>
                <div style="text-align: center;">
                    <div id="btn2" style="color: #666;">○</div>
                    <div style="font-size: 12px; margin-top: 5px;">B2</div>
                </div>
                <div style="text-align: center;">
                    <div id="btn3" style="color: #666;">○</div>
                    <div style="font-size: 12px; margin-top: 5px;">B3</div>
                </div>
                <div style="text-align: center;">
                    <div id="btn4" style="color: #666;">○</div>
                    <div style="font-size: 12px; margin-top: 5px;">B4</div>
                </div>
            </div>
        </div>
        
        <div class="spell-box">
            <div id="spell-display">Waiting for spell...</div>
        </div>
        
        <div class="ble-controls">
            <h3>📚 Spell Learning</h3>
            <div class="spell-learning-controls">
                <select id="spell-selector">
                    <option value="">-- Select a spell to practice --</option>
                </select>
                <button class="button" onclick="practiceSpell()">📖 Load Reference</button>
                <button class="button secondary" onclick="clearReferenceGesture()">🗑️ Clear</button>
            </div>
        </div>
        
        <h2 style="text-align: center; color: #4CAF50; margin-top: 30px;">Gesture Path</h2>
        <canvas id="gesture-canvas" width="600" height="600"></canvas>
        
        <h2 style="text-align: center; color: #4CAF50; margin-top: 30px;">IMU Data</h2>
        <canvas id="imu-canvas" width="800" height="400"></canvas>
        
        <div class="data-grid">
            <div class="data-item">
                <div class="data-label">Accelerometer X</div>
                <div class="data-value" id="ax">0.00</div>
            </div>
            <div class="data-item">
                <div class="data-label">Accelerometer Y</div>
                <div class="data-value" id="ay">0.00</div>
            </div>
            <div class="data-item">
                <div class="data-label">Accelerometer Z</div>
                <div class="data-value" id="az">0.00</div>
            </div>
            <div class="data-item">
                <div class="data-label">Gyroscope X</div>
                <div class="data-value" id="gx">0.00</div>
            </div>
            <div class="data-item">
                <div class="data-label">Gyroscope Y</div>
                <div class="data-value" id="gy">0.00</div>
            </div>
            <div class="data-item">
                <div class="data-label">Gyroscope Z</div>
                <div class="data-value" id="gz">0.00</div>
            </div>
        </div>
    </div>
    
    <script>
        const canvas = document.getElementById('imu-canvas');
        const ctx = canvas.getContext('2d');
        const gestureCanvas = document.getElementById('gesture-canvas');
        const gestureCtx = gestureCanvas.getContext('2d');
        const statusDiv = document.getElementById('status');
        const statusText = document.getElementById('status-text');
        const wandStatus = document.getElementById('wand-status');
        
        let accelHistory = { x: [], y: [], z: [] };
        let gyroHistory = { x: [], y: [], z: [] };
        const maxHistory = 200;
        
        // Gesture tracking
        let gesturePoints = [];
        let rawGesturePoints = [];  // Store raw coordinates from ESP32
        let isTracking = false;
        
        // Gesture reference image for spell practice
        let referenceGestureImage = null;
        let referenceGestureLoaded = false;
        
        // WebSocket connection
        let ws = null;
        
        function connectWebSocket() {
            const wsUrl = `ws://${window.location.host}/ws`;
            ws = new WebSocket(wsUrl);
            
            ws.onopen = () => {
                console.log('WebSocket connected');
                statusText.textContent = 'Connected';
                statusDiv.classList.add('connected');
                // Request current wand status
                ws.send('{"type":"request_status"}');
            };
            
            ws.onclose = () => {
                console.log('WebSocket disconnected');
                statusText.textContent = 'Disconnected';
                statusDiv.classList.remove('connected');
                // Reconnect after 2 seconds
                setTimeout(connectWebSocket, 2000);
            };
            
            ws.onerror = (error) => {
                console.error('WebSocket error:', error);
            };
            
            ws.onmessage = (event) => {
                try {
                    const data = JSON.parse(event.data);
                    
                    if (data.type === 'wand_status') {
                        wandStatus.textContent = data.connected ? '✓ Connected' : '✗ Disconnected';
                        wandStatus.style.color = data.connected ? '#4CAF50' : '#ff4444';
                        if (!data.connected) {
                            clearWandInfo();
                        }
                    } else if (data.type === 'imu') {
                        updateIMU(data);
                    } else if (data.type === 'spell') {
                        showSpell(data.spell, data.confidence);
                    } else if (data.type === 'battery') {
                        updateBattery(data.level);
                    } else if (data.type === 'gesture_start') {
                        startGesture();
                    } else if (data.type === 'gesture_point') {
                        addGesturePoint(data.x, data.y);
                    } else if (data.type === 'gesture_end') {
                        endGesture();
                    } else if (data.type === 'scan_result') {
                        addScanResult(data.address, data.name, data.rssi);
                    } else if (data.type === 'scan_complete') {
                        scanComplete();
                    } else if (data.type === 'low_confidence') {
                        showLowConfidence(data.spell, data.confidence);
                    } else if (data.type === 'wand_info') {
                        showWandInfo(data);
                    } else if (data.type === 'button_press') {
                        updateButtons(data.b1, data.b2, data.b3, data.b4);
                    }
                } catch (e) {
                    console.error('Parse error:', e);
                }
            };
        }
        
        // Connect on page load
        connectWebSocket();
        
        function updateIMU(data) {
            // Update text displays
            document.getElementById('ax').textContent = data.ax.toFixed(2);
            document.getElementById('ay').textContent = data.ay.toFixed(2);
            document.getElementById('az').textContent = data.az.toFixed(2);
            document.getElementById('gx').textContent = data.gx.toFixed(2);
            document.getElementById('gy').textContent = data.gy.toFixed(2);
            document.getElementById('gz').textContent = data.gz.toFixed(2);
            
            // Update history
            accelHistory.x.push(data.ax);
            accelHistory.y.push(data.ay);
            accelHistory.z.push(data.az);
            gyroHistory.x.push(data.gx);
            gyroHistory.y.push(data.gy);
            gyroHistory.z.push(data.gz);
            
            if (accelHistory.x.length > maxHistory) {
                accelHistory.x.shift();
                accelHistory.y.shift();
                accelHistory.z.shift();
                gyroHistory.x.shift();
                gyroHistory.y.shift();
                gyroHistory.z.shift();
            }
            
            drawGraph();
        }
        
        function drawGraph() {
            ctx.fillStyle = '#000';
            ctx.fillRect(0, 0, canvas.width, canvas.height);
            
            const mid = canvas.height / 2;
            const scale = 50;
            
            // Draw center line
            ctx.strokeStyle = '#333';
            ctx.beginPath();
            ctx.moveTo(0, mid);
            ctx.lineTo(canvas.width, mid);
            ctx.stroke();
            
            // Draw accelerometer
            drawLine(accelHistory.x, '#ff4444', scale, mid);
            drawLine(accelHistory.y, '#44ff44', scale, mid);
            drawLine(accelHistory.z, '#4444ff', scale, mid);
            
            // Legend
            ctx.font = '12px Arial';
            ctx.fillStyle = '#ff4444';
            ctx.fillText('Accel X', 10, 20);
            ctx.fillStyle = '#44ff44';
            ctx.fillText('Accel Y', 80, 20);
            ctx.fillStyle = '#4444ff';
            ctx.fillText('Accel Z', 150, 20);
        }
        
        function drawLine(data, color, scale, mid) {
            if (data.length < 2) return;
            
            ctx.strokeStyle = color;
            ctx.lineWidth = 2;
            ctx.beginPath();
            
            const step = canvas.width / maxHistory;
            for (let i = 0; i < data.length; i++) {
                const x = i * step;
                const y = mid - (data[i] * scale);
                
                if (i === 0) {
                    ctx.moveTo(x, y);
                } else {
                    ctx.lineTo(x, y);
                }
            }
            
            ctx.stroke();
        }
        
        function showSpell(spell, confidence) {
            const display = document.getElementById('spell-display');
            display.innerHTML = `<span class="spell-name">${spell}</span><br>
                                <small>${(confidence * 100).toFixed(1)}% confidence</small>`;
            
            // Fade out after 5 seconds
            setTimeout(() => {
                display.textContent = 'Waiting for spell...';
            }, 5000);
        }
        
        function updateBattery(level) {
            const batteryElem = document.getElementById('battery');
            batteryElem.textContent = level;
            
            // Change color based on battery level
            if (level < 20) {
                batteryElem.className = 'battery-level battery-low';
            } else {
                batteryElem.className = 'battery-level';
            }
        }
        
        function startGesture() {
            isTracking = true;
            gesturePoints = [];
            rawGesturePoints = [];
            clearGestureCanvas();
        }
        
        function addGesturePoint(x, y) {
            if (!isTracking) return;
            
            // Store raw coordinates
            rawGesturePoints.push({x: x, y: y});
            
            // Redraw entire gesture with auto-scaling
            drawGesture();
        }
        
        function endGesture() {
            isTracking = false;
            // Redraw final gesture with optimal scaling
            drawGesture();
            console.log(`Gesture complete: ${rawGesturePoints.length} raw points captured`);
        }
        
        function clearGestureCanvas() {
            gestureCtx.fillStyle = '#000';
            gestureCtx.fillRect(0, 0, gestureCanvas.width, gestureCanvas.height);
            
            // Draw reference gesture image if loaded (semi-transparent)
            if (referenceGestureLoaded && referenceGestureImage) {
                const centerX = gestureCanvas.width / 2;
                const centerY = gestureCanvas.height / 2;
                
                // Scale image to fit canvas while maintaining aspect ratio
                const maxSize = Math.min(gestureCanvas.width, gestureCanvas.height) * 0.9;
                const scale = Math.min(maxSize / referenceGestureImage.width, maxSize / referenceGestureImage.height);
                const scaledWidth = referenceGestureImage.width * scale;
                const scaledHeight = referenceGestureImage.height * scale;
                
                // Draw with 40% opacity as reference
                gestureCtx.globalAlpha = 0.4;
                gestureCtx.drawImage(
                    referenceGestureImage,
                    centerX - scaledWidth / 2,
                    centerY - scaledHeight / 2,
                    scaledWidth,
                    scaledHeight
                );
                gestureCtx.globalAlpha = 1.0;
            }
            
            // Draw center crosshair on top
            gestureCtx.strokeStyle = '#444';
            gestureCtx.lineWidth = 1;
            gestureCtx.beginPath();
            const centerX = gestureCanvas.width / 2;
            const centerY = gestureCanvas.height / 2;
            gestureCtx.moveTo(centerX - 20, centerY);
            gestureCtx.lineTo(centerX + 20, centerY);
            gestureCtx.moveTo(centerX, centerY - 20);
            gestureCtx.lineTo(centerX, centerY + 20);
            gestureCtx.stroke();
        }
        
        function drawGesture() {
            clearGestureCanvas();
            
            if (rawGesturePoints.length === 0) return;
            
            const canvasCenterX = gestureCanvas.width / 2;
            const canvasCenterY = gestureCanvas.height / 2;
            
            // Offset all points so first point is at origin (0,0)
            const firstPoint = rawGesturePoints[0];
            const offsetPoints = rawGesturePoints.map(p => ({
                x: p.x - firstPoint.x,
                y: p.y - firstPoint.y
            }));
            
            // Fixed scale - no auto-scaling, just offset to center
            function toCanvas(x, y) {
                return {
                    x: canvasCenterX + x,
                    y: canvasCenterY - y  // Flip Y for screen coords
                };
            }
            
            // Draw starting point (green dot) at center
            const start = toCanvas(offsetPoints[0].x, offsetPoints[0].y);
            gestureCtx.fillStyle = '#00ff00';
            gestureCtx.beginPath();
            gestureCtx.arc(start.x, start.y, 5, 0, 2 * Math.PI);
            gestureCtx.fill();
            
            if (offsetPoints.length < 2) return;
            
            // Draw the gesture path
            gestureCtx.strokeStyle = '#00ffff';
            gestureCtx.lineWidth = 3;
            gestureCtx.lineCap = 'round';
            gestureCtx.lineJoin = 'round';
            gestureCtx.beginPath();
            gestureCtx.moveTo(start.x, start.y);
            
            for (let i = 1; i < offsetPoints.length; i++) {
                const p = toCanvas(offsetPoints[i].x, offsetPoints[i].y);
                gestureCtx.lineTo(p.x, p.y);
            }
            gestureCtx.stroke();
            
            // Draw ending point (red dot)
            const end = toCanvas(
                offsetPoints[offsetPoints.length - 1].x,
                offsetPoints[offsetPoints.length - 1].y
            );
            gestureCtx.fillStyle = '#ff0000';
            gestureCtx.beginPath();
            gestureCtx.arc(end.x, end.y, 5, 0, 2 * Math.PI);
            gestureCtx.fill();
            
            // Draw current endpoint (yellow) if tracking
            if (isTracking) {
                gestureCtx.fillStyle = '#ffff00';
                gestureCtx.beginPath();
                gestureCtx.arc(end.x, end.y, 8, 0, 2 * Math.PI);
                gestureCtx.fill();
            }
        }
        
        // Initialize gesture canvas
        clearGestureCanvas();
        
        // Spell Learning Functions
        const SPELL_NAMES = [
            "The_Force_Spell", "Colloportus", "Colloshoo", "The_Hour_Reversal_Reversal_Charm",
            "Evanesco", "Herbivicus", "Orchideous", "Brachiabindo", "Meteolojinx", "Riddikulus",
            "Silencio", "Immobulus", "Confringo", "Petrificus_Totalus", "Flipendo",
            "The_Cheering_Charm", "Salvio_Hexia", "Pestis_Incendium", "Alohomora", "Protego",
            "Langlock", "Mucus_Ad_Nauseum", "Flagrate", "Glacius", "Finite", "Anteoculatia",
            "Expelliarmus", "Expecto_Patronum", "Descendo", "Depulso", "Reducto", "Colovaria",
            "Aberto", "Confundo", "Densaugeo", "The_Stretching_Jinx", "Entomorphis",
            "The_Hair_Thickening_Growing_Charm", "Bombarda", "Finestra", "The_Sleeping_Charm",
            "Rictusempra", "Piertotum_Locomotor", "Expulso", "Impedimenta", "Ascendio",
            "Incarcerous", "Ventus", "Revelio", "Accio", "Melefors", "Scourgify",
            "Wingardium_Leviosa", "Nox", "Stupefy", "Spongify", "Lumos", "Appare_Vestigium",
            "Verdimillious", "Fulgari", "Reparo", "Locomotor", "Quietus", "Everte_Statum",
            "Incendio", "Aguamenti", "Sonorus", "Cantis", "Arania_Exumai", "Calvorio",
            "The_Hour_Reversal_Charm", "Vermillious", "The_Pepper-Breath_Hex"
        ];
        
        // Map spell names to SPIFFS filenames (32 char limit including .png)
        // Some names are shortened to fit SPIFFS filename restrictions
        const SPELL_FILENAME_MAP = {
            "The_Hair_Thickening_Growing_Charm": "hair_grow_charm.png",
            // Default: use lowercase with underscores
        };
        
        function spellNameToFilename(spellName) {
            // Check if there's a custom mapping
            if (SPELL_FILENAME_MAP[spellName]) {
                const mappedFilename = SPELL_FILENAME_MAP[spellName];
                console.log('[Filename Map] Custom mapping:', spellName, '->', mappedFilename);
                return mappedFilename;
            }
            // Default: convert to lowercase
            const filename = spellName.toLowerCase() + '.png';
            console.log('[Filename Map] Default mapping:', spellName, '->', filename);
            return filename;
        }
        
        function populateSpellSelector() {
            const selector = document.getElementById('spell-selector');
            SPELL_NAMES.forEach(spell => {
                const option = document.createElement('option');
                option.value = spell;
                option.textContent = spell.replace(/_/g, ' ');
                selector.appendChild(option);
            });
        }
        
        function practiceSpell() {
            const selector = document.getElementById('spell-selector');
            const selectedSpell = selector.value;
            
            console.log('[Spell Practice] Selected spell:', selectedSpell);
            
            if (!selectedSpell) {
                showToast('Please select a spell to practice', 'error');
                return;
            }
            
            const filename = spellNameToFilename(selectedSpell);
            const imageUrl = `/gesture/${filename}`;
            
            console.log('[Spell Practice] Loading reference:', filename);
            
            // Create image object
            const img = new Image();
            
            img.onload = function() {
                console.log('[Spell Practice] Reference image loaded:', imageUrl);
                referenceGestureImage = img;
                referenceGestureLoaded = true;
                
                // Redraw canvas with reference image
                clearGestureCanvas();
                drawGesture();
                
                showToast(`Reference loaded: ${selectedSpell.replace(/_/g, ' ')}`, 'success');
            };
            
            img.onerror = function() {
                console.error('[Spell Practice] Failed to load image:', imageUrl);
                showToast('Failed to load gesture image: ' + filename, 'error');
            };
            
            img.src = imageUrl;
        }
        
        function clearReferenceGesture() {
            referenceGestureImage = null;
            referenceGestureLoaded = false;
            clearGestureCanvas();
            drawGesture();
            console.log('[Spell Practice] Reference cleared');
            showToast('Reference cleared', 'success');
        }
        
        // Initialize spell selector on page load
        populateSpellSelector();
        
        // Toast notification function
        function showToast(message, type = 'success') {
            // Remove any existing toasts
            const existingToasts = document.querySelectorAll('.toast');
            existingToasts.forEach(toast => toast.remove());
            
            // Create new toast
            const toast = document.createElement('div');
            toast.className = `toast ${type}`;
            toast.textContent = message;
            
            // Add to document
            document.body.appendChild(toast);
            
            // Trigger animation by forcing reflow
            setTimeout(() => {
                toast.style.opacity = '1';
            }, 10);
            
            // Remove after 3 seconds
            setTimeout(() => {
                toast.style.opacity = '0';
                setTimeout(() => toast.remove(), 300);
            }, 3000);
        }
        
        // BLE Management Functions
        let scanResults = [];
        let selectedMac = null;
        
        function startScan() {
            const btn = document.getElementById('scanBtn');
            const status = document.getElementById('scanStatus');
            const results = document.getElementById('scanResults');
            
            btn.disabled = true;
            btn.textContent = '⏳ Scanning...';
            status.textContent = 'Scanning for BLE devices...';
            results.innerHTML = '';
            scanResults = [];
            selectedMac = null;
            
            fetch('/scan', { method: 'POST' })
                .then(response => response.json())
                .then(data => {
                    console.log('Scan started:', data);
                    setTimeout(() => {
                        btn.disabled = false;
                        btn.textContent = '🔍 Scan for Wands';
                    }, 10000); // Re-enable after 10 seconds
                })
                .catch(error => {
                    console.error('Scan error:', error);
                    status.textContent = 'Scan failed: ' + error;
                    btn.disabled = false;
                    btn.textContent = '🔍 Scan for Wands';
                });
        }
        
        function addScanResult(address, name, rssi) {
            // Check if already in list
            if (scanResults.find(r => r.address === address)) {
                return;
            }
            
            scanResults.push({ address, name, rssi });
            
            // Sort: MCB/MCW wands first, then by RSSI
            scanResults.sort((a, b) => {
                const aIsMC = (a.name && (a.name.startsWith('MCB') || a.name.startsWith('MCW')));
                const bIsMC = (b.name && (b.name.startsWith('MCB') || b.name.startsWith('MCW')));
                
                if (aIsMC && !bIsMC) return -1;  // a goes first
                if (!aIsMC && bIsMC) return 1;   // b goes first
                
                // Both are MC or both are not, sort by RSSI (higher is better)
                return b.rssi - a.rssi;
            });
            
            // Rebuild the display
            const results = document.getElementById('scanResults');
            results.innerHTML = '';
            
            scanResults.forEach(device => {
                const item = document.createElement('div');
                item.className = 'scan-item';
                const isMCWand = device.name && (device.name.startsWith('MCB') || device.name.startsWith('MCW'));
                const nameStyle = isMCWand ? 'style="color: #4CAF50; font-weight: bold;"' : '';
                item.innerHTML = `
                    <div class="scan-info">
                        <div class="mac-address">${device.address}</div>
                        <div ${nameStyle}>${device.name || 'Unknown Device'}</div>
                        <div class="rssi">RSSI: ${device.rssi} dBm</div>
                    </div>
                    <button class="button" onclick="selectWand('${device.address}', '${device.name}')">Select</button>
                `;
                results.appendChild(item);
            });
        }
        
        function scanComplete() {
            const status = document.getElementById('scanStatus');
            status.textContent = `Scan complete. Found ${scanResults.length} device(s).`;
        }
        
        function selectWand(address, name) {
            selectedMac = address;
            document.getElementById('storedMac').value = address;
            document.getElementById('connectBtn').disabled = false;
            document.getElementById('scanStatus').textContent = `Selected: ${name || 'Unknown'} (${address})`;
            
            // Save MAC address
            fetch('/set_mac', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ mac: address })
            })
            .then(response => response.json())
            .then(data => {
                console.log('MAC saved:', data);
                showToast(`Wand selected: ${name || address}`, 'success');
            })
            .catch(error => {
                console.error('Failed to save MAC:', error);
                showToast('Failed to save MAC address', 'error');
            });
        }
        
        function loadStoredMac() {
            fetch('/get_stored_mac')
                .then(response => response.json())
                .then(data => {
                    if (data.mac) {
                        document.getElementById('storedMac').value = data.mac;
                        selectedMac = data.mac;
                        document.getElementById('connectBtn').disabled = false;
                    } else {
                        document.getElementById('storedMac').value = '';
                        document.getElementById('scanStatus').textContent = 'No stored MAC address';
                    }
                })
                .catch(error => {
                    console.error('Failed to load MAC:', error);
                });
        }
        
        function connectWand() {
            if (!selectedMac) {
                showToast('Please select a wand first', 'error');
                return;
            }
            
            const btn = document.getElementById('connectBtn');
            btn.disabled = true;
            btn.textContent = '⏳ Connecting...';
            
            fetch('/connect', { method: 'POST' })
                .then(response => response.json())
                .then(data => {
                    console.log('Connect response:', data);
                    document.getElementById('scanStatus').textContent = data.status === 'connecting' ? 
                        'Connection initiated...' : data.message;
                    setTimeout(() => {
                        btn.disabled = false;
                        btn.textContent = '🔗 Connect';
                    }, 3000);
                })
                .catch(error => {
                    console.error('Connect error:', error);
                    document.getElementById('scanStatus').textContent = 'Connection failed';
                    btn.disabled = false;
                    btn.textContent = '🔗 Connect';
                });
        }
        
        function disconnectWand() {
            fetch('/disconnect', { method: 'POST' })
                .then(response => response.json())
                .then(data => {
                    console.log('Disconnect response:', data);
                    document.getElementById('scanStatus').textContent = 'Disconnected (manual reconnect required)';
                    // Update wand status to show disconnected
                    const wandStatus = document.getElementById('wandStatus');
                    wandStatus.textContent = '✗ Disconnected';
                    wandStatus.style.color = '#ff4444';
                })
                .catch(error => {
                    console.error('Disconnect error:', error);
                });
        }
        
        function showLowConfidence(spell, confidence) {
            const display = document.getElementById('spell-display');
            display.innerHTML = `<span style="color: #ff8800;">${spell}</span><br>
                                <small>${(confidence * 100).toFixed(1)}% confidence (low)</small>`;
        }
        
        function showWandInfo(data) {
            console.log('Wand Info:', data);
            document.getElementById('wand-type').textContent = data.wand_type || '-';
            document.getElementById('wand-firmware').textContent = data.firmware || '-';
            document.getElementById('wand-serial').textContent = data.serial || '-';
            document.getElementById('wand-sku').textContent = data.sku || '-';
            document.getElementById('wand-device-id').textContent = data.device_id || '-';
            
            if (data.wand_type && data.firmware) {
                document.getElementById('scanStatus').textContent = 
                    `Connected: ${data.wand_type} Wand (FW: ${data.firmware})`;
            }
        }
        
        function clearWandInfo() {
            console.log('Clearing wand info (disconnected)');
            document.getElementById('wand-type').textContent = '-';
            document.getElementById('wand-firmware').textContent = '-';
            document.getElementById('wand-serial').textContent = '-';
            document.getElementById('wand-sku').textContent = '-';
            document.getElementById('wand-device-id').textContent = '-';
            document.getElementById('battery').textContent = '--';
            document.getElementById('battery').className = 'battery-level';
            // Clear button states
            updateButtons(false, false, false, false);
        }
        
        function updateButtons(b1, b2, b3, b4) {
            document.getElementById('btn1').textContent = b1 ? '●' : '○';
            document.getElementById('btn2').textContent = b2 ? '●' : '○';
            document.getElementById('btn3').textContent = b3 ? '●' : '○';
            document.getElementById('btn4').textContent = b4 ? '●' : '○';
            document.getElementById('btn1').style.color = b1 ? '#4CAF50' : '#666';
            document.getElementById('btn2').style.color = b2 ? '#4CAF50' : '#666';
            document.getElementById('btn3').style.color = b3 ? '#4CAF50' : '#666';
            document.getElementById('btn4').style.color = b4 ? '#4CAF50' : '#666';
        }
        
        // SPELL_NAMES already declared above in Spell Learning section (line ~1033)

        const KEY_OPTIONS = [
            { group: 'Common', label: 'None', value: 0 },
            { group: 'Letters', label: 'A', value: 0x04 },
            { group: 'Letters', label: 'B', value: 0x05 },
            { group: 'Letters', label: 'C', value: 0x06 },
            { group: 'Letters', label: 'D', value: 0x07 },
            { group: 'Letters', label: 'E', value: 0x08 },
            { group: 'Letters', label: 'F', value: 0x09 },
            { group: 'Letters', label: 'G', value: 0x0A },
            { group: 'Letters', label: 'H', value: 0x0B },
            { group: 'Letters', label: 'I', value: 0x0C },
            { group: 'Letters', label: 'J', value: 0x0D },
            { group: 'Letters', label: 'K', value: 0x0E },
            { group: 'Letters', label: 'L', value: 0x0F },
            { group: 'Letters', label: 'M', value: 0x10 },
            { group: 'Letters', label: 'N', value: 0x11 },
            { group: 'Letters', label: 'O', value: 0x12 },
            { group: 'Letters', label: 'P', value: 0x13 },
            { group: 'Letters', label: 'Q', value: 0x14 },
            { group: 'Letters', label: 'R', value: 0x15 },
            { group: 'Letters', label: 'S', value: 0x16 },
            { group: 'Letters', label: 'T', value: 0x17 },
            { group: 'Letters', label: 'U', value: 0x18 },
            { group: 'Letters', label: 'V', value: 0x19 },
            { group: 'Letters', label: 'W', value: 0x1A },
            { group: 'Letters', label: 'X', value: 0x1B },
            { group: 'Letters', label: 'Y', value: 0x1C },
            { group: 'Letters', label: 'Z', value: 0x1D },
            { group: 'Numbers', label: '1', value: 0x1E },
            { group: 'Numbers', label: '2', value: 0x1F },
            { group: 'Numbers', label: '3', value: 0x20 },
            { group: 'Numbers', label: '4', value: 0x21 },
            { group: 'Numbers', label: '5', value: 0x22 },
            { group: 'Numbers', label: '6', value: 0x23 },
            { group: 'Numbers', label: '7', value: 0x24 },
            { group: 'Numbers', label: '8', value: 0x25 },
            { group: 'Numbers', label: '9', value: 0x26 },
            { group: 'Numbers', label: '0', value: 0x27 },
            { group: 'Controls', label: 'Enter', value: 0x28 },
            { group: 'Controls', label: 'Esc', value: 0x29 },
            { group: 'Controls', label: 'Backspace', value: 0x2A },
            { group: 'Controls', label: 'Tab', value: 0x2B },
            { group: 'Controls', label: 'Space', value: 0x2C },
            { group: 'Punctuation', label: '-', value: 0x2D },
            { group: 'Punctuation', label: '=', value: 0x2E },
            { group: 'Punctuation', label: '[', value: 0x2F },
            { group: 'Punctuation', label: ']', value: 0x30 },
            { group: 'Punctuation', label: '\\', value: 0x31 },
            { group: 'Punctuation', label: '#', value: 0x32 },
            { group: 'Punctuation', label: ';', value: 0x33 },
            { group: 'Punctuation', label: '\'', value: 0x34 },
            { group: 'Punctuation', label: '`', value: 0x35 },
            { group: 'Punctuation', label: ',', value: 0x36 },
            { group: 'Punctuation', label: '.', value: 0x37 },
            { group: 'Punctuation', label: '/', value: 0x38 },
            { group: 'Controls', label: 'Caps Lock', value: 0x39 },
            { group: 'Function', label: 'F1', value: 0x3A },
            { group: 'Function', label: 'F2', value: 0x3B },
            { group: 'Function', label: 'F3', value: 0x3C },
            { group: 'Function', label: 'F4', value: 0x3D },
            { group: 'Function', label: 'F5', value: 0x3E },
            { group: 'Function', label: 'F6', value: 0x3F },
            { group: 'Function', label: 'F7', value: 0x40 },
            { group: 'Function', label: 'F8', value: 0x41 },
            { group: 'Function', label: 'F9', value: 0x42 },
            { group: 'Function', label: 'F10', value: 0x43 },
            { group: 'Function', label: 'F11', value: 0x44 },
            { group: 'Function', label: 'F12', value: 0x45 },
            { group: 'System', label: 'Print Screen', value: 0x46 },
            { group: 'System', label: 'Scroll Lock', value: 0x47 },
            { group: 'System', label: 'Pause', value: 0x48 },
            { group: 'Navigation', label: 'Insert', value: 0x49 },
            { group: 'Navigation', label: 'Home', value: 0x4A },
            { group: 'Navigation', label: 'Page Up', value: 0x4B },
            { group: 'Navigation', label: 'Delete', value: 0x4C },
            { group: 'Navigation', label: 'End', value: 0x4D },
            { group: 'Navigation', label: 'Page Down', value: 0x4E },
            { group: 'Navigation', label: 'Arrow Right', value: 0x4F },
            { group: 'Navigation', label: 'Arrow Left', value: 0x50 },
            { group: 'Navigation', label: 'Arrow Down', value: 0x51 },
            { group: 'Navigation', label: 'Arrow Up', value: 0x52 },
            { group: 'Numpad', label: 'Num Lock', value: 0x53 },
            { group: 'Numpad', label: 'Numpad /', value: 0x54 },
            { group: 'Numpad', label: 'Numpad *', value: 0x55 },
            { group: 'Numpad', label: 'Numpad -', value: 0x56 },
            { group: 'Numpad', label: 'Numpad +', value: 0x57 },
            { group: 'Numpad', label: 'Numpad Enter', value: 0x58 },
            { group: 'Numpad', label: 'Numpad 1', value: 0x59 },
            { group: 'Numpad', label: 'Numpad 2', value: 0x5A },
            { group: 'Numpad', label: 'Numpad 3', value: 0x5B },
            { group: 'Numpad', label: 'Numpad 4', value: 0x5C },
            { group: 'Numpad', label: 'Numpad 5', value: 0x5D },
            { group: 'Numpad', label: 'Numpad 6', value: 0x5E },
            { group: 'Numpad', label: 'Numpad 7', value: 0x5F },
            { group: 'Numpad', label: 'Numpad 8', value: 0x60 },
            { group: 'Numpad', label: 'Numpad 9', value: 0x61 },
            { group: 'Numpad', label: 'Numpad 0', value: 0x62 },
            { group: 'Numpad', label: 'Numpad .', value: 0x63 },
            { group: 'Function', label: 'F13', value: 0x68 },
            { group: 'Function', label: 'F14', value: 0x69 },
            { group: 'Function', label: 'F15', value: 0x6A },
            { group: 'Function', label: 'F16', value: 0x6B },
            { group: 'Function', label: 'F17', value: 0x6C },
            { group: 'Function', label: 'F18', value: 0x6D },
            { group: 'Function', label: 'F19', value: 0x6E },
            { group: 'Function', label: 'F20', value: 0x6F },
            { group: 'Function', label: 'F21', value: 0x70 },
            { group: 'Function', label: 'F22', value: 0x71 },
            { group: 'Function', label: 'F23', value: 0x72 },
            { group: 'Function', label: 'F24', value: 0x73 }
        ];

        const GAMEPAD_BUTTON_OPTIONS = [
            { label: 'Disabled', value: 0 },
            { label: 'Button 1', value: 1 },
            { label: 'Button 2', value: 2 },
            { label: 'Button 3', value: 3 },
            { label: 'Button 4', value: 4 },
            { label: 'Button 5', value: 5 },
            { label: 'Button 6', value: 6 },
            { label: 'Button 7', value: 7 },
            { label: 'Button 8', value: 8 },
            { label: 'Button 9', value: 9 },
            { label: 'Button 10', value: 10 }
        ];

        function buildKeySelectOptions(select) {
            const groups = new Map();
            KEY_OPTIONS.forEach((opt) => {
                if (!groups.has(opt.group)) {
                    const optgroup = document.createElement('optgroup');
                    optgroup.label = opt.group;
                    groups.set(opt.group, optgroup);
                }
                const option = document.createElement('option');
                option.value = opt.value;
                option.textContent = opt.label;
                groups.get(opt.group).appendChild(option);
            });
            groups.forEach((optgroup) => select.appendChild(optgroup));
        }

        function buildGamepadSelectOptions(select) {
            GAMEPAD_BUTTON_OPTIONS.forEach((opt) => {
                const option = document.createElement('option');
                option.value = opt.value;
                option.textContent = opt.label;
                select.appendChild(option);
            });
        }

        // Populate spell mapping dropdowns
        function populateSpellMappings() {
            const container = document.getElementById('spell-mappings');
            container.innerHTML = '';

            for (let i = 0; i < SPELL_NAMES.length; i++) {
                const spell = SPELL_NAMES[i];
                const select = document.createElement('select');
                select.id = `spell_${i}`;
                buildKeySelectOptions(select);

                const label = document.createElement('label');
                label.style.cssText = 'font-size: 12px; word-break: break-word;';
                label.textContent = spell.replace(/_/g, ' ');

                const wrapper = document.createElement('div');
                wrapper.className = 'spell-mapping-item';
                wrapper.dataset.spellName = spell.toLowerCase().replace(/_/g, ' ');
                wrapper.appendChild(label);
                wrapper.appendChild(select);
                container.appendChild(wrapper);
            }
        }

        function populateGamepadMappings() {
            const container = document.getElementById('gamepad-mappings');
            container.innerHTML = '';

            for (let i = 0; i < SPELL_NAMES.length; i++) {
                const spell = SPELL_NAMES[i];
                const select = document.createElement('select');
                select.id = `gpad_spell_${i}`;
                buildGamepadSelectOptions(select);

                const label = document.createElement('label');
                label.style.cssText = 'font-size: 12px; word-break: break-word;';
                label.textContent = spell.replace(/_/g, ' ');

                const wrapper = document.createElement('div');
                wrapper.className = 'spell-mapping-item';
                wrapper.dataset.spellName = spell.toLowerCase().replace(/_/g, ' ');
                wrapper.appendChild(label);
                wrapper.appendChild(select);
                container.appendChild(wrapper);
            }
        }

        function filterSpellMappings() {
            const input = document.getElementById('spell-filter');
            const filter = input.value.trim().toLowerCase();
            const items = document.querySelectorAll('#spell-mappings .spell-mapping-item');
            items.forEach((item) => {
                const name = item.dataset.spellName || '';
                item.style.display = name.includes(filter) ? 'flex' : 'none';
            });
        }

        function filterGamepadMappings() {
            const input = document.getElementById('gamepad-spell-filter');
            const filter = input.value.trim().toLowerCase();
            const items = document.querySelectorAll('#gamepad-mappings .spell-mapping-item');
            items.forEach((item) => {
                const name = item.dataset.spellName || '';
                item.style.display = name.includes(filter) ? 'flex' : 'none';
            });
        }
        
        // Mouse sensitivity slider handler
        document.getElementById('mouse-sensitivity').addEventListener('input', (e) => {
            const value = parseFloat(e.target.value);
            document.getElementById('sens-value').textContent = value.toFixed(1) + 'x';
        });

        // Gamepad sensitivity slider handler
        document.getElementById('gamepad-sensitivity').addEventListener('input', (e) => {
            const value = parseFloat(e.target.value);
            document.getElementById('gpad-sens-value').textContent = value.toFixed(1) + 'x';
        });

        // Gamepad deadzone slider handler
        document.getElementById('gamepad-deadzone').addEventListener('input', (e) => {
            const value = parseFloat(e.target.value);
            document.getElementById('gpad-deadzone-value').textContent = value.toFixed(2);
        });
        
        // Load and save settings with spell mappings
        function saveSettings() {
            const settings = {
                mouse_sensitivity: parseFloat(document.getElementById('mouse-sensitivity').value),
                invert_mouse_y: document.getElementById('invert-mouse-y').checked,
                hid_mode: parseInt(document.getElementById('hid-mode').value),
                gamepad_sensitivity: parseFloat(document.getElementById('gamepad-sensitivity').value),
                gamepad_deadzone: parseFloat(document.getElementById('gamepad-deadzone').value),
                gamepad_invert_y: document.getElementById('invert-gamepad-y').checked,
                ha_mqtt_enabled: document.getElementById('ha-mqtt-enabled').checked,
                mqtt_broker: document.getElementById('mqtt-broker').value,
                mqtt_username: document.getElementById('mqtt-username').value,
                mqtt_password: document.getElementById('mqtt-password').value,
                spells: [],
                gamepad_spells: []
            };
            
            // Collect all spell keycode mappings
            for (let i = 0; i < SPELL_NAMES.length; i++) {
                const select = document.getElementById(`spell_${i}`);
                settings.spells.push(parseInt(select.value));
            }

            // Collect all spell gamepad button mappings
            for (let i = 0; i < SPELL_NAMES.length; i++) {
                const select = document.getElementById(`gpad_spell_${i}`);
                settings.gamepad_spells.push(parseInt(select.value));
            }
            
            fetch('/settings/save', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(settings)
            })
            .then(response => response.json())
            .then(data => {
                showToast('Settings saved successfully!', 'success');
                console.log('Settings saved:', data);
            })
            .catch(error => {
                showToast('Failed to save settings', 'error');
                console.error('Save error:', error);
            });
        }
        
        function loadSettings() {
            fetch('/settings/get')
                .then(response => response.json())
                .then(data => {
                    console.log('Settings loaded:', data);
                    document.getElementById('mouse-sensitivity').value = data.mouse_sensitivity || 1.0;
                    document.getElementById('sens-value').textContent = (data.mouse_sensitivity || 1.0).toFixed(1) + 'x';
                    document.getElementById('invert-mouse-y').checked = data.invert_mouse_y !== false;
                    document.getElementById('hid-mode').value = (data.hid_mode !== undefined) ? data.hid_mode : 0;
                    document.getElementById('gamepad-sensitivity').value = data.gamepad_sensitivity || 1.0;
                    document.getElementById('gpad-sens-value').textContent = (data.gamepad_sensitivity || 1.0).toFixed(1) + 'x';
                    document.getElementById('gamepad-deadzone').value = (data.gamepad_deadzone !== undefined) ? data.gamepad_deadzone : 0.05;
                    document.getElementById('gpad-deadzone-value').textContent = ((data.gamepad_deadzone !== undefined) ? data.gamepad_deadzone : 0.05).toFixed(2);
                    document.getElementById('invert-gamepad-y').checked = data.gamepad_invert_y !== false;
                    document.getElementById('ha-mqtt-enabled').checked = data.ha_mqtt_enabled !== false;
                    document.getElementById('mqtt-broker').value = data.mqtt_broker || '';
                    document.getElementById('mqtt-username').value = data.mqtt_username || '';
                    document.getElementById('mqtt-password').value = data.mqtt_password || '';
                    
                    // Load spell keycodes
                    if (data.spells && data.spells.length === SPELL_NAMES.length) {
                        for (let i = 0; i < data.spells.length; i++) {
                            const select = document.getElementById(`spell_${i}`);
                            if (select) {
                                select.value = data.spells[i];
                            }
                        }
                    }
                    if (data.gamepad_spells && data.gamepad_spells.length === SPELL_NAMES.length) {
                        for (let i = 0; i < data.gamepad_spells.length; i++) {
                            const select = document.getElementById(`gpad_spell_${i}`);
                            if (select) {
                                select.value = data.gamepad_spells[i];
                            }
                        }
                    }
                    showToast('Settings loaded from device', 'success');
                })
                .catch(error => {
                    showToast('Failed to load settings', 'error');
                    console.error('Load error:', error);
                });
        }
        
        function resetSettings() {
            if (confirm('⚠️ Reset all settings to defaults?')) {
                fetch('/settings/reset', { method: 'POST' })
                    .then(response => response.json())
                    .then(data => {
                        console.log('Settings reset:', data);
                        // Reset all spell mappings to 0 (disabled)
                        for (let i = 0; i < SPELL_NAMES.length; i++) {
                            const select = document.getElementById(`spell_${i}`);
                            if (select) select.value = 0;
                        }
                        for (let i = 0; i < SPELL_NAMES.length; i++) {
                            const select = document.getElementById(`gpad_spell_${i}`);
                            if (select) select.value = 0;
                        }
                        document.getElementById('mouse-sensitivity').value = 1.0;
                        document.getElementById('sens-value').textContent = '1.0x';
                        document.getElementById('gamepad-sensitivity').value = 1.0;
                        document.getElementById('gpad-sens-value').textContent = '1.0x';
                        document.getElementById('gamepad-deadzone').value = 0.05;
                        document.getElementById('gpad-deadzone-value').textContent = '0.05';
                        document.getElementById('invert-gamepad-y').checked = true;
                        showToast('Settings reset to defaults!', 'success');
                    })
                    .catch(error => {
                        showToast('Failed to reset settings', 'error');
                        console.error('Reset error:', error);
                    });
            }
        }
        
        // Initialize UI
        populateSpellMappings();
        populateGamepadMappings();
        
        // Load settings on page load
        setTimeout(loadSettings, 2000);
        
        // Load stored MAC on page load
        setTimeout(loadStoredMac, 1000);
        
        // WiFi Management Functions
        function scanWifi() {
            const btn = event.target;
            const status = document.getElementById('wifiScanStatus');
            const results = document.getElementById('wifiResults');
            
            btn.disabled = true;
            btn.textContent = '⏳ Scanning...';
            status.textContent = 'Scanning for WiFi networks...';
            results.innerHTML = '';
            
            fetch('/wifi/scan', { method: 'POST' })
                .then(response => response.json())
                .then(data => {
                    if (data.networks && data.networks.length > 0) {
                        status.textContent = `Found ${data.networks.length} network(s)`;
                        data.networks.forEach(network => {
                            const item = document.createElement('div');
                            item.className = 'scan-item';
                            item.innerHTML = `
                                <div class="scan-info">
                                    <div style="font-weight: bold;">${network.ssid}</div>
                                    <div class="rssi">RSSI: ${network.rssi} dBm | Security: ${network.auth}</div>
                                </div>
                                <button class="button" onclick="selectWifiNetwork('${network.ssid}')">Select</button>
                            `;
                            results.appendChild(item);
                        });
                    } else {
                        status.textContent = 'No networks found';
                    }
                    btn.disabled = false;
                    btn.textContent = '🔍 Scan WiFi Networks';
                })
                .catch(error => {
                    status.textContent = 'Scan failed: ' + error;
                    btn.disabled = false;
                    btn.textContent = '🔍 Scan WiFi Networks';
                    showToast('WiFi scan failed', 'error');
                });
        }
        
        function selectWifiNetwork(ssid) {
            document.getElementById('wifi-ssid').value = ssid;
            showToast(`Selected: ${ssid}`, 'success');
        }
        
        function connectWifi() {
            const ssid = document.getElementById('wifi-ssid').value;
            const password = document.getElementById('wifi-password').value;
            const status = document.getElementById('wifiConnectStatus');
            
            if (!ssid) {
                showToast('Please enter WiFi SSID', 'error');
                return;
            }
            
            if (!confirm(`⚠️ Connecting to ${ssid} will reboot the device. Continue?`)) {
                return;
            }
            
            status.textContent = 'Saving WiFi settings and rebooting...';
            
            fetch('/wifi/connect', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ ssid: ssid, password: password })
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    status.textContent = 'Device rebooting to apply WiFi settings...';
                    showToast('Rebooting to connect to WiFi...', 'success');
                    setTimeout(() => {
                        window.location.reload();
                    }, 5000);
                } else {
                    status.textContent = 'Connection failed: ' + (data.message || 'Unknown error');
                    showToast('WiFi connection failed', 'error');
                }
            })
            .catch(error => {
                status.textContent = 'Device rebooting...';
                showToast('Device rebooting...', 'success');
                setTimeout(() => {
                    window.location.reload();
                }, 5000);
            });
        }
        
        function rebootDevice() {
            if (!confirm('⚠️ Are you sure you want to reboot the device?')) {
                return;
            }
            
            showToast('Rebooting device...', 'success');
            
            fetch('/system/reboot', { method: 'POST' })
                .then(response => response.json())
                .then(data => {
                    showToast('Device rebooting...', 'success');
                    setTimeout(() => {
                        window.location.reload();
                    }, 5000);
                })
                .catch(error => {
                    showToast('Reboot command sent', 'success');
                    setTimeout(() => {
                        window.location.reload();
                    }, 5000);
                });
        }
        
        function switchWifiMode() {
            const mode = document.getElementById('wifi-mode').value;
            
            if (!confirm(`⚠️ Switch to ${mode === 'ap' ? 'Hotspot' : 'Client'} mode? Device will reboot.`)) {
                return;
            }
            
            showToast('Switching WiFi mode...', 'success');
            
            fetch('/system/wifi_mode', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ mode: mode })
            })
            .then(response => response.json())
            .then(data => {
                showToast('Device rebooting to apply mode change...', 'success');
                setTimeout(() => {
                    window.location.reload();
                }, 5000);
            })
            .catch(error => {
                showToast('Mode change command sent, device rebooting...', 'success');
                setTimeout(() => {
                    window.location.reload();
                }, 5000);
            });
        }
        
        function resetToDefaults() {
            if (!confirm('⚠️ WARNING: This will erase ALL settings including WiFi credentials, wand MAC, and MQTT settings. Continue?')) {
                return;
            }
            
            if (!confirm('⚠️ FINAL CONFIRMATION: Are you absolutely sure? This cannot be undone!')) {
                return;
            }
            
            showToast('Resetting to defaults...', 'success');
            
            fetch('/system/reset_nvs', { method: 'POST' })
                .then(response => response.json())
                .then(data => {
                    showToast('Settings cleared! Device rebooting...', 'success');
                    setTimeout(() => {
                        window.location.href = 'http://192.168.4.1/';
                    }, 5000);
                })
                .catch(error => {
                    showToast('Reset command sent, device rebooting...', 'success');
                    setTimeout(() => {
                        window.location.href = 'http://192.168.4.1/';
                    }, 5000);
                });
        }
        
        // Load current WiFi mode and set dropdown
        function loadWifiMode() {
            fetch('/system/get_wifi_mode')
                .then(response => response.json())
                .then(data => {
                    if (data.success && data.mode) {
                        const dropdown = document.getElementById('wifi-mode');
                        if (dropdown) {
                            dropdown.value = data.mode;
                            console.log('Current WiFi mode:', data.mode, 'Force AP:', data.force_ap);
                        }
                    }
                })
                .catch(error => {
                    console.error('Failed to load WiFi mode:', error);
                });
        }
        
        // Load WiFi mode when page loads
        loadWifiMode();
    </script>
</body>
</html>
)rawliteral";

WebServer::WebServer()
    : server(nullptr), running(false), ws_client_count(0)
{
    memset(ws_clients, 0, sizeof(ws_clients));
    memset(&cached_data, 0, sizeof(cached_data));
    cached_data.wand_connected = false;

    client_mutex = xSemaphoreCreateMutex();
    if (!client_mutex)
    {
        ESP_LOGE(TAG, "FATAL: Failed to create client_mutex");
    }

    data_mutex = xSemaphoreCreateMutex();
    if (!data_mutex)
    {
        ESP_LOGE(TAG, "FATAL: Failed to create data_mutex");
    }
}

WebServer::~WebServer()
{
    stop();
    if (client_mutex)
    {
        vSemaphoreDelete(client_mutex);
    }
    if (data_mutex)
    {
        vSemaphoreDelete(data_mutex);
    }
}

// Captive portal handler - redirect all unknown requests to root
esp_err_t WebServer::captive_portal_handler(httpd_req_t *req)
{
    // Redirect to root page
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

bool WebServer::begin(uint16_t port)
{
    if (running)
    {
        ESP_LOGW(TAG, "Server already running");
        return true;
    }

    // Initialize SPIFFS for gesture images
    ESP_LOGI(TAG, "Initializing SPIFFS for gesture images...");
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = true // Auto-format empty partition
    };

    esp_err_t ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGW(TAG, "SPIFFS mount failed - partition may be corrupted or not flashed");
            ESP_LOGW(TAG, "Run './upload_gestures.sh' to flash gesture images");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGW(TAG, "SPIFFS partition not found - check partition table");
            ESP_LOGW(TAG, "Expected: offset=0x490000, size=0x370000 (3.6MB)");
        }
        else
        {
            ESP_LOGW(TAG, "SPIFFS init failed (%s) - gesture images unavailable", esp_err_to_name(ret));
        }
    }
    else
    {
        size_t total = 0, used = 0;
        ret = esp_spiffs_info("spiffs", &total, &used);
        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG, "SPIFFS: %d KB total, %d KB used", total / 1024, used / 1024);
            if (used == 0)
            {
                ESP_LOGI(TAG, "SPIFFS is empty - run './upload_gestures.sh' to upload gesture images");
            }
            else
            {
                // List SPIFFS contents for debugging
                ESP_LOGI(TAG, "Listing SPIFFS contents:");
                DIR *dir = opendir("/spiffs");
                if (dir)
                {
                    struct dirent *entry;
                    int file_count = 0;
                    while ((entry = readdir(dir)) != NULL && file_count < 10)
                    {
                        ESP_LOGI(TAG, "  - %s", entry->d_name);
                        file_count++;
                    }
                    if (file_count == 10)
                    {
                        ESP_LOGI(TAG, "  ... (showing first 10 files)");
                    }
                    closedir(dir);
                }
                else
                {
                    ESP_LOGW(TAG, "Failed to open SPIFFS directory for listing");
                }
            }
        }
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_open_sockets = 7;
    config.max_uri_handlers = 25; // Support all handlers + buffer for future endpoints
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return false;
    }

    // Register handlers
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &root) != ESP_OK)
    {
        ESP_LOGW(TAG, "Root handler registration FAILED");
    }

    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = this,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &ws) != ESP_OK)
    {
        ESP_LOGW(TAG, "WebSocket handler registration FAILED");
    }

    // Captive portal handlers for Android/iOS detection
    httpd_uri_t captive_generate_204 = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = captive_portal_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &captive_generate_204) != ESP_OK)
    {
        ESP_LOGW(TAG, "Captive portal /generate_204 handler registration FAILED");
    }

    httpd_uri_t captive_hotspot = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = captive_portal_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &captive_hotspot) != ESP_OK)
    {
        ESP_LOGW(TAG, "Captive portal /hotspot-detect.html handler registration FAILED");
    }

    // BLE scan and MAC management handlers
    httpd_uri_t scan = {
        .uri = "/scan",
        .method = HTTP_POST,
        .handler = scan_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &scan) != ESP_OK)
    {
        ESP_LOGW(TAG, "Scan handler registration FAILED");
    }

    httpd_uri_t set_mac = {
        .uri = "/set_mac",
        .method = HTTP_POST,
        .handler = set_mac_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &set_mac) != ESP_OK)
    {
        ESP_LOGW(TAG, "Set MAC handler registration FAILED");
    }

    httpd_uri_t get_stored_mac = {
        .uri = "/get_stored_mac",
        .method = HTTP_GET,
        .handler = get_stored_mac_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &get_stored_mac) != ESP_OK)
    {
        ESP_LOGW(TAG, "Get stored MAC handler registration FAILED");
    }

    httpd_uri_t connect = {
        .uri = "/connect",
        .method = HTTP_POST,
        .handler = connect_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &connect) != ESP_OK)
    {
        ESP_LOGW(TAG, "Connect handler registration FAILED");
    }

    httpd_uri_t disconnect = {
        .uri = "/disconnect",
        .method = HTTP_POST,
        .handler = disconnect_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &disconnect) != ESP_OK)
    {
        ESP_LOGW(TAG, "Disconnect handler registration FAILED");
    }

    // Settings endpoints
    httpd_uri_t settings_get = {
        .uri = "/settings/get",
        .method = HTTP_GET,
        .handler = settings_get_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &settings_get) != ESP_OK)
    {
        ESP_LOGW(TAG, "Settings GET handler registration FAILED");
    }

    httpd_uri_t settings_save = {
        .uri = "/settings/save",
        .method = HTTP_POST,
        .handler = settings_save_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &settings_save) != ESP_OK)
    {
        ESP_LOGW(TAG, "Settings SAVE handler registration FAILED");
    }

    httpd_uri_t settings_reset = {
        .uri = "/settings/reset",
        .method = HTTP_POST,
        .handler = settings_reset_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &settings_reset) != ESP_OK)
    {
        ESP_LOGW(TAG, "Settings RESET handler registration FAILED");
    }

    httpd_uri_t wifi_scan = {
        .uri = "/wifi/scan",
        .method = HTTP_POST,
        .handler = wifi_scan_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &wifi_scan) != ESP_OK)
    {
        ESP_LOGW(TAG, "WiFi scan handler registration FAILED");
    }

    httpd_uri_t wifi_connect = {
        .uri = "/wifi/connect",
        .method = HTTP_POST,
        .handler = wifi_connect_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &wifi_connect) != ESP_OK)
    {
        ESP_LOGW(TAG, "WiFi connect handler registration FAILED");
    }

    httpd_uri_t hotspot_settings = {
        .uri = "/hotspot/settings",
        .method = HTTP_POST,
        .handler = hotspot_settings_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &hotspot_settings) != ESP_OK)
    {
        ESP_LOGW(TAG, "Hotspot settings handler registration FAILED");
    }

    httpd_uri_t hotspot_get = {
        .uri = "/hotspot/get",
        .method = HTTP_GET,
        .handler = hotspot_get_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &hotspot_get) != ESP_OK)
    {
        ESP_LOGW(TAG, "Hotspot get handler registration FAILED");
    }

    httpd_uri_t system_reboot = {
        .uri = "/system/reboot",
        .method = HTTP_POST,
        .handler = system_reboot_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &system_reboot) != ESP_OK)
    {
        ESP_LOGW(TAG, "System reboot handler registration FAILED");
    }

    httpd_uri_t system_wifi_mode = {
        .uri = "/system/wifi_mode",
        .method = HTTP_POST,
        .handler = system_wifi_mode_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &system_wifi_mode) != ESP_OK)
    {
        ESP_LOGW(TAG, "System wifi_mode handler registration FAILED");
    }

    httpd_uri_t system_reset_nvs = {
        .uri = "/system/reset_nvs",
        .method = HTTP_POST,
        .handler = system_reset_nvs_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &system_reset_nvs) != ESP_OK)
    {
        ESP_LOGW(TAG, "System reset_nvs handler registration FAILED");
    }

    httpd_uri_t system_get_wifi_mode = {
        .uri = "/system/get_wifi_mode",
        .method = HTTP_GET,
        .handler = system_get_wifi_mode_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    if (httpd_register_uri_handler(server, &system_get_wifi_mode) != ESP_OK)
    {
        ESP_LOGW(TAG, "System get_wifi_mode handler registration FAILED");
    }

    // Register 404 error handler to intercept gesture image requests
    // ESP-IDF httpd wildcards don't work well, so use error handler approach
    ESP_LOGI(TAG, "Registering 404 handler for gesture images");
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, gesture_404_handler);

    running = true;
    ESP_LOGI(TAG, "Web server started on port %d", port);
    ESP_LOGI(TAG, "Registered endpoints: /, /ws, /generate_204, /hotspot-detect.html, /scan, /set_mac, /get_stored_mac, /connect, /disconnect, /settings/get, /settings/save, /settings/reset, /wifi/scan, /wifi/connect, /hotspot/settings, /hotspot/get, /system/reboot, [404:gesture/*]");
    return true;
}

void WebServer::stop()
{
    if (server)
    {
        httpd_stop(server);
        server = nullptr;
        running = false;
    }
}

esp_err_t WebServer::root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html, strlen(index_html));
    return ESP_OK;
}

esp_err_t WebServer::ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
    {
        // Initial WebSocket handshake - add client immediately
        WebServer *server = (WebServer *)req->user_ctx;
        int fd = httpd_req_to_sockfd(req);
        server->addWebSocketClient(fd);

        ESP_LOGI(TAG, "WebSocket client connected, fd=%d", fd);

        // Don't send data during handshake - WebSocket isn't fully established yet
        // The client will receive status via normal broadcasts after connection is complete

        return ESP_OK;
    }

    // Handle incoming WebSocket frames (pings, client messages, etc.)
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    // Get frame info
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %d", ret);
        return ret;
    }

    // Handle incoming messages from client
    if (ws_pkt.len > 0 && ws_pkt.type == HTTPD_WS_TYPE_TEXT)
    {
        // Allocate buffer for payload
        uint8_t *buf = (uint8_t *)malloc(ws_pkt.len + 1);
        if (buf)
        {
            ws_pkt.payload = buf;
            ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
            if (ret == ESP_OK)
            {
                buf[ws_pkt.len] = 0; // Null terminate

                // Check if client is requesting status
                if (strstr((char *)buf, "request_status") != NULL)
                {
                    WebServer *server = (WebServer *)req->user_ctx;
                    bool wand_connected = false;
                    char firmware[32], serial[32], sku[32], device_id[32], wand_type[32];

                    if (xSemaphoreTake(server->data_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
                    {
                        wand_connected = server->cached_data.wand_connected;
                        strncpy(firmware, server->cached_data.firmware_version, sizeof(firmware));
                        strncpy(serial, server->cached_data.serial_number, sizeof(serial));
                        strncpy(sku, server->cached_data.sku, sizeof(sku));
                        strncpy(device_id, server->cached_data.device_id, sizeof(device_id));
                        strncpy(wand_type, server->cached_data.wand_type, sizeof(wand_type));
                        xSemaphoreGive(server->data_mutex);
                    }

                    // Send wand status response
                    char response[100];
                    snprintf(response, sizeof(response), "{\"type\":\"wand_status\",\"connected\":%s}",
                             wand_connected ? "true" : "false");

                    httpd_ws_frame_t resp_pkt;
                    memset(&resp_pkt, 0, sizeof(httpd_ws_frame_t));
                    resp_pkt.payload = (uint8_t *)response;
                    resp_pkt.len = strlen(response);
                    resp_pkt.type = HTTPD_WS_TYPE_TEXT;
                    httpd_ws_send_frame(req, &resp_pkt);

                    // Also send cached wand info if available
                    if (wand_connected && serial[0] != '\0')
                    {
                        char wand_info[512];
                        snprintf(wand_info, sizeof(wand_info),
                                 "{\"type\":\"wand_info\",\"firmware\":\"%s\",\"serial\":\"%s\",\"sku\":\"%s\",\"device_id\":\"%s\",\"wand_type\":\"%s\"}",
                                 firmware, serial, sku, device_id, wand_type);

                        vTaskDelay(pdMS_TO_TICKS(10)); // Small delay between frames

                        memset(&resp_pkt, 0, sizeof(httpd_ws_frame_t));
                        resp_pkt.payload = (uint8_t *)wand_info;
                        resp_pkt.len = strlen(wand_info);
                        resp_pkt.type = HTTPD_WS_TYPE_TEXT;
                        httpd_ws_send_frame(req, &resp_pkt);
                    }
                }
            }
            free(buf);
        }
    }

    return ESP_OK;
}

esp_err_t WebServer::data_handler(httpd_req_t *req)
{
    WebServer *server = (WebServer *)req->user_ctx;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    char json[512];

    if (xSemaphoreTake(server->data_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        if (server->cached_data.has_spell)
        {
            snprintf(json, sizeof(json),
                     "{\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,\"gx\":%.3f,\"gy\":%.3f,\"gz\":%.3f,"
                     "\"spell\":\"%s\",\"confidence\":%.3f,\"battery\":%d}",
                     server->cached_data.ax, server->cached_data.ay, server->cached_data.az,
                     server->cached_data.gx, server->cached_data.gy, server->cached_data.gz,
                     server->cached_data.spell, server->cached_data.confidence,
                     server->cached_data.battery);
            server->cached_data.has_spell = false; // Clear after sending
        }
        else
        {
            snprintf(json, sizeof(json),
                     "{\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,\"gx\":%.3f,\"gy\":%.3f,\"gz\":%.3f,"
                     "\"battery\":%d}",
                     server->cached_data.ax, server->cached_data.ay, server->cached_data.az,
                     server->cached_data.gx, server->cached_data.gy, server->cached_data.gz,
                     server->cached_data.battery);
        }
        xSemaphoreGive(server->data_mutex);
    }
    else
    {
        snprintf(json, sizeof(json), "{\"error\":\"timeout\"}");
    }

    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

void WebServer::broadcastIMU(float ax, float ay, float az, float gx, float gy, float gz)
{
    if (!running)
        return;

    char json[300];
    snprintf(json, sizeof(json),
             "{\"type\":\"imu\",\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,\"gx\":%.3f,\"gy\":%.3f,\"gz\":%.3f}",
             ax, ay, az, gx, gy, gz);

    broadcast_to_clients(server, ws_clients, &ws_client_count, client_mutex, json);
}

void WebServer::broadcastSpell(const char *spell_name, float confidence)
{
    if (!running || !spell_name)
        return;

    char json[300];
    snprintf(json, sizeof(json),
             "{\"type\":\"spell\",\"spell\":\"%s\",\"confidence\":%.3f}",
             spell_name, confidence);

    broadcast_to_clients(server, ws_clients, &ws_client_count, client_mutex, json);
}

void WebServer::broadcastBattery(uint8_t level)
{
    if (!running)
        return;

    char json[150];
    snprintf(json, sizeof(json),
             "{\"type\":\"battery\",\"level\":%d}",
             level);

    broadcast_to_clients(server, ws_clients, &ws_client_count, client_mutex, json);
}

void WebServer::broadcastWandStatus(bool connected)
{
    if (!running)
        return;

    // Update cached status
    if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        cached_data.wand_connected = connected;
        xSemaphoreGive(data_mutex);
    }

    char json[150];
    snprintf(json, sizeof(json),
             "{\"type\":\"wand_status\",\"connected\":%s}",
             connected ? "true" : "false");

    broadcast_to_clients(server, ws_clients, &ws_client_count, client_mutex, json);
}

void WebServer::broadcastGestureStart()
{
    if (!running)
        return;

    ESP_LOGI(TAG, "Broadcasting gesture_start to %d clients", ws_client_count);
    const char *json = "{\"type\":\"gesture_start\"}";
    broadcast_to_clients(server, ws_clients, &ws_client_count, client_mutex, json);
}

void WebServer::broadcastGesturePoint(float x, float y)
{
    if (!running)
    {
        return;
    }

    char json[200];
    // Flip both axes to match wand movement direction with screen display
    // (wand right = screen right, wand down = screen down)
    snprintf(json, sizeof(json),
             "{\"type\":\"gesture_point\",\"x\":%.4f,\"y\":%.4f}",
             x, -y);
    broadcast_to_clients(server, ws_clients, &ws_client_count, client_mutex, json);
}

void WebServer::broadcastGestureEnd()
{
    if (!running)
        return;

    ESP_LOGI(TAG, "Broadcasting gesture_end to %d clients", ws_client_count);
    const char *json = "{\"type\":\"gesture_end\"}";
    broadcast_to_clients(server, ws_clients, &ws_client_count, client_mutex, json);
}

void WebServer::addWebSocketClient(int fd)
{
    if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        if (ws_client_count >= 10)
        {
            ESP_LOGW(TAG, "Max WebSocket clients reached, rejecting connection");
            xSemaphoreGive(client_mutex);
            return;
        }

        ws_clients[ws_client_count++] = fd;
        ESP_LOGI(TAG, "WebSocket client added (total: %d)", ws_client_count);
        xSemaphoreGive(client_mutex);
    }
}

void WebServer::removeWebSocketClient(int fd)
{
    if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        for (int i = 0; i < ws_client_count; i++)
        {
            if (ws_clients[i] == fd)
            {
                for (int j = i; j < ws_client_count - 1; j++)
                {
                    ws_clients[j] = ws_clients[j + 1];
                }
                ws_client_count--;
                ESP_LOGI(TAG, "WebSocket client removed (total: %d)", ws_client_count);
                break;
            }
        }
        xSemaphoreGive(client_mutex);
    }
}

void WebServer::broadcastLowConfidence(const char *spell_name, float confidence)
{
    if (!running || !spell_name)
        return;

    char json[256];
    snprintf(json, sizeof(json),
             "{\"type\":\"low_confidence\",\"spell\":\"%s\",\"confidence\":%.4f}",
             spell_name, confidence);
    broadcast_to_clients(server, ws_clients, &ws_client_count, client_mutex, json);

    ESP_LOGI(TAG, "Low confidence prediction: %s (%.2f%%)", spell_name, confidence * 100.0f);
}

void WebServer::broadcastWandInfo(const char *firmware_version, const char *serial_number,
                                  const char *sku, const char *device_id, const char *wand_type)
{
    // Sanitize all input strings
    char safe_fw[32], safe_serial[32], safe_sku[32], safe_devid[32], safe_type[32];
    sanitize_for_json(safe_fw, firmware_version, sizeof(safe_fw));
    sanitize_for_json(safe_serial, serial_number, sizeof(safe_serial));
    sanitize_for_json(safe_sku, sku, sizeof(safe_sku));
    sanitize_for_json(safe_devid, device_id, sizeof(safe_devid));
    sanitize_for_json(safe_type, wand_type, sizeof(safe_type));

    // Cache the wand info for new clients
    if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        snprintf(cached_data.firmware_version, sizeof(cached_data.firmware_version), "%s", safe_fw);
        snprintf(cached_data.serial_number, sizeof(cached_data.serial_number), "%s", safe_serial);
        snprintf(cached_data.sku, sizeof(cached_data.sku), "%s", safe_sku);
        snprintf(cached_data.device_id, sizeof(cached_data.device_id), "%s", safe_devid);
        snprintf(cached_data.wand_type, sizeof(cached_data.wand_type), "%s", safe_type);
        xSemaphoreGive(data_mutex);
    }

    if (!running)
        return;

    char json[1024];
    snprintf(json, sizeof(json),
             "{\"type\":\"wand_info\",\"firmware\":\"%s\",\"serial\":\"%s\",\"sku\":\"%s\",\"device_id\":\"%s\",\"wand_type\":\"%s\"}",
             safe_fw, safe_serial, safe_sku, safe_devid, safe_type);
    broadcast_to_clients(server, ws_clients, &ws_client_count, client_mutex, json);

    ESP_LOGI(TAG, "Wand info broadcast: FW=%s, Serial=%s, SKU=%s, DevID=%s, Type=%s",
             safe_fw, safe_serial, safe_sku, safe_devid, safe_type);
}

void WebServer::broadcastButtonPress(bool b1, bool b2, bool b3, bool b4)
{
    if (!running)
        return;

    char json[128];
    snprintf(json, sizeof(json),
             "{\"type\":\"button_press\",\"b1\":%s,\"b2\":%s,\"b3\":%s,\"b4\":%s}",
             b1 ? "true" : "false",
             b2 ? "true" : "false",
             b3 ? "true" : "false",
             b4 ? "true" : "false");
    broadcast_to_clients(server, ws_clients, &ws_client_count, client_mutex, json);
}

void WebServer::broadcastScanResult(const char *address, const char *name, int rssi)
{
    if (!running || !address || !name)
        return;

    char sanitized_name[64];
    char sanitized_address[24];
    sanitize_for_json(sanitized_name, name, sizeof(sanitized_name));
    sanitize_for_json(sanitized_address, address, sizeof(sanitized_address));

    char json[256];
    snprintf(json, sizeof(json),
             "{\"type\":\"scan_result\",\"address\":\"%s\",\"name\":\"%s\",\"rssi\":%d}",
             sanitized_address, sanitized_name, rssi);
    broadcast_to_clients(server, ws_clients, &ws_client_count, client_mutex, json);
}

void WebServer::broadcastScanComplete()
{
    if (!running)
        return;

    const char *json = "{\"type\":\"scan_complete\"}";
    broadcast_to_clients(server, ws_clients, &ws_client_count, client_mutex, json);
    ESP_LOGI(TAG, "Scan complete broadcast");
}

// Global pointer to access BLE client from HTTP handlers
static class WandBLEClient *g_wand_client = nullptr;

void WebServer::setWandClient(WandBLEClient *client)
{
    g_wand_client = client;
}

esp_err_t WebServer::scan_handler(httpd_req_t *req)
{
    if (!g_wand_client)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "BLE client not initialized");
        return ESP_FAIL;
    }

    // Stop any ongoing scan first
    g_wand_client->stopScan();

    // Disconnect wand before scanning (if connected)
    if (g_wand_client->isConnected())
    {
        ESP_LOGI(TAG, "Disconnecting wand before scan");
        g_wand_client->setUserDisconnectRequested(true);
        g_wand_client->disconnect();
        // Give BLE stack time to process disconnect
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Start BLE scan for 10 seconds
    bool success = g_wand_client->startScan(10);

    const char *response = success ? "{\"status\":\"scanning\",\"duration\":10}" : "{\"status\":\"error\",\"message\":\"Cannot scan (already connected or scanning)\"}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

esp_err_t WebServer::set_mac_handler(httpd_req_t *req)
{
    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    // Parse JSON: {"mac":"XX:XX:XX:XX:XX:XX"}
    char mac[18] = {0};
    char *mac_start = strstr(content, "\"mac\":\"");
    if (mac_start)
    {
        mac_start += 7; // Skip "mac":"
        char *mac_end = strchr(mac_start, '"');
        if (mac_end && (mac_end - mac_start) == 17) // MAC address is 17 chars
        {
            strncpy(mac, mac_start, 17);
            mac[17] = '\0';

            // Check if MAC changed
            char old_mac[18] = {0};
            bool mac_changed = false;

            nvs_handle_t nvs_handle_read;
            esp_err_t err_read = nvs_open("storage", NVS_READONLY, &nvs_handle_read);
            if (err_read == ESP_OK)
            {
                size_t old_mac_len = sizeof(old_mac);
                if (nvs_get_str(nvs_handle_read, "wand_mac", old_mac, &old_mac_len) == ESP_OK)
                {
                    mac_changed = (strcmp(old_mac, mac) != 0);
                    ESP_LOGI(TAG, "MAC change detected: old=%s, new=%s", old_mac, mac);
                }
                else
                {
                    ESP_LOGI(TAG, "No previous MAC stored, first time setup");
                }
                nvs_close(nvs_handle_read);
            }

            // Store in NVS
            nvs_handle_t nvs_handle;
            esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
            if (err == ESP_OK)
            {
                err = nvs_set_str(nvs_handle, "wand_mac", mac);
                if (err == ESP_OK)
                {
                    nvs_commit(nvs_handle);
                    ESP_LOGI(TAG, "Stored wand MAC: %s", mac);

                    // If MAC changed, disconnect current wand and wait longer
                    if (mac_changed && g_wand_client && g_wand_client->isConnected())
                    {
                        ESP_LOGI(TAG, "MAC changed, disconnecting current wand");
                        g_wand_client->setUserDisconnectRequested(true);
                        g_wand_client->disconnect();
                        // Wait longer to ensure clean disconnect before connecting to new wand
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    }

                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"MAC address saved\"}");
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to save MAC: %d", err);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
                }
                nvs_close(nvs_handle);
            }
            else
            {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS error");
            }
            return err == ESP_OK ? ESP_OK : ESP_FAIL;
        }
    }

    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid MAC format");
    return ESP_FAIL;
}

esp_err_t WebServer::get_stored_mac_handler(httpd_req_t *req)
{
    char mac[18] = {0};
    nvs_handle_t nvs_handle;

    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK)
    {
        size_t mac_len = sizeof(mac);
        err = nvs_get_str(nvs_handle, "wand_mac", mac, &mac_len);
        nvs_close(nvs_handle);

        if (err == ESP_OK && mac[0] != '\0')
        {
            char response[128];
            snprintf(response, sizeof(response), "{\"status\":\"success\",\"mac\":\"%s\"}", mac);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, response);
            return ESP_OK;
        }
    }

    // No MAC stored
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"not_found\",\"mac\":\"\"}");
    return ESP_OK;
}

esp_err_t WebServer::connect_handler(httpd_req_t *req)
{
    if (!g_wand_client)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "BLE client not initialized");
        return ESP_FAIL;
    }

    // Read stored MAC from NVS
    char mac[18] = {0};
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK)
    {
        size_t mac_len = sizeof(mac);
        err = nvs_get_str(nvs_handle, "wand_mac", mac, &mac_len);
        nvs_close(nvs_handle);

        if (err == ESP_OK && mac[0] != '\0')
        {
            ESP_LOGI(TAG, "Attempting connection to stored MAC: %s", mac);

            // Disconnect if already connected
            if (g_wand_client->isConnected())
            {
                g_wand_client->disconnect();
                vTaskDelay(pdMS_TO_TICKS(1000));
            }

            // Reset user disconnect flag - user is explicitly requesting connection
            g_wand_client->setUserDisconnectRequested(false);

            // Mark that initialization is needed after this connection
            g_wand_client->setNeedsInitialization(true);

            // Try to connect
            bool success = g_wand_client->connect(mac);

            if (success)
            {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, "{\"status\":\"connecting\",\"message\":\"Connection initiated\"}");
                return ESP_OK;
            }
            else
            {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Connection failed\"}");
                return ESP_FAIL;
            }
        }
    }

    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No stored MAC address");
    return ESP_FAIL;
}

esp_err_t WebServer::disconnect_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "disconnect_handler called!");

    if (!g_wand_client)
    {
        ESP_LOGE(TAG, "BLE client not initialized (g_wand_client is NULL)");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "BLE client not initialized");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Wand connected status: %d", g_wand_client->isConnected());

    if (g_wand_client->isConnected())
    {
        g_wand_client->setUserDisconnectRequested(true);
        g_wand_client->disconnect();
        ESP_LOGI(TAG, "User-initiated disconnect via web interface - auto-reconnect disabled");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"disconnected\"}");
    }
    else
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"not_connected\"}");
    }

    return ESP_OK;
}

esp_err_t WebServer::settings_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "settings_get_handler called!");

    // Return mouse sensitivity and all 73 spell keycodes
#if USE_USB_HID_DEVICE
    // Allocate buffer on heap to avoid stack overflow
    char *buffer = (char *)malloc(4096);
    if (!buffer)
    {
        ESP_LOGE(TAG, "Failed to allocate buffer for settings response");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    int offset = 0;
    size_t buffer_size = 4096;

    offset += snprintf(buffer + offset, buffer_size - offset,
                       "{\"mouse_sensitivity\": %.2f, \"invert_mouse_y\": %s, \"hid_mode\": %u, \"gamepad_sensitivity\": %.2f, \"gamepad_deadzone\": %.2f, \"gamepad_invert_y\": %s, \"spells\": [",
                       usbHID.getMouseSensitivity(),
                       usbHID.getInvertMouseY() ? "true" : "false",
                       static_cast<unsigned>(usbHID.getHidMode()),
                       usbHID.getGamepadSensitivity(),
                       usbHID.getGamepadDeadzone(),
                       usbHID.getGamepadInvertY() ? "true" : "false");

    const uint8_t *spell_keycodes = usbHID.getSpellKeycodes();
    for (int i = 0; i < 73; i++)
    {
        offset += snprintf(buffer + offset, buffer_size - offset, "%d%s",
                           spell_keycodes[i],
                           i < 72 ? "," : "");
    }

    // Add HA MQTT setting
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    bool ha_mqtt_enabled = true; // Default: enabled
    char mqtt_broker[128] = {0};
    char mqtt_username[64] = {0};
    char mqtt_password[64] = {0};

    if (err == ESP_OK)
    {
        uint8_t ha_mqtt_u8 = 1;
        nvs_get_u8(nvs_handle, "ha_mqtt_enabled", &ha_mqtt_u8);
        ha_mqtt_enabled = (ha_mqtt_u8 != 0);

        size_t required_size;
        err = nvs_get_str(nvs_handle, "mqtt_broker", NULL, &required_size);
        if (err == ESP_OK && required_size <= sizeof(mqtt_broker))
        {
            nvs_get_str(nvs_handle, "mqtt_broker", mqtt_broker, &required_size);
        }

        err = nvs_get_str(nvs_handle, "mqtt_username", NULL, &required_size);
        if (err == ESP_OK && required_size <= sizeof(mqtt_username))
        {
            nvs_get_str(nvs_handle, "mqtt_username", mqtt_username, &required_size);
        }

        err = nvs_get_str(nvs_handle, "mqtt_password", NULL, &required_size);
        if (err == ESP_OK && required_size <= sizeof(mqtt_password))
        {
            nvs_get_str(nvs_handle, "mqtt_password", mqtt_password, &required_size);
        }

        nvs_close(nvs_handle);
    }

    const uint8_t *gamepad_buttons = usbHID.getSpellGamepadButtons();
    offset += snprintf(buffer + offset, buffer_size - offset, "], \"gamepad_spells\": [");
    for (int i = 0; i < 73; i++)
    {
        offset += snprintf(buffer + offset, buffer_size - offset, "%d%s",
                           gamepad_buttons[i],
                           i < 72 ? "," : "");
    }

    offset += snprintf(buffer + offset, buffer_size - offset,
                       "], \"ha_mqtt_enabled\": %s, \"mqtt_broker\": \"%s\", \"mqtt_username\": \"%s\", \"mqtt_password\": \"%s\"}",
                       ha_mqtt_enabled ? "true" : "false",
                       mqtt_broker,
                       mqtt_username,
                       mqtt_password);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buffer);
    free(buffer);
#else
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"disabled\",\"message\":\"USB HID not enabled\"}");
#endif
    return ESP_OK;
}

esp_err_t WebServer::settings_save_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "settings_save_handler called!");

    // Read the POST body
    int content_len = req->content_len;
    char *buffer = (char *)malloc(content_len + 1);
    if (!buffer)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Allocation failed");
        return ESP_FAIL;
    }

    int read_len = httpd_req_recv(req, buffer, content_len);
    if (read_len != content_len)
    {
        free(buffer);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body");
        return ESP_FAIL;
    }

    buffer[content_len] = 0;
    ESP_LOGI(TAG, "Received settings: %s", buffer);

    // Parse JSON - expect format: {"mouse_sensitivity": 1.5, "spells": [0, 58, 0, ...]}
#if USE_USB_HID_DEVICE
    float mouse_sens = 1.0f;
    char *mouse_ptr = strstr(buffer, "\"mouse_sensitivity\"");
    if (mouse_ptr)
    {
        sscanf(mouse_ptr, "\"mouse_sensitivity\": %f", &mouse_sens);
        usbHID.setMouseSensitivityValue(mouse_sens);
    }

    // Parse invert_mouse_y
    char *invert_ptr = strstr(buffer, "\"invert_mouse_y\"");
    if (invert_ptr)
    {
        bool invert = (strstr(invert_ptr, "true") != NULL);
        usbHID.setInvertMouseY(invert);
    }

    // Parse HID mode
    char *hid_mode_ptr = strstr(buffer, "\"hid_mode\"");
    if (hid_mode_ptr)
    {
        int hid_mode = HID_MODE_MOUSE;
        sscanf(hid_mode_ptr, "\"hid_mode\": %d", &hid_mode);
        usbHID.setHidMode(static_cast<HIDMode>(hid_mode));
    }

    // Parse gamepad sensitivity
    char *gpad_sens_ptr = strstr(buffer, "\"gamepad_sensitivity\"");
    if (gpad_sens_ptr)
    {
        float gpad_sens = 1.0f;
        sscanf(gpad_sens_ptr, "\"gamepad_sensitivity\": %f", &gpad_sens);
        usbHID.setGamepadSensitivityValue(gpad_sens);
    }

    // Parse gamepad deadzone
    char *gpad_deadzone_ptr = strstr(buffer, "\"gamepad_deadzone\"");
    if (gpad_deadzone_ptr)
    {
        float gpad_deadzone = 0.05f;
        sscanf(gpad_deadzone_ptr, "\"gamepad_deadzone\": %f", &gpad_deadzone);
        usbHID.setGamepadDeadzoneValue(gpad_deadzone);
    }

    // Parse gamepad invert_y
    char *gpad_invert_ptr = strstr(buffer, "\"gamepad_invert_y\"");
    if (gpad_invert_ptr)
    {
        bool invert = (strstr(gpad_invert_ptr, "true") != NULL);
        usbHID.setGamepadInvertY(invert);
    }

    // Parse HA MQTT enabled
    char *ha_mqtt_ptr = strstr(buffer, "\"ha_mqtt_enabled\"");
    if (ha_mqtt_ptr)
    {
        bool enabled = (strstr(ha_mqtt_ptr, "true") != NULL);
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK)
        {
            nvs_set_u8(nvs_handle, "ha_mqtt_enabled", enabled ? 1 : 0);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            ESP_LOGI(TAG, "HA MQTT enabled setting saved: %d (restart required)", enabled);
        }
    }

    // Parse MQTT broker URI
    char *mqtt_broker_ptr = strstr(buffer, "\"mqtt_broker\":");
    if (mqtt_broker_ptr)
    {
        mqtt_broker_ptr += strlen("\"mqtt_broker\":");
        while (*mqtt_broker_ptr == ' ')
            mqtt_broker_ptr++;
        if (*mqtt_broker_ptr == '\"')
        {
            mqtt_broker_ptr++;
            char *end_quote = strchr(mqtt_broker_ptr, '\"');
            if (end_quote)
            {
                size_t len = end_quote - mqtt_broker_ptr;
                char mqtt_broker[128] = {0};
                if (len < sizeof(mqtt_broker))
                {
                    strncpy(mqtt_broker, mqtt_broker_ptr, len);
                    nvs_handle_t nvs_handle;
                    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
                    if (err == ESP_OK)
                    {
                        nvs_set_str(nvs_handle, "mqtt_broker", mqtt_broker);
                        nvs_commit(nvs_handle);
                        nvs_close(nvs_handle);
                        ESP_LOGI(TAG, "MQTT broker saved: %s", mqtt_broker);
                    }
                }
            }
        }
    }

    // Parse MQTT username
    char *mqtt_username_ptr = strstr(buffer, "\"mqtt_username\":");
    if (mqtt_username_ptr)
    {
        mqtt_username_ptr += strlen("\"mqtt_username\":");
        while (*mqtt_username_ptr == ' ')
            mqtt_username_ptr++;
        if (*mqtt_username_ptr == '\"')
        {
            mqtt_username_ptr++;
            char *end_quote = strchr(mqtt_username_ptr, '\"');
            if (end_quote)
            {
                size_t len = end_quote - mqtt_username_ptr;
                char mqtt_username[64] = {0};
                if (len < sizeof(mqtt_username))
                {
                    strncpy(mqtt_username, mqtt_username_ptr, len);
                    nvs_handle_t nvs_handle;
                    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
                    if (err == ESP_OK)
                    {
                        nvs_set_str(nvs_handle, "mqtt_username", mqtt_username);
                        nvs_commit(nvs_handle);
                        nvs_close(nvs_handle);
                        ESP_LOGI(TAG, "MQTT username saved");
                    }
                }
            }
        }
    }

    // Parse MQTT password
    char *mqtt_password_ptr = strstr(buffer, "\"mqtt_password\":");
    if (mqtt_password_ptr)
    {
        mqtt_password_ptr += strlen("\"mqtt_password\":");
        while (*mqtt_password_ptr == ' ')
            mqtt_password_ptr++;
        if (*mqtt_password_ptr == '\"')
        {
            mqtt_password_ptr++;
            char *end_quote = strchr(mqtt_password_ptr, '\"');
            if (end_quote)
            {
                size_t len = end_quote - mqtt_password_ptr;
                char mqtt_password[64] = {0};
                if (len < sizeof(mqtt_password))
                {
                    strncpy(mqtt_password, mqtt_password_ptr, len);
                    nvs_handle_t nvs_handle;
                    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
                    if (err == ESP_OK)
                    {
                        nvs_set_str(nvs_handle, "mqtt_password", mqtt_password);
                        nvs_commit(nvs_handle);
                        nvs_close(nvs_handle);
                        ESP_LOGI(TAG, "MQTT password saved");
                    }
                }
            }
        }
    }

    // Parse spell keycodes array
    char *spells_ptr = strstr(buffer, "\"spells\":");
    if (spells_ptr)
    {
        // Skip to the opening bracket
        spells_ptr = strchr(spells_ptr, '[');
        if (spells_ptr)
        {
            extern const char *SPELL_NAMES[73];
            char *end_bracket = strchr(spells_ptr, ']');
            if (end_bracket)
            {
                int spell_idx = 0;
                const char *parse_ptr = spells_ptr + 1;

                while (spell_idx < 73 && parse_ptr < end_bracket)
                {
                    int keycode = 0;
                    int matched = sscanf(parse_ptr, "%d", &keycode);
                    if (matched == 1)
                    {
                        usbHID.setSpellKeycode(SPELL_NAMES[spell_idx], (uint8_t)keycode);
                        spell_idx++;
                        // Skip to next comma or bracket
                        parse_ptr = strchr(parse_ptr, ',');
                        if (parse_ptr)
                            parse_ptr++;
                        else
                            parse_ptr = end_bracket;
                    }
                    else
                    {
                        break;
                    }
                }
                ESP_LOGI(TAG, "Parsed %d spell keycodes", spell_idx);
            }
        }
    }

    // Parse gamepad spell button array
    char *gpad_ptr = strstr(buffer, "\"gamepad_spells\":");
    if (gpad_ptr)
    {
        gpad_ptr = strchr(gpad_ptr, '[');
        if (gpad_ptr)
        {
            extern const char *SPELL_NAMES[73];
            char *end_bracket = strchr(gpad_ptr, ']');
            if (end_bracket)
            {
                int spell_idx = 0;
                const char *parse_ptr = gpad_ptr + 1;

                while (spell_idx < 73 && parse_ptr < end_bracket)
                {
                    int button = 0;
                    int matched = sscanf(parse_ptr, "%d", &button);
                    if (matched == 1)
                    {
                        usbHID.setSpellGamepadButton(SPELL_NAMES[spell_idx], (uint8_t)button);
                        spell_idx++;
                        parse_ptr = strchr(parse_ptr, ',');
                        if (parse_ptr)
                            parse_ptr++;
                        else
                            parse_ptr = end_bracket;
                    }
                    else
                    {
                        break;
                    }
                }
                ESP_LOGI(TAG, "Parsed %d gamepad spell mappings", spell_idx);
            }
        }
    }
#endif

    // Save to NVS
#if USE_USB_HID_DEVICE
    if (usbHID.saveSettings())
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"Settings saved\"}");
        free(buffer);
        return ESP_OK;
    }
    else
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to save to NVS\"}");
        free(buffer);
        return ESP_FAIL;
    }
#else
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"disabled\",\"message\":\"USB HID not enabled\"}");
    free(buffer);
    return ESP_OK;
#endif
}

esp_err_t WebServer::settings_reset_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "settings_reset_handler called!");

#if USE_USB_HID_DEVICE
    if (usbHID.resetSettings())
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"Settings reset to defaults\"}");
        return ESP_OK;
    }
    else
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to reset settings\"}");
        return ESP_FAIL;
    }
#else
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"disabled\",\"message\":\"USB HID not enabled\"}");
    return ESP_OK;
#endif
}

esp_err_t WebServer::wifi_scan_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "wifi_scan_handler called!");

    // Get current WiFi mode
    wifi_mode_t current_mode;
    esp_err_t err = esp_wifi_get_mode(&current_mode);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get WiFi mode: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Failed to get WiFi mode\",\"networks\":[]}");
        return ESP_OK;
    }

    // If in AP-only mode, switch to APSTA mode temporarily
    bool mode_changed = false;
    if (current_mode == WIFI_MODE_AP)
    {
        ESP_LOGI(TAG, "Switching from AP to APSTA mode for scanning");
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to set APSTA mode: %s", esp_err_to_name(err));
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Failed to set scan mode\",\"networks\":[]}");
            return ESP_OK;
        }
        mode_changed = true;
    }

    // Configure scan to find all available APs
    wifi_scan_config_t scan_config = {};
    scan_config.ssid = NULL;
    scan_config.bssid = NULL;
    scan_config.channel = 0;
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 100;
    scan_config.scan_time.active.max = 300;
    scan_config.scan_time.passive = 0;

    // Start WiFi scan (blocking)
    err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));

        // Restore original mode if we changed it
        if (mode_changed)
        {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Scan failed\",\"networks\":[]}");
        return ESP_OK;
    }

    // Get number of APs found
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    ESP_LOGI(TAG, "Found %d access points", ap_count);

    if (ap_count == 0)
    {
        // Restore original mode if we changed it
        if (mode_changed)
        {
            ESP_LOGI(TAG, "Restoring AP mode");
            esp_wifi_set_mode(WIFI_MODE_AP);
        }

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":true,\"networks\":[]}");
        return ESP_OK;
    }

    // Limit to max 20 networks to avoid memory issues
    if (ap_count > 20)
    {
        ap_count = 20;
    }

    // Allocate memory for AP records
    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_records)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for AP records");

        // Restore original mode if we changed it
        if (mode_changed)
        {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Memory allocation failed\",\"networks\":[]}");
        return ESP_OK;
    }

    // Get AP records BEFORE restoring mode (mode change clears scan results!)
    err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get AP records: %s", esp_err_to_name(err));
        free(ap_records);

        // Restore original mode if we changed it
        if (mode_changed)
        {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Failed to get records\",\"networks\":[]}");
        return ESP_OK;
    }

    // Now restore original mode after getting records
    if (mode_changed)
    {
        ESP_LOGI(TAG, "Restoring AP mode");
        esp_wifi_set_mode(WIFI_MODE_AP);
    }

    // Build JSON response
    char *response = (char *)malloc(8192);
    if (!response)
    {
        free(ap_records);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Memory allocation failed\",\"networks\":[]}");
        return ESP_OK;
    }

    int offset = snprintf(response, 8192, "{\"success\":true,\"networks\":[");

    for (int i = 0; i < ap_count; i++)
    {
        const char *auth_mode = "OPEN";
        switch (ap_records[i].authmode)
        {
        case WIFI_AUTH_OPEN:
            auth_mode = "OPEN";
            break;
        case WIFI_AUTH_WEP:
            auth_mode = "WEP";
            break;
        case WIFI_AUTH_WPA_PSK:
            auth_mode = "WPA";
            break;
        case WIFI_AUTH_WPA2_PSK:
            auth_mode = "WPA2";
            break;
        case WIFI_AUTH_WPA_WPA2_PSK:
            auth_mode = "WPA/WPA2";
            break;
        case WIFI_AUTH_WPA3_PSK:
            auth_mode = "WPA3";
            break;
        case WIFI_AUTH_WPA2_WPA3_PSK:
            auth_mode = "WPA2/WPA3";
            break;
        default:
            auth_mode = "UNKNOWN";
            break;
        }

        // Escape any special characters in SSID
        char escaped_ssid[64];
        const char *src = (const char *)ap_records[i].ssid;
        char *dst = escaped_ssid;
        int max_len = sizeof(escaped_ssid) - 1;

        while (*src && max_len > 1)
        {
            if (*src == '"' || *src == '\\')
            {
                if (max_len > 2)
                {
                    *dst++ = '\\';
                    max_len--;
                }
            }
            *dst++ = *src++;
            max_len--;
        }
        *dst = '\0';

        offset += snprintf(response + offset, 8192 - offset,
                           "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\",\"channel\":%d}",
                           i > 0 ? "," : "",
                           escaped_ssid,
                           ap_records[i].rssi,
                           auth_mode,
                           ap_records[i].primary);

        if (offset >= 8000)
            break;
    }

    offset += snprintf(response + offset, 8192 - offset, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);

    free(response);
    free(ap_records);

    ESP_LOGI(TAG, "WiFi scan completed successfully with %d networks", ap_count);
    return ESP_OK;
}

esp_err_t WebServer::wifi_connect_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "wifi_connect_handler called!");

    // Read the POST body
    int content_len = req->content_len;
    char *buffer = (char *)malloc(content_len + 1);
    if (!buffer)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Allocation failed");
        return ESP_FAIL;
    }

    int read_len = httpd_req_recv(req, buffer, content_len);
    if (read_len != content_len)
    {
        free(buffer);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body");
        return ESP_FAIL;
    }

    buffer[content_len] = 0;
    ESP_LOGI(TAG, "Received WiFi connect request: %s", buffer);

    // Parse SSID and password
    char ssid[32] = {0};
    char password[64] = {0};

    char *ssid_ptr = strstr(buffer, "\"ssid\":");
    if (ssid_ptr)
    {
        ssid_ptr += strlen("\"ssid\":");
        while (*ssid_ptr == ' ')
            ssid_ptr++;
        if (*ssid_ptr == '\"')
        {
            ssid_ptr++;
            char *end_quote = strchr(ssid_ptr, '\"');
            if (end_quote)
            {
                size_t len = end_quote - ssid_ptr;
                if (len < sizeof(ssid))
                {
                    strncpy(ssid, ssid_ptr, len);
                }
            }
        }
    }

    char *password_ptr = strstr(buffer, "\"password\":");
    if (password_ptr)
    {
        password_ptr += strlen("\"password\":");
        while (*password_ptr == ' ')
            password_ptr++;
        if (*password_ptr == '\"')
        {
            password_ptr++;
            char *end_quote = strchr(password_ptr, '\"');
            if (end_quote)
            {
                size_t len = end_quote - password_ptr;
                if (len < sizeof(password))
                {
                    strncpy(password, password_ptr, len);
                }
            }
        }
    }

    // Save WiFi credentials to NVS
    if (strlen(ssid) > 0)
    {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK)
        {
            nvs_set_str(nvs_handle, "wifi_ssid", ssid);
            nvs_set_str(nvs_handle, "wifi_password", password);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            ESP_LOGI(TAG, "WiFi credentials saved to NVS: SSID=%s", ssid);
        }
    }

    // Send response indicating reboot will occur
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"WiFi credentials saved. Rebooting to apply changes...\"}");

    free(buffer);

    // Schedule reboot to apply WiFi changes
    ESP_LOGI(TAG, "WiFi configuration updated. Rebooting in 2 seconds...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;
}

esp_err_t WebServer::hotspot_settings_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "hotspot_settings_handler called!");

    // Read the POST body
    int content_len = req->content_len;
    char *buffer = (char *)malloc(content_len + 1);
    if (!buffer)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Allocation failed");
        return ESP_FAIL;
    }

    int read_len = httpd_req_recv(req, buffer, content_len);
    if (read_len != content_len)
    {
        free(buffer);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body");
        return ESP_FAIL;
    }

    buffer[content_len] = 0;
    ESP_LOGI(TAG, "Received hotspot settings: %s", buffer);

    // Parse settings
    bool enabled = (strstr(buffer, "\"enabled\":true") != NULL);
    char ssid[32] = {0};
    char password[64] = {0};
    int channel = 1;

    char *ssid_ptr = strstr(buffer, "\"ssid\":");
    if (ssid_ptr)
    {
        ssid_ptr += strlen("\"ssid\":");
        while (*ssid_ptr == ' ')
            ssid_ptr++;
        if (*ssid_ptr == '\"')
        {
            ssid_ptr++;
            char *end_quote = strchr(ssid_ptr, '\"');
            if (end_quote)
            {
                size_t len = end_quote - ssid_ptr;
                if (len < sizeof(ssid))
                {
                    strncpy(ssid, ssid_ptr, len);
                }
            }
        }
    }

    char *password_ptr = strstr(buffer, "\"password\":");
    if (password_ptr)
    {
        password_ptr += strlen("\"password\":");
        while (*password_ptr == ' ')
            password_ptr++;
        if (*password_ptr == '\"')
        {
            password_ptr++;
            char *end_quote = strchr(password_ptr, '\"');
            if (end_quote)
            {
                size_t len = end_quote - password_ptr;
                if (len < sizeof(password))
                {
                    strncpy(password, password_ptr, len);
                }
            }
        }
    }

    char *channel_ptr = strstr(buffer, "\"channel\":");
    if (channel_ptr)
    {
        sscanf(channel_ptr, "\"channel\":%d", &channel);
    }

    // Save hotspot settings to NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK)
    {
        nvs_set_u8(nvs_handle, "hotspot_enabled", enabled ? 1 : 0);
        if (strlen(ssid) > 0)
        {
            nvs_set_str(nvs_handle, "hotspot_ssid", ssid);
        }
        if (strlen(password) > 0)
        {
            nvs_set_str(nvs_handle, "hotspot_password", password);
        }
        nvs_set_u8(nvs_handle, "hotspot_channel", (uint8_t)channel);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Hotspot settings saved: enabled=%d, SSID=%s, channel=%d", enabled, ssid, channel);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Hotspot settings saved. Rebooting to apply changes...\"}");

    free(buffer);

    // Schedule reboot to apply hotspot changes
    ESP_LOGI(TAG, "Hotspot configuration updated. Rebooting in 2 seconds...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;
}

esp_err_t WebServer::hotspot_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "hotspot_get_handler called!");

    // Load hotspot settings from NVS
    char hotspot_ssid[32] = {0};
    char hotspot_password[64] = {0};
    uint8_t hotspot_channel = 6;
    bool hotspot_enabled = false;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK)
    {
        uint8_t enabled = 0;
        nvs_get_u8(nvs_handle, "hotspot_enabled", &enabled);
        hotspot_enabled = (enabled != 0);

        size_t required_size;
        err = nvs_get_str(nvs_handle, "hotspot_ssid", NULL, &required_size);
        if (err == ESP_OK && required_size > 0 && required_size <= sizeof(hotspot_ssid))
        {
            nvs_get_str(nvs_handle, "hotspot_ssid", hotspot_ssid, &required_size);
        }

        err = nvs_get_str(nvs_handle, "hotspot_password", NULL, &required_size);
        if (err == ESP_OK && required_size > 0 && required_size <= sizeof(hotspot_password))
        {
            nvs_get_str(nvs_handle, "hotspot_password", hotspot_password, &required_size);
        }

        uint8_t channel = 6;
        nvs_get_u8(nvs_handle, "hotspot_channel", &channel);
        if (channel >= 1 && channel <= 13)
        {
            hotspot_channel = channel;
        }

        nvs_close(nvs_handle);
    }

    // Build JSON response
    char response[512];
    snprintf(response, sizeof(response),
             "{\"success\":true,\"enabled\":%s,\"ssid\":\"%s\",\"password\":\"%s\",\"channel\":%d}",
             hotspot_enabled ? "true" : "false",
             hotspot_ssid,
             hotspot_password,
             hotspot_channel);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);

    return ESP_OK;
}

esp_err_t WebServer::system_reboot_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "system_reboot_handler called! Rebooting in 2 seconds...");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Rebooting device...\"}");

    // Schedule reboot after a short delay to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;
}

esp_err_t WebServer::system_wifi_mode_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "system_wifi_mode_handler called!");

    // Read request body
    char buf[100];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"No data received\"}");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Parse JSON to extract mode
    // Expected: {"mode":"client"} or {"mode":"ap"}
    bool force_ap = false;
    if (strstr(buf, "\"ap\"") != nullptr)
    {
        force_ap = true;
    }

    ESP_LOGI(TAG, "Setting force_ap_mode to %d", force_ap ? 1 : 0);

    // Save to NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK)
    {
        err = nvs_set_u8(nvs_handle, "force_ap_mode", force_ap ? 1 : 0);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to save force_ap_mode to NVS");
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    else
    {
        ESP_LOGW(TAG, "Failed to open NVS for force_ap_mode");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"WiFi mode will change after reboot\"}");

    // Schedule reboot after a short delay to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;
}

esp_err_t WebServer::system_reset_nvs_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "system_reset_nvs_handler called! Clearing ALL NVS settings...");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Resetting to defaults...\"}");

    // Allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Erase NVS partition
    esp_err_t err = nvs_flash_erase();
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "NVS erased successfully");
        // Reinitialize NVS
        err = nvs_flash_init();
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "NVS reinitialized successfully");
        }
        else
        {
            ESP_LOGW(TAG, "NVS reinit failed: %s", esp_err_to_name(err));
        }
    }
    else
    {
        ESP_LOGW(TAG, "NVS erase failed: %s", esp_err_to_name(err));
    }

    // Reboot after delay
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

esp_err_t WebServer::system_get_wifi_mode_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "system_get_wifi_mode_handler called");

    // Check force_ap_mode flag from NVS
    bool force_ap = false;
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK)
    {
        uint8_t ap_mode = 0;
        err = nvs_get_u8(nvs_handle, "force_ap_mode", &ap_mode);
        if (err == ESP_OK && ap_mode == 1)
        {
            force_ap = true;
        }
        nvs_close(nvs_handle);
    }

    // Also check current WiFi mode
    wifi_mode_t current_mode;
    esp_wifi_get_mode(&current_mode);

    char response[200];
    snprintf(response, sizeof(response),
             "{\"success\":true,\"mode\":\"%s\",\"current_wifi_mode\":\"%s\",\"force_ap\":%s}",
             force_ap ? "ap" : "client",
             current_mode == WIFI_MODE_AP ? "AP" : (current_mode == WIFI_MODE_STA ? "STA" : "APSTA"),
             force_ap ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);

    return ESP_OK;
}

// Custom 404 handler that intercepts gesture image requests
esp_err_t WebServer::gesture_404_handler(httpd_req_t *req, httpd_err_code_t error)
{
    // Check if this is a gesture image request
    const char *uri = req->uri;
    if (strncmp(uri, "/gesture/", 9) == 0)
    {
        ESP_LOGI(TAG, "404 handler intercepted gesture request: %s", uri);
        // This is a gesture request - serve the image
        return gesture_image_handler(req);
    }

    // Not a gesture request - send normal 404
    ESP_LOGW(TAG, "404 Not Found: %s", uri);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "URI not found");
    return ESP_FAIL;
}

esp_err_t WebServer::gesture_image_handler(httpd_req_t *req)
{
    // Extract spell name from URI: /gesture/<spell_name>.png
    const char *uri = req->uri;
    const char *prefix = "/gesture/";
    size_t prefix_len = strlen(prefix);

    ESP_LOGI(TAG, "Gesture request received: %s", uri);

    if (strncmp(uri, prefix, prefix_len) != 0)
    {
        ESP_LOGW(TAG, "Invalid gesture URI (missing prefix): %s", uri);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    // Get filename (spell_name.png)
    const char *filename_start = uri + prefix_len;
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "/spiffs/%s", filename_start);

    ESP_LOGI(TAG, "Looking for gesture image: %s", filepath);

    // Open file from SPIFFS
    FILE *file = fopen(filepath, "r");
    if (!file)
    {
        ESP_LOGE(TAG, "Gesture image not found: %s (errno: %d - %s)",
                 filepath, errno, strerror(errno));
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    size_t file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    ESP_LOGI(TAG, "Serving gesture image: %s (size: %zu bytes)", filepath, file_size);

    // Set content type and headers
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400"); // Cache for 1 day

    // Allocate buffer for file transfer
    char *buffer = (char *)malloc(1024);
    if (!buffer)
    {
        fclose(file);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Stream file to client
    size_t bytes_remaining = file_size;
    while (bytes_remaining > 0)
    {
        size_t chunk_size = (bytes_remaining > 1024) ? 1024 : bytes_remaining;
        size_t bytes_read = fread(buffer, 1, chunk_size, file);

        if (bytes_read > 0)
        {
            httpd_resp_send_chunk(req, buffer, bytes_read);
            bytes_remaining -= bytes_read;
        }
        else
        {
            break; // EOF or error
        }
    }

    // Finish response
    httpd_resp_send_chunk(req, NULL, 0);

    free(buffer);
    fclose(file);
    return ESP_OK;
}
