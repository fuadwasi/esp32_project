# MQTT Implementation for ESP32 IoT System

## Summary of Changes

This document describes the modifications made to replace HTTP API relay control with MQTT-based control.

## What Was Changed

### 1. **Added MQTT Support**
- Included `PubSubClient` library for MQTT communication
- Added MQTT broker configuration:
  - Broker: `103.181.42.126`
  - Port: `1883`
  - Username: `fuad`
  - Password: `orbiqlabsMqtt159`

### 2. **MQTT Message Format**
The system now handles JSON messages from the MQTT broker in this format:
```json
{"t":"MessageType","d":"Data"}
```

**Supported Message Types:**

#### `Status` - Relay Control
- **Example:** `{"t":"Status","d":"01001001"}`
- **Function:** Controls 8 relays (0=OFF, 1=ON)
- The data field contains 8 characters representing each relay state

#### `Message` - LCD Display
- **Example:** `{"t":"Message","d":"E56379"}`
- **Function:** Displays custom message on LCD
- When a message is received:
  - LCD shows `MSG:` on line 1
  - Displays the message content on line 2
  - Normal sensor cycling stops until user presses `BTN_TOGGLE`

#### `Ping` - Trigger Sensor Reading
- **Example:** `{"t":"Ping","d":null}`
- **Function:** Immediately triggers a sensor reading and sends data to HTTP API
- Forces the sensor reading cycle to run immediately

#### `Sync` - Synchronization
- **Example:** `{"t":"Sync","d":"434242"}` or `{"t":"Sync","d":null}`
- **Function:** Reserved for device sync operations
- Currently logged but not actively used

### 3. **MQTT Topic Subscription**
- Topic pattern: `devices/{device_id}/state`
- Example: If `device_id` is "esp32-device-001", it subscribes to: `devices/esp32-device-001/state`

### 4. **Custom Message Mode**
- When a `Message` type MQTT event is received:
  - LCD stops cycling through sensor data
  - Displays the custom message
  - Continues showing the message until user presses `BTN_TOGGLE`
  - After clearing, normal sensor cycling resumes

### 5. **HTTP API Changes**
- **Kept:** `postSensorReadings()` - Still sends sensor data via HTTP API
- **Removed:** `fetchRelayStateFromBackend()` - No longer fetches relay states via HTTP
- **Removed:** `postRelayUpdateToBackend()` - No longer posts relay updates via HTTP
- Relay control is now exclusively via MQTT

### 6. **Button Behavior Updates**
- `BTN_TOGGLE` now has dual function:
  - **In custom message mode:** Clears the message and returns to normal display
  - **In config mode:** Still toggles relays (local control only, not sent to backend)

## Testing the MQTT Implementation

### Subscribe to Device Topic (View Messages)
```bash
mosquitto_sub -h 103.181.42.126 -u fuad -P orbiqlabsMqtt159 -t "devices/{your_device_id}/state"
```

### Publish Test Messages

#### Test Relay Control
```bash
mosquitto_pub -h 103.181.42.126 -u fuad -P orbiqlabsMqtt159 -t "devices/{your_device_id}/state" -m '{"t":"Status","d":"11110000"}'
```
This will turn ON relays 1-4 and turn OFF relays 5-8.

#### Test Custom LCD Message
```bash
mosquitto_pub -h 103.181.42.126 -u fuad -P orbiqlabsMqtt159 -t "devices/{your_device_id}/state" -m '{"t":"Message","d":"Hello World!"}'
```
This will display "Hello World!" on the LCD. Press `BTN_TOGGLE` to clear.

#### Test Ping (Trigger Sensor Reading)
```bash
mosquitto_pub -h 103.181.42.126 -u fuad -P orbiqlabsMqtt159 -t "devices/{your_device_id}/state" -m '{"t":"Ping","d":null}'
```
This will immediately trigger a sensor reading and send data to the HTTP API.

## Required Library

Make sure to install the **PubSubClient** library in Arduino IDE:
1. Go to `Sketch` → `Include Library` → `Manage Libraries...`
2. Search for "PubSubClient"
3. Install the library by Nick O'Leary

## Configuration

Update the following in the code if needed:
- `MQTT_BROKER`: IP address of your MQTT broker
- `MQTT_PORT`: MQTT broker port (default: 1883)
- `MQTT_USERNAME`: MQTT authentication username
- `MQTT_PASSWORD`: MQTT authentication password
- `device_id`: Unique device identifier (can be changed in web config)

## Connection Flow

1. ESP32 connects to WiFi via WiFiManager
2. Connects to MQTT broker
3. Subscribes to `devices/{device_id}/state` topic
4. Listens for incoming messages
5. Maintains MQTT connection with auto-reconnect
6. Continues sending sensor data via HTTP API periodically
7. Responds to Ping messages by immediately sending sensor data

## Notes

- The HTTP API is still used for **sensor data transmission only**
- All relay control is now via **MQTT only**
- Web interface relay control works locally but doesn't sync to backend
- MQTT connection is maintained with automatic reconnection if lost
- Custom messages on LCD can be cleared by pressing `BTN_TOGGLE`
