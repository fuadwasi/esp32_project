# ESP32 IoT System with JWT Authentication

A comprehensive IoT system for ESP32 that integrates temperature/humidity sensors, relay control, LCD display, MQTT communication, and a web-based configuration dashboard with JWT-based API authentication.

## Features

- **Local JWT Generation**: Generate JWT tokens locally on ESP32 with configurable secret key
- **IoT Device API Integration**: Full implementation of the IoT Manager Device API
- **Dual Sensor Support**: DHT11 and DS18B20 temperature sensors
- **8-Channel Relay Control**: Control relays via web UI, MQTT, or physical buttons
- **16x2 LCD Display**: Real-time display of sensor readings and relay status
- **MQTT Integration**: Subscribe to device-specific topics for remote control
- **WiFi Configuration Portal**: Easy WiFi setup via captive portal
- **Web Dashboard**: Configure device settings, view sensors, control relays
- **Persistent Configuration**: All settings stored in non-volatile memory
- **Heartbeat & Status Sync**: Automatic device ping and relay status synchronization

## Hardware Requirements

### Components
- ESP32 Development Board
- DHT11 Temperature & Humidity Sensor
- DS18B20 Waterproof Temperature Sensor
- 16x2 I2C LCD Display (0x27 address)
- 8-Channel Relay Module
- 4 Push Buttons (for WiFi reset, navigation, configuration, toggle)
- LED indicator (for WiFi status)

### Pin Configuration

```cpp
// I2C Display
I2C_SDA = 21
I2C_SCL = 22

// Sensors
DHT11_PIN = 4
DS18B20_PIN = 5

// Indicators & Buttons
LED_WIFI_PIN = 2
BTN_WIFI_RESET = 23  (hold 5s to reset WiFi)
BTN_NAV = 19         (navigate relay display)
BTN_CFG = 17         (enter config mode)
BTN_TOGGLE = 18      (toggle relay/clear messages)

// 8-Channel Relay Module
Relay 1 = GPIO 13
Relay 2 = GPIO 12
Relay 3 = GPIO 14
Relay 4 = GPIO 27
Relay 5 = GPIO 26
Relay 6 = GPIO 25
Relay 7 = GPIO 33
Relay 8 = GPIO 32
```

## Software Requirements

### Arduino IDE Setup

1. **Install ESP32 Board Support**
   - Open Arduino IDE
   - Go to `File` → `Preferences`
   - Add to "Additional Board Manager URLs":
     ```
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```
   - Go to `Tools` → `Board` → `Board Manager`
   - Search for "ESP32" and install "esp32 by Espressif Systems"

### Required Libraries

Install the following libraries via Arduino IDE Library Manager (`Sketch` → `Include Library` → `Manage Libraries`):

#### Core Libraries (Built-in with ESP32)
- **WiFi** - Built-in
- **WebServer** - Built-in
- **HTTPClient** - Built-in
- **Preferences** - Built-in
- **mbedtls** - Built-in (for HMAC SHA256)

#### External Libraries (Install via Library Manager)

1. **WiFiManager** by tzapu
   - Search: "WiFiManager"
   - Install: "WiFiManager by tzapu"
   - Version: 2.0.16-rc.2 or later

2. **DHT sensor library** by Adafruit
   - Search: "DHT sensor library"
   - Install: "DHT sensor library by Adafruit"
   - Also install dependency: "Adafruit Unified Sensor"

3. **OneWire** by Paul Stoffregen
   - Search: "OneWire"
   - Install: "OneWire by Paul Stoffregen"

4. **DallasTemperature** by Miles Burton
   - Search: "DallasTemperature"
   - Install: "DallasTemperature by Miles Burton"

5. **LiquidCrystal I2C** by Frank de Brabander
   - Search: "LiquidCrystal I2C"
   - Install: "LiquidCrystal I2C by Frank de Brabander"

6. **PubSubClient** by Nick O'Leary
   - Search: "PubSubClient"
   - Install: "PubSubClient by Nick O'Leary"
   - For MQTT communication

7. **ArduinoJson** by Benoit Blanchon
   - Search: "ArduinoJson"
   - Install: "ArduinoJson by Benoit Blanchon"
   - Version: 6.x or later
   - Used for JWT generation and JSON parsing

## Installation & Setup

### 1. Upload the Code

1. Open `full_code.ino` in Arduino IDE
2. Select your ESP32 board: `Tools` → `Board` → `ESP32 Dev Module`
3. Select the correct COM port: `Tools` → `Port`
4. Click `Upload` button

### 2. Initial WiFi Configuration

