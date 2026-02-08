# MQTT Troubleshooting Guide

## Changes Made

### 1. **Fixed Discovery Topic**
- Changed from `homeassistant/event/wand_spell/config` to `homeassistant/sensor/wand_spell/config`
- The `event` component type doesn't work properly for spell data
- Using `sensor` component allows state tracking in Home Assistant

### 2. **Added Comprehensive Debug Logging**
All MQTT operations now show detailed logs:
- **Connection**: Shows broker URI, username, network status
- **Publishing**: Shows topic, payload, QoS, msg_id
- **Published confirmation**: `MQTT_EVENT_PUBLISHED` confirms message was acknowledged by broker
- **Subscriptions**: Confirms when subscriptions are successful
- **Received data**: Shows any data received from broker

### 3. **Added Connectivity Test**
- ESP32 now subscribes to `wand/test` topic on connect
- This verifies bidirectional MQTT communication
- You can test by publishing to this topic from Home Assistant

## How to Test

### Step 1: Recompile and Flash
```bash
./build-s3.sh flash monitor
```

### Step 2: Watch Serial Output
After connecting to WiFi, you should see:
```
I (xxxxx) ha_mqtt: ✓ Connected to MQTT broker
I (xxxxx) ha_mqtt: 📤 Publishing discovery to: homeassistant/sensor/wand_spell/config
I (xxxxx) ha_mqtt: 📤 Discovery payload: {"name":"Magic Wand Spell",...}
I (xxxxx) ha_mqtt:    Spell discovery msg_id: 1
I (xxxxx) ha_mqtt: 📤 Publishing discovery to: homeassistant/sensor/wand_battery/config
I (xxxxx) ha_mqtt: 📤 Discovery payload: {"name":"Wand Battery",...}
I (xxxxx) ha_mqtt:    Battery discovery msg_id: 2
I (xxxxx) ha_mqtt: 📥 Subscribed to wand/test for connectivity verification [msg_id=3]
I (xxxxx) ha_mqtt: ✓ MQTT subscription successful [msg_id=3]
```

### Step 3: Test Bidirectional Communication
In Home Assistant, go to **Developer Tools** → **MQTT**:

1. **Publish a test message:**
   - Topic: `wand/test`
   - Payload: `hello`
   - Click "PUBLISH"

2. **Check ESP32 serial output:**
   ```
   I (xxxxx) ha_mqtt: 📥 MQTT data received on topic: wand/test
   I (xxxxx) ha_mqtt:    Payload: hello
   ```

If you see this, MQTT is working bidirectionally! ✓

### Step 4: Cast a Spell
Wave your wand and cast a spell. You should see:
```
I (xxxxx) main: 🎯 Spell detected in callback - processing...
I (xxxxx) main:   → Checking MQTT connection (isConnected=1)
I (xxxxx) main:   → Calling mqttClient.publishSpell()
I (xxxxx) ha_mqtt: publishSpell() called: spell_name='Incendio', confidence=0.994
I (xxxxx) ha_mqtt:   Connection status: connected=1, mqtt_client=0x...
I (xxxxx) ha_mqtt:   📤 Publishing to topic 'wand/spell'
I (xxxxx) ha_mqtt:   📤 Payload: {"spell":"Incendio","confidence":0.994}
I (xxxxx) ha_mqtt:   📤 QoS: 1, Retain: false
I (xxxxx) ha_mqtt:   ✓ Published spell: Incendio (99.4%) [msg_id=XXX]
I (xxxxx) ha_mqtt: ✓ MQTT message published successfully [msg_id=XXX]
```

The **✓ MQTT message published successfully** line is critical - it means the broker acknowledged receipt!

### Step 5: Listen in Home Assistant
In Home Assistant **Developer Tools** → **MQTT**:
- Topic: `wand/spell`
- Click "START LISTENING"

Cast a spell - you should immediately see:
```json
{"spell":"Incendio","confidence":0.994}
```

