#include "web_server.h"
#include "config.h"
#include "esp_log.h"
#include "ble_client.h"
#include "usb_hid.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

// Forward declaration from main.cpp
#if USE_USB_HID_DEVICE
extern USBHIDManager usbHID;
#endif

static const char *TAG = "web_server";

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
                    </div>
                </div>
            </div>
            <div style="margin-top: 15px;">
                <button class="button" onclick="saveSettings()">💾 Save Settings</button>
                <button class="button secondary" onclick="loadSettings()">🔄 Load Settings</button>
                <button class="button danger" onclick="resetSettings()">🔁 Reset to Defaults</button>
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
            
            // Draw center crosshair
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
            })
            .catch(error => {
                console.error('Failed to save MAC:', error);
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
                alert('Please select a wand first');
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
        
        // Spell names list (73 spells - must match server-side SPELL_NAMES array)
        const SPELL_NAMES = [
            "The_Force_Spell", "Colloportus", "Colloshoo", "The_Hour_Reversal_Reversal_Charm", "Evanesco", 
            "Herbivicus", "Orchideous", "Brachiabindo", "Meteolojinx", "Riddikulus", "Silencio", "Immobulus", 
            "Confringo", "Petrificus_Totalus", "Flipendo", "The_Cheering_Charm", "Salvio_Hexia", "Pestis_Incendium", 
            "Alohomora", "Protego", "Langlock", "Mucus_Ad_Nauseum", "Flagrate", "Glacius", "Finite", "Anteoculatia", 
            "Expelliarmus", "Expecto_Patronum", "Descendo", "Depulso", "Reducto", "Colovaria", "Aberto", "Confundo", 
            "Densaugeo", "The_Stretching_Jinx", "Entomorphis", "The_Hair_Thickening_Growing_Charm", "Bombarda", 
            "Finestra", "The_Sleeping_Charm", "Rictusempra", "Piertotum_Locomotor", "Expulso", "Impedimenta", 
            "Ascendio", "Incarcerous", "Ventus", "Revelio", "Accio", "Melefors", "Scourgify", "Wingardium_Leviosa", 
            "Nox", "Stupefy", "Spongify", "Lumos", "Appare_Vestigium", "Verdimillious", "Fulgari", "Reparo", 
            "Locomotor", "Quietus", "Everte_Statum", "Incendio", "Aguamenti", "Sonorus", "Cantis", "Arania_Exumai", 
            "Calvorio", "The_Hour_Reversal_Charm", "Vermillious", "The_Pepper-Breath_Hex"
        ];

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

        function filterSpellMappings() {
            const input = document.getElementById('spell-filter');
            const filter = input.value.trim().toLowerCase();
            const items = document.querySelectorAll('.spell-mapping-item');
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
        
        // Load and save settings with spell mappings
        function saveSettings() {
            const settings = {
                mouse_sensitivity: parseFloat(document.getElementById('mouse-sensitivity').value),
                spells: []
            };
            
            // Collect all spell keycode mappings
            for (let i = 0; i < SPELL_NAMES.length; i++) {
                const select = document.getElementById(`spell_${i}`);
                settings.spells.push(parseInt(select.value));
            }
            
            fetch('/settings/save', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(settings)
            })
            .then(response => response.json())
            .then(data => {
                alert('✓ Settings saved successfully!');
                console.log('Settings saved:', data);
            })
            .catch(error => {
                alert('✗ Failed to save settings');
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
                    
                    // Load spell keycodes
                    if (data.spells && data.spells.length === SPELL_NAMES.length) {
                        for (let i = 0; i < data.spells.length; i++) {
                            const select = document.getElementById(`spell_${i}`);
                            if (select) {
                                select.value = data.spells[i];
                            }
                        }
                    }
                    alert('✓ Settings loaded from device');
                })
                .catch(error => {
                    alert('✗ Failed to load settings');
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
                        document.getElementById('mouse-sensitivity').value = 1.0;
                        document.getElementById('sens-value').textContent = '1.0x';
                        alert('✓ Settings reset to defaults!');
                    })
                    .catch(error => {
                        alert('✗ Failed to reset settings');
                        console.error('Reset error:', error);
                    });
            }
        }
        
        // Initialize UI
        populateSpellMappings();
        
        // Load settings on page load
        setTimeout(loadSettings, 2000);
        
        // Load stored MAC on page load
        setTimeout(loadStoredMac, 1000);
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

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_open_sockets = 7;
    config.max_uri_handlers = 15; // Support 12 handlers + buffer for future endpoints
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

    running = true;
    ESP_LOGI(TAG, "Web server started on port %d", port);
    ESP_LOGI(TAG, "Registered endpoints: /, /ws, /generate_204, /hotspot-detect.html, /scan, /set_mac, /get_stored_mac, /connect, /disconnect, /settings/get, /settings/save, /settings/reset");
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
    // Cache the wand info for new clients
    if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        snprintf(cached_data.firmware_version, sizeof(cached_data.firmware_version), "%s", firmware_version ? firmware_version : "");
        snprintf(cached_data.serial_number, sizeof(cached_data.serial_number), "%s", serial_number ? serial_number : "");
        snprintf(cached_data.sku, sizeof(cached_data.sku), "%s", sku ? sku : "");
        snprintf(cached_data.device_id, sizeof(cached_data.device_id), "%s", device_id ? device_id : "");
        snprintf(cached_data.wand_type, sizeof(cached_data.wand_type), "%s", wand_type ? wand_type : "");
        xSemaphoreGive(data_mutex);
    }

    if (!running)
        return;

    char json[1024];
    snprintf(json, sizeof(json),
             "{\"type\":\"wand_info\",\"firmware\":\"%s\",\"serial\":\"%s\",\"sku\":\"%s\",\"device_id\":\"%s\",\"wand_type\":\"%s\"}",
             firmware_version ? firmware_version : "",
             serial_number ? serial_number : "",
             sku ? sku : "",
             device_id ? device_id : "",
             wand_type ? wand_type : "");
    broadcast_to_clients(server, ws_clients, &ws_client_count, client_mutex, json);

    ESP_LOGI(TAG, "Wand info broadcast: FW=%s, Serial=%s, SKU=%s, DevID=%s, Type=%s",
             firmware_version ? firmware_version : "--",
             serial_number ? serial_number : "--",
             sku ? sku : "--",
             device_id ? device_id : "--",
             wand_type ? wand_type : "--");
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

    char json[256];
    snprintf(json, sizeof(json),
             "{\"type\":\"scan_result\",\"address\":\"%s\",\"name\":\"%s\",\"rssi\":%d}",
             address, name, rssi);
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

    // Disconnect wand before scanning (if connected)
    if (g_wand_client->isConnected())
    {
        ESP_LOGI(TAG, "Disconnecting wand before scan");
        g_wand_client->disconnect();
        // Give BLE stack time to process disconnect
        vTaskDelay(pdMS_TO_TICKS(500));
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
    char buffer[2048];
    int offset = 0;

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "{\"mouse_sensitivity\": %.2f, \"spells\": [",
                       usbHID.getMouseSensitivity());

    const uint8_t *spell_keycodes = usbHID.getSpellKeycodes();
    for (int i = 0; i < 73; i++)
    {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%d%s",
                           spell_keycodes[i],
                           i < 72 ? "," : "");
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buffer);
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