1. After upload, the ESP32 will create a WiFi Access Point:
   - **SSID**: `ESP32_ConfigAP`
   - **Password**: `12345678`

2. Connect to this WiFi network from your phone or computer

3. A captive portal should open automatically. If not, browse to: `http://192.168.4.1`

4. Click "Configure WiFi" and enter your WiFi credentials

5. The ESP32 will connect to your WiFi network and display the IP address on the LCD

### 3. Web Dashboard Access

Once connected to WiFi, access the web dashboard:

```
http://<ESP32_IP_ADDRESS>/
```

You will be redirected to a login page.

**Default login credentials:**
- **Username**: `admin`
- **Password**: `admin`

**Security Features:**
- Session-based authentication with 1-hour timeout
- Login page with credential validation
- Automatic redirect to login for unauthenticated users
- Logout functionality to clear session
- Session cookies for maintaining login state

**Important**: Change the default credentials immediately after first login for security!

### 4. Configure Device Settings

Navigate to the **Config** page in the web dashboard to configure:

- **Device ID**: Unique identifier for your ESP32 (e.g., `esp32-living-room`)
- **JWT Secret Key**: Secret key for JWT token encryption (default: `orbiqlabs_iot_key`)
- **Web Username/Password**: Credentials for web dashboard access
- **Sensor Interval**: How often to read and send sensor data (milliseconds)

**Important**: Change the JWT Secret Key to match your backend configuration!

## IoT Device API Integration

### API Endpoints

The system communicates with the IoT Manager API at:
```
https://iot.orbiqlabs.com/api-iot/device/
```

#### 1. Push Sensor Reading
**POST** `/api-iot/device/sensor-readings`

Sends sensor readings to the backend.

**Headers**:
- `Content-Type: application/json`
- `X-ESP32-Device-Id: <device_id>`
- `X-IOT-Token: <jwt_token>`

**Body**:
```json
{
  "sensorIdentifier": "temp-01",
  "numericValue": 24.7,
  "textValue": "24.7 °C"
}
```

#### 2. Get Device Status
**GET** `/api-iot/device/status`

Retrieves device status including relay states.

**Headers**:
- `X-ESP32-Device-Id: <device_id>`
- `X-IOT-Token: <jwt_token>`

**Response**:
```json
{
  "status": "10100110",
  "online": true,
  "lastHeartbeat": "2025-01-15T10:30:00Z"
}
```

#### 3. Submit Device Status
**POST** `/api-iot/device/status`

Updates the device relay status on the backend.

**Headers**:
- `Content-Type: application/json`
- `X-ESP32-Device-Id: <device_id>`
- `X-IOT-Token: <jwt_token>`

**Body**:
```json
{
  "status": "10100110"
}
```

#### 4. Ping Device
**POST** `/api-iot/device/ping`

Sends a heartbeat to update the device's last seen timestamp.

**Headers**:
- `X-ESP32-Device-Id: <device_id>`
- `X-IOT-Token: <jwt_token>`

### JWT Token Generation

The ESP32 generates JWT tokens **locally** with the following specifications:

- **Algorithm**: HS256 (HMAC SHA256)
- **Expiration**: 30 minutes
- **Payload**:
  ```json
  {
    "deviceId": "your-device-id",
    "iat": 1705320600,
    "exp": 1705322400
  }
  ```

Tokens are automatically regenerated before expiration to ensure continuous authentication.

## MQTT Configuration

The system connects to an MQTT broker for real-time communication:

```cpp
MQTT_BROKER = "103.181.42.126"
MQTT_PORT = 1883
MQTT_USERNAME = "fuad"
MQTT_PASSWORD = "orbiqlabsMqtt159"
```

### MQTT Topics

**Subscribe to**: `devices/{device_id}/state`

The device listens for JSON messages with the following format:
```json
{
  "t": "Type",
  "d": "Data"
}
```

#### Message Types

1. **Status** - Update relay states
   ```json
   {
     "t": "Status",
     "d": "10100110"
   }
   ```

2. **Message** - Display custom message on LCD
   ```json
   {
     "t": "Message",
     "d": "Hello from cloud!"
   }
   ```

3. **Ping** - Trigger immediate sensor reading and send ping to API
   ```json
   {
     "t": "Ping",
     "d": null
   }
   ```
   When a Ping message is received via MQTT:
   - Triggers immediate sensor reading (bypasses the interval timer)
   - Sends a ping request to the backend API endpoint
   - Updates the device heartbeat timestamp on the backend
   
   **Note**: Ping is ONLY sent when requested via MQTT, not automatically on a timer.