## Common Issues

### Issue 1: No "MQTT message published successfully" Event
**Symptom:** You see "Published spell" but no "✓ MQTT message published successfully"

**Causes:**
- QoS 1 requires broker acknowledgment
- Broker might not be acknowledging messages
- Check Home Assistant MQTT broker logs

**Solution:**
```bash
# In Home Assistant container or OS
docker logs homeassistant 2>&1 | grep -i mqtt
# or
journalctl -u hassio-supervisor -f | grep -i mqtt
```

### Issue 2: Discovery Not Working
**Symptom:** No "Magic Wand Gateway" device appears in Home Assistant

**Check:**
1. Home Assistant MQTT integration is enabled
2. Discovery is enabled in MQTT integration settings
3. ESP32 logs show discovery messages published (msg_id > 0)

**Manual verification:**
```bash
# Subscribe to discovery topics in HA Developer Tools → MQTT
Topic: homeassistant/sensor/wand_spell/config
```

You should see the discovery payload appear immediately after ESP32 connects.

### Issue 3: Messages Not Appearing in HA
**Symptom:** ESP32 shows "published successfully" but HA sees nothing

**Debug steps:**

1. **Check MQTT broker is running:**
   ```bash
   # From Linux machine on same network
   nc -zv 192.168.2.29 1883
   ```

2. **Use mosquitto_sub to listen:**
   ```bash
   mosquitto_sub -h 192.168.2.29 -p 1883 -u magicwandesp32 -P wlqJQtAfLvcYK5 -t 'wand/#' -v
   ```
   This bypasses Home Assistant and listens directly to broker.

3. **Check Home Assistant MQTT integration:**
   - Settings → Devices & Services → MQTT
   - Should show "Connected"
   - Check broker address matches: `192.168.2.29`

4. **Check MQTT broker logs:**
   If using Mosquitto add-on in HA:
   - Settings → Add-ons → Mosquitto broker → Log tab

### Issue 4: Wrong Broker Configuration
**Check your NVS settings match:**
```
MQTT broker: mqtt://192.168.2.29:1883  (or just "192.168.2.29" - code adds mqtt:// prefix)
Username: magicwandesp32
Password: wlqJQtAfLvcYK5
```

Verify in ESP32 logs:
```
I (xxxxx) main: MQTT Configuration:
I (xxxxx) main:   Broker: mqtt://192.168.2.29:1883
I (xxxxx) main:   Username: magicwandesp32
```

## Expected Behavior

After these changes, you should see:

1. ✅ MQTT connects successfully
2. ✅ Discovery messages published (msg_id > 0)
3. ✅ Subscription to wand/test successful
4. ✅ Test message from HA appears in ESP32 logs
5. ✅ Spell published with positive msg_id
6. ✅ "MQTT message published successfully" confirmation
7. ✅ Spell appears in HA when listening to wand/spell

## Home Assistant Configuration

After successful connection, check for the auto-discovered entity:

**Settings** → **Devices & Services** → **MQTT** → Look for:
- Device: "Magic Wand Gateway"
- Entity: `sensor.magic_wand_spell`
- Entity: `sensor.wand_battery`

Create a simple automation to test:
```yaml
alias: Test Wand MQTT
trigger:
  - platform: mqtt
    topic: wand/spell
action:
  - service: notify.persistent_notification
    data:
      title: "🪄 Wand Spell Detected!"
      message: "{{ trigger.payload_json.spell }} ({{ (trigger.payload_json.confidence * 100) | round }}%)"
```

## Next Steps

If all tests pass but you still don't see data in Home Assistant:
1. Check Home Assistant logs for MQTT errors
2. Verify MQTT integration configuration in HA
3. Try restarting Home Assistant after first discovery message
4. Check if HA MQTT discovery is enabled:
   ```yaml
   # In configuration.yaml
   mqtt:
     discovery: true
   ```