4. **Sync** - Device synchronization
   ```json
   {
     "t": "Sync",
     "d": "sync_data"
   }
   ```

## Button Controls

### WiFi Reset Button (GPIO 23)
- **Press and hold for 5 seconds** to reset WiFi settings
- Device will reboot and enter WiFi configuration mode

### Navigation Button (GPIO 19)
- Press to enter **Navigation Mode**
- Cycles through individual relay status on LCD
- Auto-exits after 30 seconds

### Config Button (GPIO 17)
- Toggle **Configuration Mode**
- Enables relay toggling via the Toggle button

### Toggle Button (GPIO 18)
- In Config Mode: Toggle the currently focused relay
- With Custom Message: Clear the custom message from LCD

## LCD Display Modes

### Normal Mode
Cycles every 4 seconds through:
1. DHT11 Temperature & Humidity
2. DS18B20 Temperature
3. Relay Status (8-bit binary)

### Navigation Mode
Shows individual relay status:
```
Relay 1
ON
```

### Custom Message Mode
Displays MQTT custom messages:
```
MSG:
Hello from cloud!
```

## Automatic Functions

### Sensor Reading (every 30s)
- Reads DHT11 temperature and humidity
- Reads DS18B20 temperature
- Posts readings to backend API
- Buffers failed readings for retry

### Relay Status Sync (every 120s)
- Fetches current relay states from backend
- Updates local relay states to match backend

### MQTT-Triggered Ping
- Ping is only sent when requested via MQTT "Ping" message
- Updates device heartbeat on backend
- No automatic periodic ping (only on-demand)

## Troubleshooting

### WiFi Connection Issues
1. Hold WiFi Reset button for 5 seconds
2. Reconnect to `ESP32_ConfigAP` and reconfigure

### LCD Not Displaying
- Check I2C connections (SDA=21, SCL=22)
- Verify LCD address is 0x27 (adjust in code if different)

### Sensors Returning NaN
- Check sensor wiring and power
- Ensure correct pin assignments
- For DS18B20, a 4.7kΩ pull-up resistor may be needed

### API Calls Failing
- Verify Device ID matches backend registration
- Check JWT Secret Key matches backend configuration
- Ensure ESP32 has internet connectivity
- Check Serial Monitor for error messages

### MQTT Not Connecting
- Verify MQTT broker IP and port
- Check username and password
- Ensure network allows outbound connection to port 1883

## Serial Monitor Output

Enable Serial Monitor at **115200 baud** to view:
- WiFi connection status
- API request/response logs
- MQTT connection status
- Sensor readings
- JWT token generation
- Button press events
- Error messages

Example output:
```
Starting ESP32 IoT system...
✅ WiFi connected
SSID: MyHomeWiFi
IP: 192.168.1.100
✅ MQTT connected!
Subscribed to topic: devices/esp32-living-room/state
Generated JWT: eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...
Reading sensors...
DHT11_Temperature: 24.50 °C
DHT11_Humidity: 60 %
DS18B20 Temperature: 23.87 °C
Posting sensor reading to: https://iot.orbiqlabs.com/api-iot/device/sensor-readings
Successfully posted: DHT11_Temperature
```

## Security Considerations

1. **Change Default Passwords**: Update web dashboard credentials immediately after first login
2. **Session Management**: Web sessions automatically expire after 1 hour of inactivity
3. **Logout**: Always logout when finished to clear your session
4. **Secure JWT Secret**: Use a strong, unique secret key for JWT generation
5. **HTTPS**: Backend API uses HTTPS for secure communication
6. **WiFi Security**: Use WPA2 encryption for your WiFi network
7. **MQTT Authentication**: MQTT connection uses username/password authentication

## Web Dashboard Features

### Login System
- Secure login page with username/password authentication
- Session-based authentication (1-hour timeout)
- Automatic redirect for unauthenticated access
- Logout button on all authenticated pages
- Session cookies for maintaining login state

### Available Pages
- **Home** (`/`) - Navigation hub with links to all sections
- **Live Sensors** (`/sensors`) - Real-time sensor readings
- **Relays** (`/relays`) - Control all 8 relays with checkboxes
- **Config** (`/config`) - Configure device settings
- **Login** (`/login`) - Authentication page
- **Logout** (`/logout`) - Clear session and logout

## License

This project is provided as-is for educational and personal use.

## Support

For issues or questions:
- Check Serial Monitor output for debugging
- Verify all libraries are correctly installed
- Ensure pin connections match the configuration
- Review the Postman collection for API reference

## Version History

- **v1.0** - Initial release with JWT authentication and IoT Device API integration
