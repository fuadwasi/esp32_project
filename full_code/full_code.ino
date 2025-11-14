/*
  ESP32 IoT System - Single-file sketch
  Version: 1.0.7
  Last Updated: 2025-11-14
  
  Changelog:
  - v1.0.7 (2025-11-14):
    * Implemented sensor data storage and sync system
    * Added LittleFS support for local data storage
    * Sensor readings now saved to /sensor_data.json instead of immediate API sending
    * Added 5-minute periodic data sync to API
    * MQTT Sync message ({"t":"Sync","d":null}) triggers immediate data transmission
    * MQTT Ping message now also sends stored sensor data
    * Startup check for existing stored data and automatic transmission
    * Improved data resilience during network outages
  - v1.0.6 (2025-11-13):
    * Added NTP time synchronization for accurate LCD time display
    * Redesigned relay page with iOS/Android-style toggle switches
    * Implemented instant AJAX relay control (no save button needed)
    * Updated LCD display format: Line 1 shows time + WiFi signal, Line 2 shows DS18B20 temp + relay status
    * Removed DHT11 temperature from LCD display
    * Added IP address display in LCD relay/sensor loop
    * Enhanced temperature bar gradients with -40°C to 60°C range (Blue→Green→Yellow→Orange→Red)
    * Added time.h include for NTP functionality
  - v1.0.4 (2025-11-13):
    * CRITICAL FIX: Corrected relay logic for Active LOW relay modules
    * Added RELAY_ACTIVE_LOW configuration constant
    * Created setRelayState() helper function for proper relay control
    * Fixed inverted relay states (server "1" now correctly turns relay ON)
    * Added auto-reload functionality on sensors page (5-second countdown)
  - v1.0.3 (2025-11-09):
    * Improved UI/UX with modern input field styling
    * Added beautiful logout button with icon and hover effects
    * Made Device ID read-only (cannot be changed from admin panel)
    * Added visual bar graph representations on sensors page
    * Enhanced form styling with proper labels and spacing
  - v1.0.2 (2025-11-09):
    * Added 5-minute auto-timeout for MQTT Info messages on LCD
    * LCD automatically returns to normal display after 5 minutes
  - v1.0.1 (2025-11-09): 
    * Changed /device/status sync to event-based (startup, WiFi reconnect, MQTT reconnect)
    * Removed periodic 120s relay status sync
    * Fixed login redirect using JavaScript instead of 302 redirect for better cookie handling
    * Added nested JSON parsing for MQTT Sync messages with Info type
  - v1.0.0 (2025-11-09): Initial release with JWT authentication, MQTT, web dashboard
  
  Implements core requirements described by the user.
  Libraries required:
    - WiFiManager (tzapu/WiFiManager)
    - Preferences (built-in)
    - WebServer (built-in)
    - HTTPClient (built-in)
    - DHT sensor library (Adafruit or simple DHT)
    - OneWire
    - DallasTemperature
    - LiquidCrystal_I2C
    - PubSubClient (for MQTT)
    - ArduinoJson (bblanchon/ArduinoJson) - for JWT generation
    - mbedtls (built-in) - for HMAC SHA256
    - LittleFS (built-in) - for sensor data storage
  
  Pin assignments (change if needed):
    - I2C SDA = 21, SCL = 22
    - DHT11_PIN = 4
    - DS18B20_PIN = 5
    - LED_WIFI_PIN = 2
    - Buttons: BTN_WIFI_RESET = 23, BTN_NAV = 19, BTN_CFG = 17, BTN_TOGGLE = 18
    - Relays: relayPins[8] = {12, 13, 14, 27, 26, 25, 33, 32}
*/

#define FIRMWARE_VERSION "1.0.7"

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <mbedtls/md.h>
#include <base64.h>

#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>
#include <FS.h>
#include <LittleFS.h>

// ---------- CONFIG ----------
// Sensor Identifiers (unique for each sensor type)
#define SENSOR_DHT11_TEMP "dht11-temperature"
#define SENSOR_DHT11_HUMIDITY "dht11-humidity"
#define SENSOR_DS18B20_TEMP "ds18b20-temperature"
#define BACKEND_BASE_URL "https://iot.orbiqlabs.com" // IoT Device API base URL
#define SENSOR_POST_ENDPOINT "/api-iot/device/sensor-readings"
#define GET_STATUS_ENDPOINT "/api-iot/device/status"
#define POST_STATUS_ENDPOINT "/api-iot/device/status"
#define PING_ENDPOINT "/api-iot/device/ping"

#define DEVICE_ID_DEFAULT "esp32-default-device"
#define JWT_SECRET_DEFAULT "O0htANStQl1bGUJV"
#define JWT_EXPIRATION_MINUTES 30

// MQTT Configuration
#define MQTT_BROKER "103.181.42.126"
#define MQTT_PORT 1883
#define MQTT_USERNAME "fuad"
#define MQTT_PASSWORD "orbiqlabsMqtt159"

// Pins
const int I2C_SDA = 21;
const int I2C_SCL = 22;
const int DHT11_PIN = 4;
const int DS18B20_PIN = 5;
const int LED_WIFI_PIN = 2;

const int BTN_WIFI_RESET = 18; // input only (use with pullup)
const int BTN_OK = 17; // OK button for Info messages (input with pullup)
const int BUZZER_PIN = 19; // buzzer pin (active high)

// Relay pins (change to match your wiring)
const int relayPins[8] = {12, 13, 14, 27, 26, 25, 33, 32};

// Relay module type: true = Active LOW (most common), false = Active HIGH
// Active LOW means: LOW = relay ON, HIGH = relay OFF
const bool RELAY_ACTIVE_LOW = true;

// Sensor intervals (ms)
unsigned long sensorInterval = 60UL * 1000UL; // default 1 minute; configurable
unsigned long lastSensorMillis = 0;

// Data sync intervals (ms)
unsigned long dataSyncInterval = 5UL * 60UL * 1000UL; // 5 minutes
unsigned long lastDataSyncMillis = 0;

// File paths for data storage
#define SENSOR_DATA_FILE "/sensor_data.json"

// WiFi and MQTT connection state tracking
bool wasWiFiConnected = false;
bool wasMqttConnected = false;

// MQTT reconnection backoff
unsigned long lastMqttReconnectAttempt = 0;
const unsigned long MQTT_RECONNECT_INTERVAL = 5000UL; // 5 seconds minimum between attempts

// DHT
#define DHTTYPE DHT11
DHT dht(DHT11_PIN, DHTTYPE);

// DS18B20
OneWire oneWire(DS18B20_PIN);
DallasTemperature ds18b20(&oneWire);

// LCD (address 0x27 typical)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// MQTT Client
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// Web server
WebServer server(80);
WiFiManager wifiManager;
Preferences prefs;

// Device state
String device_id = DEVICE_ID_DEFAULT;
String jwtSecretKey = JWT_SECRET_DEFAULT;
String apiUsername = "admin";
String apiPassword = "admin";
String webUsername = "admin";
String webPassword = "admin";

String cachedJWT = "";
unsigned long jwtGeneratedAt = 0;

// Relay states (0=off,1=on)
int relayState[8] = {0,0,0,0,0,0,0,0};

// MQTT custom message handling
bool customMessageMode = false;
String customMessage = "";
unsigned long customMessageReceivedAt = 0;
const unsigned long CUSTOM_MESSAGE_TIMEOUT = 5UL * 60UL * 1000UL; // 5 minutes

// Info message handling (pauses LCD cycling until OK button pressed)
bool infoMessageMode = false;
String infoMessage = "";
unsigned long infoMessageReceivedAt = 0;

// Buttons debounce & press detection
const unsigned long WIFI_RESET_HOLD_MS = 5000; // 5 seconds

// Sensor buffer for retrying posts
struct SensorReading {
  String name;
  float value;
  String unit;
};

// Device status struct for API responses
struct DeviceStatus {
  String status;
  bool online;
  bool success;
};

// Session management
String sessionToken = "";
unsigned long sessionCreatedAt = 0;
const unsigned long SESSION_TIMEOUT = 3600UL * 1000UL; // 1 hour session timeout

// Generate formatted timestamp for logging
String getTimestamp() {
  struct tm timeinfo;
  char timestamp[20];
  if (getLocalTime(&timeinfo)) {
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(timestamp);
  } else {
    return "0000-00-00 00:00:00";
  }
}

// Generate simple session token
String generateSessionToken() {
  String token = "";
  for (int i = 0; i < 32; i++) {
    token += String(random(0, 16), HEX);
  }
  return token;
}

bool isSessionValid(const String &token) {
  if (token.length() == 0 || sessionToken.length() == 0) {
    return false;
  }
  if (token != sessionToken) {
    return false;
  }
  
  // Check if session has expired
  unsigned long currentTime = millis();
  if (currentTime - sessionCreatedAt > SESSION_TIMEOUT) {
    sessionToken = ""; // Clear expired session
    return false;
  }
  
  return true;
}

String getSessionCookie() {
  if (server.hasHeader("Cookie")) {
    String cookies = server.header("Cookie");
    int idx = cookies.indexOf("ESPSESSION=");
    if (idx >= 0) {
      int endIdx = cookies.indexOf(';', idx);
      if (endIdx < 0) endIdx = cookies.length();
      String token = cookies.substring(idx + 11, endIdx);
      token.trim(); // Remove any whitespace
      return token;
    }
  }
  return "";
}

// ---------- Helpers ----------

// Helper function to set relay state correctly based on hardware type
void setRelayState(int relayIndex, int state) {
  if (RELAY_ACTIVE_LOW) {
    // Active LOW relay: 1 (ON) = LOW, 0 (OFF) = HIGH
    digitalWrite(relayPins[relayIndex], state ? LOW : HIGH);
  } else {
    // Active HIGH relay: 1 (ON) = HIGH, 0 (OFF) = LOW
    digitalWrite(relayPins[relayIndex], state ? HIGH : LOW);
  }
}

// Base64 URL encoding (without padding)
String base64UrlEncode(const uint8_t* input, size_t length) {
  String b64 = base64::encode(input, length);
  // Convert to URL-safe base64
  b64.replace('+', '-');
  b64.replace('/', '_');
  // Remove padding
  while (b64.endsWith("=")) {
    b64.remove(b64.length() - 1);
  }
  return b64;
}

// HMAC SHA256 signature
String hmacSHA256(const String& data, const String& key) {
  byte hmacResult[32];
  
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
  
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key.c_str(), key.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)data.c_str(), data.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);
  
  return base64UrlEncode(hmacResult, 32);
}

// Generate JWT token locally
String generateJWT() {
  // JWT Header
  StaticJsonDocument<128> headerDoc;
  headerDoc["alg"] = "HS256";
  headerDoc["typ"] = "JWT";
  
  String headerJson;
  serializeJson(headerDoc, headerJson);
  String headerEncoded = base64UrlEncode((const uint8_t*)headerJson.c_str(), headerJson.length());
  
  // JWT Payload
  unsigned long currentTime = millis() / 1000; // Convert to seconds
  unsigned long expirationTime = currentTime + (JWT_EXPIRATION_MINUTES * 60);
  
  StaticJsonDocument<256> payloadDoc;
  payloadDoc["deviceId"] = device_id;
  payloadDoc["iat"] = currentTime;
  payloadDoc["exp"] = expirationTime;
  
  String payloadJson;
  serializeJson(payloadDoc, payloadJson);
  String payloadEncoded = base64UrlEncode((const uint8_t*)payloadJson.c_str(), payloadJson.length());
  
  // Create signature
  String dataToSign = headerEncoded + "." + payloadEncoded;
  String signature = hmacSHA256(dataToSign, jwtSecretKey);
  
  // Combine all parts
  String jwt = headerEncoded + "." + payloadEncoded + "." + signature;
  return jwt;
}

// Get valid JWT (generate new if expired or missing)
String getValidJWT() {
  unsigned long currentTime = millis();
  unsigned long jwtLifetime = JWT_EXPIRATION_MINUTES * 60 * 1000UL;
  
  // Check if we need to generate a new token (expired or not cached)
  if (cachedJWT.length() == 0 || (currentTime - jwtGeneratedAt) >= (jwtLifetime - 60000UL)) {
    // Generate new token (with 1 minute buffer before expiration)
    cachedJWT = generateJWT();
    jwtGeneratedAt = currentTime;
  }
  
  return cachedJWT;
}

String headerBasicAuthEncoded(const String &user, const String &pass) {
  // not using Basic, but helper if needed
  return "";
}

void loadPrefs() {
  prefs.begin("iotcfg", false);
  device_id = prefs.getString("device_id", DEVICE_ID_DEFAULT);
  jwtSecretKey = prefs.getString("jwtSecret", JWT_SECRET_DEFAULT);
  apiUsername = prefs.getString("apiUser", "admin");
  apiPassword = prefs.getString("apiPass", "admin");
  webUsername = prefs.getString("webUser", "admin");
  webPassword = prefs.getString("webPass", "admin");
  sensorInterval = prefs.getULong("sensInterval", 30000UL);
  prefs.end();
}

void savePrefs() {
  prefs.begin("iotcfg", false);
  prefs.putString("device_id", device_id);
  prefs.putString("jwtSecret", jwtSecretKey);
  prefs.putString("apiUser", apiUsername);
  prefs.putString("apiPass", apiPassword);
  prefs.putString("webUser", webUsername);
  prefs.putString("webPass", webPassword);
  prefs.putULong("sensInterval", sensorInterval);
  prefs.end();
}

void saveSensorReadingToFile(const SensorReading &reading) {
  // Load existing data from file
  StaticJsonDocument<4096> doc;
  JsonArray readingsArray;
  
  // Try to read existing file
  if (LittleFS.exists(SENSOR_DATA_FILE)) {
    File file = LittleFS.open(SENSOR_DATA_FILE, "r");
    if (file) {
      DeserializationError error = deserializeJson(doc, file);
      file.close();
      
      if (!error) {
        readingsArray = doc.as<JsonArray>();
      } else {
        // Create new array if file is corrupted
        readingsArray = doc.to<JsonArray>();
      }
    } else {
      // File doesn't exist, create new array
      readingsArray = doc.to<JsonArray>();
    }
  } else {
    // File doesn't exist, create new array
    readingsArray = doc.to<JsonArray>();
  }
  
  // Get current UTC time
  struct tm timeinfo;
  char timestamp[25];
  if (getLocalTime(&timeinfo)) {
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  } else {
    strcpy(timestamp, "2024-01-01T00:00:00Z");
  }
  
  // Add new reading to array
  JsonObject readingObj = readingsArray.createNestedObject();
  readingObj["sensorIdentifier"] = reading.name;
  readingObj["numericValue"] = reading.value;
  readingObj["textValue"] = String(reading.value, 2) + " " + reading.unit;
  readingObj["capturedOnUtc"] = timestamp;
  
  // Save back to file
  File file = LittleFS.open(SENSOR_DATA_FILE, "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
  } else {
  }
}

bool sendStoredSensorData() {
  if (!LittleFS.exists(SENSOR_DATA_FILE)) {
    return true; // Not an error, just no data to send
  }
  
  File file = LittleFS.open(SENSOR_DATA_FILE, "r");
  if (!file) {
    return false;
  }
  
  // Read file content
  String fileContent = file.readString();
  file.close();
  
  if (fileContent.length() == 0) {
    LittleFS.remove(SENSOR_DATA_FILE);
    return true;
  }
  
  // Parse JSON
  StaticJsonDocument<4096> doc;
  DeserializationError error = deserializeJson(doc, fileContent);
  
  if (error) {
    LittleFS.remove(SENSOR_DATA_FILE); // Remove corrupted file
    return false;
  }
  
  JsonArray readingsArray = doc.as<JsonArray>();
  if (readingsArray.size() == 0) {
    LittleFS.remove(SENSOR_DATA_FILE);
    return true;
  }
  
  // Send to API
  HTTPClient http;
  String url = String(BACKEND_BASE_URL) + SENSOR_POST_ENDPOINT;
  
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-ESP32-Device-Id", device_id);
  http.addHeader("X-IOT-Token", getValidJWT());
  
  String payload;
  serializeJson(doc, payload);
  
  int code = http.POST(payload);
  if (code > 0) {
    String resp = http.getString();
    Serial.println("[" + getTimestamp() + "] Stored data response (" + String(code) + "): " + resp);
    
    if (code >= 200 && code < 300) {
      // Success - delete the file
      LittleFS.remove(SENSOR_DATA_FILE);
      http.end();
      return true;
    } else {
    }
  } else {
  }
  
  http.end();
  return false;
}

void checkAndSendStoredDataOnStartup() {
  if (sendStoredSensorData()) {
  } else {
  }
}

void resetWifiSettings() {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("WiFi reset successful");
  lcd.setCursor(0,1);
  lcd.print("Restarting ESP");

  Serial.println("Resetting Wi-Fi settings and clearing saved Wi-Fi...");
  delay(1000);

  digitalWrite(LED_WIFI_PIN, LOW);
  wifiManager.resetSettings();
  // Optionally also clear stored API/web credentials?
  // prefs.begin("iotcfg", false); prefs.clear(); prefs.end();
  ESP.restart();
}

// ---------- IoT Device API Functions ----------

DeviceStatus getDeviceStatus() {
  HTTPClient http;
  String url = String(BACKEND_BASE_URL) + GET_STATUS_ENDPOINT;

  Serial.println("[" + getTimestamp() + "] Getting device status from: " + url);

  http.begin(url);
  http.addHeader("X-ESP32-Device-Id", device_id);
  http.addHeader("X-IOT-Token", getValidJWT());

  int code = http.GET();
  DeviceStatus result = {"", false}; // Default values
  
  if (code > 0) {
    String resp = http.getString();
    Serial.println("[" + getTimestamp() + "] Get Status response (" + String(code) + "): " + resp);

    if (code == 200) {
      // Parse JSON response
      StaticJsonDocument<512> doc;
      DeserializationError error = deserializeJson(doc, resp);

      if (!error) {
        result.status = doc["status"].as<String>();
        result.online = doc["online"].as<bool>();
        result.success = true;
        http.end();
        return result;
      } else {
        Serial.println("[" + getTimestamp() + "] JSON parsing error: " + String(error.c_str()));
      }
    } else if (code == 404 || code == 401) {
      Serial.println("[" + getTimestamp() + "] ❌ Device not found or unauthorized. Code: " + String(code));
    }
  } else {
    Serial.println("[" + getTimestamp() + "] Get Status failed, error: " + http.errorToString(code));
  }
  http.end();
  return result;
}

bool submitDeviceStatus(const String &statusString) {
  HTTPClient http;
  String url = String(BACKEND_BASE_URL) + POST_STATUS_ENDPOINT;

  Serial.println("[" + getTimestamp() + "] Submitting device status to: " + url + " Device ID: " + device_id);

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-ESP32-Device-Id", device_id);

  String jwt = getValidJWT();
  http.addHeader("X-IOT-Token", jwt);
  Serial.println("[" + getTimestamp() + "] JWT Token length: " + String(jwt.length()));


  // Check if JWT is expired
  if (jwt.length() > 0) {
    // Parse JWT to check expiration (simple check)
    int dot1 = jwt.indexOf('.');
    int dot2 = jwt.indexOf('.', dot1 + 1);
    if (dot1 > 0 && dot2 > dot1) {
      String payload = jwt.substring(dot1 + 1, dot2);
      // Base64 decode would be needed here, but for now just log
      Serial.println("[" + getTimestamp() + "] JWT payload (base64): " + payload);
    }
  }

  StaticJsonDocument<128> doc;
  doc["formattedstatus"] = statusString;

  String payload;
  serializeJson(doc, payload);

  Serial.println("[" + getTimestamp() + "] Status payload: " + payload);
  Serial.println("[" + getTimestamp() + "] Status string length: " + String(statusString.length()));
  Serial.println("[" + getTimestamp() + "] Status string content: '" + statusString + "'");

  int code = http.POST(payload);
  if (code > 0) {
    String resp = http.getString();
    Serial.println("[" + getTimestamp() + "] Submit Status response (" + String(code) + "): " + resp);

    if (code == 400) {
      Serial.println("[" + getTimestamp() + "] ❌ 400 Bad Request - Possible causes:");
      Serial.println("[" + getTimestamp() + "]   1. Device '" + device_id + "' may not be registered in backend");
      Serial.println("[" + getTimestamp() + "]   2. JWT token may be invalid or expired");
      Serial.println("[" + getTimestamp() + "]   3. Status format may be incorrect (expected: string of 1-8 digits)");
      Serial.println("[" + getTimestamp() + "]   4. API endpoint may require different authentication");

      // Try to get device status to check if device exists
      Serial.println("[" + getTimestamp() + "] Checking if device exists in backend...");
      DeviceStatus devStatus = getDeviceStatus();
      if (devStatus.success) {
        Serial.println("[" + getTimestamp() + "] ✅ Device exists in backend (status: " + devStatus.status + ", online: " + String(devStatus.online) + ")");
      } else {
        Serial.println("[" + getTimestamp() + "] ❌ Device NOT found in backend or authentication failed");
      }
    } else if (code == 401) {
      Serial.println("[" + getTimestamp() + "] ❌ 401 Unauthorized - JWT token is invalid");
      // Clear cached JWT to force regeneration
      cachedJWT = "";
      jwtGeneratedAt = 0;
      Serial.println("[" + getTimestamp() + "] Cleared cached JWT - will regenerate on next attempt");
    } else if (code == 404) {
      Serial.println("[" + getTimestamp() + "] ❌ 404 Not Found - Device not registered or wrong endpoint");
    }

    http.end();
    return (code >= 200 && code < 300);
  } else {
    Serial.println("[" + getTimestamp() + "] Submit Status failed, error: " + http.errorToString(code));
    http.end();
    return false;
  }
}

bool pingDevice() {
  HTTPClient http;
  String url = String(BACKEND_BASE_URL) + PING_ENDPOINT;

  Serial.println("[" + getTimestamp() + "] Pinging device to: " + url);

  http.begin(url);
  http.addHeader("X-ESP32-Device-Id", device_id);
  http.addHeader("X-IOT-Token", getValidJWT());

  int code = http.POST("");
  if (code > 0) {
    String resp = http.getString();
    Serial.println("[" + getTimestamp() + "] Ping response (" + String(code) + "): " + resp);
    http.end();
    return (code >= 200 && code < 300);
  } else {
    Serial.println("[" + getTimestamp() + "] Ping failed, error: " + http.errorToString(code));
    http.end();
    return false;
  }
}

// ---------- MQTT Callback ----------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Convert payload to String
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  Serial.print("📨 MQTT message received on topic: ");
  Serial.println(topic);
  Serial.print("📄 Raw message: ");
  Serial.println(message);

  // Parse JSON: {"t":"Type","d":"Data"}
  // Find type field
  int tIdx = message.indexOf("\"t\":");
  if (tIdx < 0) {
    Serial.println("⚠️ No 't' field found in message");
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    return;
  }
  
  int typeStart = message.indexOf('"', tIdx + 4);
  int typeEnd = message.indexOf('"', typeStart + 1);
  if (typeStart < 0 || typeEnd < 0) {
    Serial.println("⚠️ Invalid 't' field format");
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    return;
  }
  
  String msgType = message.substring(typeStart + 1, typeEnd);
  
  // Find data field
  int dIdx = message.indexOf("\"d\":", typeEnd);
  String msgData = "";
  
  if (dIdx >= 0) {
    int dataStart = message.indexOf('"', dIdx + 4);
    if (dataStart >= 0) {
      int dataEnd = message.indexOf('"', dataStart + 1);
      if (dataEnd >= 0) {
        msgData = message.substring(dataStart + 1, dataEnd);
      }
    } else if (message.indexOf("null", dIdx + 4) >= 0) {
      msgData = "null";
    }
  }

  Serial.print("🔖 Type: ");
  Serial.println(msgType);
  Serial.print("📦 Data: ");
  Serial.println(msgData);

  // Handle message types
  if (msgType == "Status") {
    // Status: relay states (can be 1-8 characters)
    if (msgData.length() > 0) {
      int numRelays = min(8, (int)msgData.length());
      Serial.println("🔌 Updating " + String(numRelays) + " relays from MQTT");
      
      for (int i = 0; i < numRelays; i++) {
        int val = (msgData[i] == '1') ? 1 : 0;
        relayState[i] = val;
        setRelayState(i, val);
        Serial.println("   Relay " + String(i + 1) + " → " + String(val ? "ON" : "OFF"));
      }
      
      // Set remaining relays to OFF if status string is shorter than 8
      for (int i = numRelays; i < 8; i++) {
        relayState[i] = 0;
        setRelayState(i, 0);
        Serial.println("   Relay " + String(i + 1) + " → OFF (not in status)");
      }
      
      Serial.println("✅ Relay states updated from MQTT");
    } else {
      Serial.println("⚠️ Empty status data received");
    }
  } 
  else if (msgType == "Message") {
    // Custom message to display on LCD for 5 minutes
    customMessage = msgData;
    customMessageMode = true;
    customMessageReceivedAt = millis();
    Serial.println("💬 Custom message set: " + customMessage + " (will auto-clear in 5 minutes)");
  }
  else if (msgType == "Ping") {
    // Trigger immediate sensor reading and send ping to API, plus send stored data
    Serial.println("🏓 Ping received - triggering sensor read, sending stored data, and API ping");
    lastSensorMillis = 0; // Force immediate sensor read in loop
    
    // Send stored sensor data
    if (sendStoredSensorData()) {
      Serial.println("✅ Stored sensor data sent successfully via Ping");
    } else {
      Serial.println("❌ Failed to send stored sensor data via Ping");
    }
    
    // Send ping to backend API
    if (pingDevice()) {
      Serial.println("✅ Ping sent successfully to backend");
    } else {
      Serial.println("❌ Failed to send ping to backend");
    }
  }
  else if (msgType == "Sync") {
    // Sync message - parse nested data object
    Serial.println("🔄 Sync message received");
    
    // Parse nested data: {"t":"Info","m":"hi fuad"}
    // The "d" field contains another JSON object, not a string
    int nestedDIdx = message.indexOf("\"d\":", typeEnd);
    if (nestedDIdx >= 0) {
      // Check if data is an object (starts with {)
      int objStart = message.indexOf('{', nestedDIdx + 4);
      if (objStart >= 0) {
        int objEnd = message.indexOf('}', objStart);
        if (objEnd >= 0) {
          String nestedData = message.substring(objStart, objEnd + 1);
          Serial.println("📦 Nested data: " + nestedData);
          
          // Parse nested type
          int nestedTIdx = nestedData.indexOf("\"t\":");
          if (nestedTIdx >= 0) {
            int nestedTypeStart = nestedData.indexOf('"', nestedTIdx + 4);
            int nestedTypeEnd = nestedData.indexOf('"', nestedTypeStart + 1);
            if (nestedTypeStart >= 0 && nestedTypeEnd >= 0) {
              String nestedType = nestedData.substring(nestedTypeStart + 1, nestedTypeEnd);
              Serial.println("🔖 Nested type: " + nestedType);
              
              // Handle nested Info type
              if (nestedType == "Info") {
                // Extract the message from "m" field
                int nestedMIdx = nestedData.indexOf("\"m\":");
                if (nestedMIdx >= 0) {
                  int nestedMsgStart = nestedData.indexOf('"', nestedMIdx + 4);
                  int nestedMsgEnd = nestedData.indexOf('"', nestedMsgStart + 1);
                  if (nestedMsgStart >= 0 && nestedMsgEnd >= 0) {
                    String infoMessageText = nestedData.substring(nestedMsgStart + 1, nestedMsgEnd);
                    Serial.println("ℹ️ Nested Info message: " + infoMessageText);
                    
                    // Display on LCD and wait for OK button press
                    infoMessage = infoMessageText;
                    infoMessageMode = true;
                    infoMessageReceivedAt = millis();
                    Serial.println("✅ Info message displayed on LCD - pausing cycling until OK button pressed");
                  }
                }
              }
            }
          }
        }
      }
    }
    // Sync message - trigger sending of stored sensor data
    Serial.println("🔄 Sync message received - sending stored sensor data");
    if (sendStoredSensorData()) {
      Serial.println("✅ Stored sensor data sent successfully via Sync");
    } else {
      Serial.println("❌ Failed to send stored sensor data via Sync");
    }
  }
  else if (msgType == "Buzz") {
    // Activate buzzer
    Serial.println("🔊 Buzz message received - activating buzzer");
    digitalWrite(BUZZER_PIN, HIGH);
    delay(500); // Buzz for 0.5 second
    digitalWrite(BUZZER_PIN, LOW);
    Serial.println("✅ Buzzer deactivated");
  }
  else if (msgType == "Info") {
    // Info message - display on LCD and wait for OK button press
    infoMessage = msgData;
    infoMessageMode = true;
    infoMessageReceivedAt = millis();
    Serial.println("ℹ️ Info message received - pausing LCD cycling until OK button pressed");
    Serial.println("   Message: " + infoMessage);
  }
  
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
}

// ---------- MQTT Connection ----------
void connectToMQTT() {
  if (mqttClient.connected()) return;
  
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  Serial.println("🔌 Connecting to MQTT broker...");
  Serial.println("   Broker: " + String(MQTT_BROKER));
  Serial.println("   Port: " + String(MQTT_PORT));
  Serial.println("   Username: " + String(MQTT_USERNAME));
  
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  
  String clientId = "ESP32-" + device_id;
  Serial.println("   Client ID: " + clientId);
  
  if (mqttClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD)) {
    Serial.println("✅ MQTT connected successfully!");
    
    // Subscribe to device-specific topic: devices/{device_id}
    String topic = "devices/" + device_id;
    Serial.println("📡 Subscribing to topic: " + topic);
    
    if (mqttClient.subscribe(topic.c_str())) {
      Serial.println("✅ Successfully subscribed to: " + topic);
      Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
      
      // Sync relay state from backend after MQTT reconnection
      Serial.println("🔄 Syncing relay states from backend after MQTT reconnection...");
      fetchRelayStateFromBackend();
    } else {
      Serial.println("❌ Failed to subscribe to: " + topic);
      Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    }
  } else {
    Serial.print("❌ MQTT connection failed, rc=");
    Serial.println(mqttClient.state());
    Serial.println("   Error codes:");
    Serial.println("   -4 = MQTT_CONNECTION_TIMEOUT");
    Serial.println("   -3 = MQTT_CONNECTION_LOST");
    Serial.println("   -2 = MQTT_CONNECT_FAILED");
    Serial.println("   -1 = MQTT_DISCONNECTED");
    Serial.println("    1 = MQTT_CONNECT_BAD_PROTOCOL");
    Serial.println("    2 = MQTT_CONNECT_BAD_CLIENT_ID");
    Serial.println("    3 = MQTT_CONNECT_UNAVAILABLE");
    Serial.println("    4 = MQTT_CONNECT_BAD_CREDENTIALS");
    Serial.println("    5 = MQTT_CONNECT_UNAUTHORIZED");
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  }
}

bool fetchRelayStateFromBackend() {
  // Get device status which includes relay states
  DeviceStatus devStatus = getDeviceStatus();
  
  if (devStatus.success) {
    Serial.println("Device status: " + devStatus.status + ", Online: " + String(devStatus.online));
    
    // Parse status string (up to 8 characters representing relay states)
    if (devStatus.status.length() > 0) {
      int numRelays = min(8, (int)devStatus.status.length());
      Serial.println("Updating " + String(numRelays) + " relays from status string");
      
      for (int i = 0; i < numRelays; i++) {
        int val = (devStatus.status[i] == '1') ? 1 : 0;
        relayState[i] = val;
        setRelayState(i, val);
        Serial.println("  Relay " + String(i + 1) + " set to: " + String(val) + " (" + String(val ? "ON" : "OFF") + ")");
      }
      
      // Set remaining relays to OFF if status string is shorter than 8
      for (int i = numRelays; i < 8; i++) {
        relayState[i] = 0;
        setRelayState(i, 0);
        Serial.println("  Relay " + String(i + 1) + " set to: 0 (OFF) - not in status string");
      }
      
      Serial.println("✅ Relay states updated successfully from backend");
      return true;
    } else {
      Serial.println("⚠️ Status string is empty");
    }
  } else {
    Serial.println("❌ Failed to get device status from backend");
  }
  
  return false;
}

bool postRelayUpdateToBackend(int index, int val) {
  // Build status string from current relay states
  String statusString = "";
  for (int i = 0; i < 8; i++) {
    statusString += String(relayState[i]);
  }
  
  return submitDeviceStatus(statusString);
}

// ---------- Web UI ----------
bool checkWebAuth() {
  // Check if user has a valid session cookie
  String cookieToken = getSessionCookie();
  bool isValid = isSessionValid(cookieToken);
  return isValid;
}

void requireAuth() {
  if (!checkWebAuth()) {
    server.sendHeader("Location", "/login");
    server.send(302);
  }
}

String htmlTemplate(const String &title, const String &body) {
  String s = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>" + title + "</title>";
  s += "<script src='https://code.jquery.com/jquery-3.7.1.min.js' integrity='sha256-/JqT3SQfawRcv/BIHPThkBvs0OEvtFFmqPF/lYI/Cxo=' crossorigin='anonymous'></script>";
  s += "<style>";
  s += "body{font-family:Arial,sans-serif;margin:20px;background:#f0f0f0;}";
  s += ".container{max-width:800px;margin:0 auto;background:white;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}";
  s += "h3{color:#333;border-bottom:2px solid #4CAF50;padding-bottom:10px;}";
  s += "a{color:#4CAF50;text-decoration:none;margin-right:15px;}";
  s += "a:hover{text-decoration:underline;}";
  s += "input[type=text],input[type=password],input[type=number]{width:100%;padding:12px;margin:5px 0 15px;border:1px solid #ddd;border-radius:6px;box-sizing:border-box;font-size:14px;transition:border-color 0.3s;}";
  s += "input[type=text]:focus,input[type=password]:focus,input[type=number]:focus{outline:none;border-color:#4CAF50;box-shadow:0 0 0 2px rgba(76,175,80,0.1);}";
  s += "input[type=text]:read-only{background:#f5f5f5;color:#666;cursor:not-allowed;}";
  s += "input[type=submit],button{background:#4CAF50;color:white;padding:10px 20px;border:none;border-radius:4px;cursor:pointer;font-size:14px;transition:all 0.3s;}";
  s += "input[type=submit]:hover,button:hover{background:#45a049;transform:translateY(-1px);box-shadow:0 2px 5px rgba(0,0,0,0.2);}";
  s += "input[type=checkbox]{margin-right:5px;}";
  s += ".logout{float:right;background:#f44336;color:white;padding:8px 16px;border-radius:20px;text-decoration:none;font-size:13px;transition:all 0.3s;display:inline-block;margin-bottom:10px;}";
  s += ".logout:hover{background:#da190b;transform:translateY(-2px);box-shadow:0 4px 8px rgba(244,67,54,0.3);text-decoration:none;}";
  s += ".alert{padding:15px;margin:15px 0;border-radius:4px;display:none;}";
  s += ".alert.success{background:#d4edda;color:#155724;border:1px solid #c3e6cb;}";
  s += ".alert.show{display:block;animation:slideIn 0.3s ease-out;}";
  s += "@keyframes slideIn{from{opacity:0;transform:translateY(-10px);}to{opacity:1;transform:translateY(0);}}";
  s += "label{display:block;margin-bottom:5px;color:#555;font-weight:500;font-size:14px;}";
  s += ".form-group{margin-bottom:20px;}";
  s += "</style>";
  s += "<script>";
  s += "function showAlert(msg,type){";
  s += "var alert=document.createElement('div');";
  s += "alert.className='alert '+type+' show';";
  s += "alert.textContent=msg;";
  s += "document.querySelector('.container').insertBefore(alert,document.querySelector('h3').nextSibling);";
  s += "setTimeout(function(){alert.style.display='none';},3000);";
  s += "}";
  s += "window.onload=function(){";
  s += "var url=new URL(window.location.href);";
  s += "if(url.searchParams.get('success')=='1'){showAlert('Settings saved successfully!','success');}";
  s += "if(url.searchParams.get('updated')=='1'){showAlert('Relays updated successfully!','success');}";
  s += "}";
  s += "</script>";
  s += "</head><body><div class='container'>";
  if (checkWebAuth()) {
    s += "<a href='/logout' class='logout'>🚪 Logout</a>";
  }
  s += "<h3>" + title + "</h3>";
  s += body;
  s += "<hr><small>Device: " + device_id + "</small></div></body></html>";
  return s;
}

void handleLogin() {
  // If already logged in, redirect to sensors page
  if (checkWebAuth()) {
    server.sendHeader("Location", "/sensors");
    server.send(302);
    return;
  }
  
  String html = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>ESP32 Login</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);height:100vh;margin:0;display:flex;align-items:center;justify-content:center;}";
  html += ".login-box{background:white;padding:40px;border-radius:10px;box-shadow:0 10px 25px rgba(0,0,0,0.2);width:300px;}";
  html += "h2{text-align:center;color:#333;margin-bottom:30px;}";
  html += "input[type=text],input[type=password]{width:100%;padding:12px;margin:8px 0;border:1px solid #ddd;border-radius:4px;box-sizing:border-box;}";
  html += "input[type=submit]{width:100%;background:#4CAF50;color:white;padding:14px;border:none;border-radius:4px;cursor:pointer;font-size:16px;margin-top:10px;}";
  html += "input[type=submit]:hover{background:#45a049;}";
  html += ".error{color:#f44336;text-align:center;margin-top:10px;}";
  html += "label{color:#666;font-size:14px;}";
  html += "</style></head><body>";
  html += "<div class='login-box'>";
  html += "<h2>ESP32 IoT System</h2>";
  html += "<form method='POST' action='/login'>";
  html += "<label>Username:</label>";
  html += "<input type='text' name='username' required autofocus>";
  html += "<label>Password:</label>";
  html += "<input type='password' name='password' required>";
  html += "<input type='submit' value='Login'>";
  html += "</form>";
  
  // Show error message if login failed
  if (server.hasArg("error")) {
    html += "<div class='error'>Invalid username or password</div>";
  }
  
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleLoginPost() {
  String username = server.arg("username");
  String password = server.arg("password");
  
  if (username == webUsername && password == webPassword) {
    // Create new session
    sessionToken = generateSessionToken();
    sessionCreatedAt = millis();
    
    // Set session cookie - removed HttpOnly for better compatibility
    String cookie = "ESPSESSION=" + sessionToken + "; Path=/; Max-Age=3600";
    server.sendHeader("Set-Cookie", cookie);
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    
    // Send HTML page with JavaScript to set cookie client-side and redirect
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    html += "<title>Login Success</title>";
    html += "<script>";
    html += "document.cookie = 'ESPSESSION=" + sessionToken + "; path=/; max-age=3600';";
    html += "setTimeout(function(){window.location.href='/sensors';}, 1000);";
    html += "</script>";
    html += "</head><body style='font-family:Arial;text-align:center;padding-top:100px;'>";
    html += "<h2 style='color:green;'>✅ Login Successful!</h2>";
    html += "<p>Redirecting to sensors page...</p>";
    html += "<p><a href='/sensors'>Click here if not redirected</a></p>";
    html += "</body></html>";
    
    server.send(200, "text/html", html);
    
  } else {
    // Login failed
    server.sendHeader("Location", "/login?error=1");
    server.send(302);
  }
}

void handleLogout() {
  // Clear session
  sessionToken = "";
  sessionCreatedAt = 0;
  
  // Clear cookie
  server.sendHeader("Set-Cookie", "ESPSESSION=; Path=/; Max-Age=0");
  server.sendHeader("Location", "/login");
  server.send(302);
}

void handleDiagnostics() {
  String html = "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<title>ESP32 Diagnostics</title>";
  html += "<style>body{font-family:monospace;padding:20px;background:#1e1e1e;color:#d4d4d4;}";
  html += "h2{color:#4ec9b0;}";
  html += ".info{background:#2d2d2d;padding:15px;margin:10px 0;border-radius:5px;border-left:4px solid #4ec9b0;}";
  html += ".label{color:#9cdcfe;font-weight:bold;}";
  html += ".value{color:#ce9178;}</style></head><body>";
  html += "<h2>🔍 ESP32 Diagnostics</h2>";
  
  html += "<div class='info'><span class='label'>Firmware Version:</span> <span class='value'>" + String(FIRMWARE_VERSION) + "</span></div>";
  html += "<div class='info'><span class='label'>Device ID:</span> <span class='value'>" + device_id + "</span></div>";
  html += "<div class='info'><span class='label'>WiFi SSID:</span> <span class='value'>" + WiFi.SSID() + "</span></div>";
  html += "<div class='info'><span class='label'>IP Address:</span> <span class='value'>" + WiFi.localIP().toString() + "</span></div>";
  html += "<div class='info'><span class='label'>MAC Address:</span> <span class='value'>" + WiFi.macAddress() + "</span></div>";
  html += "<div class='info'><span class='label'>Session Token:</span> <span class='value'>" + (sessionToken.length() > 0 ? sessionToken : "None") + "</span></div>";
  
  String cookieValue = getSessionCookie();
  html += "<div class='info'><span class='label'>Cookie Received:</span> <span class='value'>" + (cookieValue.length() > 0 ? cookieValue : "None") + "</span></div>";
  
  html += "<div class='info'><span class='label'>Auth Status:</span> <span class='value'>" + String(checkWebAuth() ? "✅ Authenticated" : "❌ Not Authenticated") + "</span></div>";
  
  html += "<div class='info'><span class='label'>MQTT Status:</span> <span class='value'>" + String(mqttClient.connected() ? "✅ Connected" : "❌ Disconnected") + "</span></div>";
  html += "<div class='info'><span class='label'>API Base URL:</span> <span class='value'>" + String(BACKEND_BASE_URL) + "</span></div>";
  html += "<div class='info'><span class='label'>Sensor Endpoint:</span> <span class='value'>" + String(SENSOR_POST_ENDPOINT) + "</span></div>";
  html += "<div class='info'><span class='label'>Sensor Interval:</span> <span class='value'>" + String(sensorInterval) + " ms</span></div>";
  
  html += "<div class='info'><span class='label'>Uptime:</span> <span class='value'>" + String(millis() / 1000) + " seconds</span></div>";
  html += "<div class='info'><span class='label'>Free Heap:</span> <span class='value'>" + String(ESP.getFreeHeap()) + " bytes</span></div>";
  
  html += "<p><a href='/' style='color:#4ec9b0;'>← Back to Home</a></p>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleRoot() {
  if (!checkWebAuth()) {
    requireAuth();
    return;
  }
  
  // show sensor readings and relay controls
  String body = "<div style='text-align:center;padding:20px;'>";
  body += "<h4 style='color:#666;'>Welcome to ESP32 IoT Dashboard</h4>";
  body += "<div style='margin:30px 0;'>";
  body += "<a href='/sensors' style='display:inline-block;margin:10px;padding:15px 30px;background:#4CAF50;color:white;border-radius:5px;text-decoration:none;'>📊 Live Sensors</a>";
  body += "<a href='/relays' style='display:inline-block;margin:10px;padding:15px 30px;background:#2196F3;color:white;border-radius:5px;text-decoration:none;'>🔌 Relays</a>";
  body += "<a href='/config' style='display:inline-block;margin:10px;padding:15px 30px;background:#FF9800;color:white;border-radius:5px;text-decoration:none;'>⚙️ Settings</a>";
  body += "<a href='/diagnostics' style='display:inline-block;margin:10px;padding:15px 30px;background:#9C27B0;color:white;border-radius:5px;text-decoration:none;'>🔍 Diagnostics</a>";
  body += "</div></div>";
  
  server.send(200, "text/html", htmlTemplate("ESP32 - Home", body));
}

void handleSensorsPage() {
  if (!checkWebAuth()) {
    requireAuth();
    return;
  }
  
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  ds18b20.requestTemperatures();
  float t2 = ds18b20.getTempCByIndex(0);
  
  String b = "<style>";
  b += ".sensor-card{background:#f9f9f9;padding:20px;border-radius:8px;margin-bottom:20px;border-left:4px solid #4CAF50;}";
  b += ".sensor-value{font-size:32px;font-weight:bold;margin:10px 0;}";
  b += ".sensor-label{color:#666;font-size:14px;text-transform:uppercase;letter-spacing:1px;}";
  b += ".bar-graph{width:100%;height:30px;background:#e0e0e0;border-radius:15px;overflow:hidden;margin:10px 0;position:relative;}";
  b += ".bar-fill{height:100%;background:linear-gradient(90deg,#4CAF50,#8BC34A);border-radius:15px;transition:width 0.5s ease;}";
  b += ".bar-label{position:absolute;left:50%;top:50%;transform:translate(-50%,-50%);color:#333;font-size:12px;font-weight:bold;z-index:1;}";
  b += ".refresh-indicator{position:fixed;top:20px;right:20px;background:rgba(76,175,80,0.9);color:white;padding:10px 20px;border-radius:20px;font-size:12px;box-shadow:0 2px 5px rgba(0,0,0,0.2);z-index:1000;}";
  b += "</style>";
  
  // Auto-reload script
  b += "<script>";
  b += "var countdown = 5;";
  b += "var interval;";
  b += "function updateCountdown(){";
  b += "  var indicator = document.getElementById('refresh-indicator');";
  b += "  if(indicator){";
  b += "    indicator.textContent = '🔄 Auto-refresh in ' + countdown + 's';";
  b += "    countdown--;";
  b += "    if(countdown < 0){";
  b += "      clearInterval(interval);";
  b += "      location.reload();";
  b += "    }";
  b += "  }";
  b += "}";
  b += "window.onload = function(){";
  b += "  interval = setInterval(updateCountdown, 1000);";
  b += "};";
  b += "</script>";
  
  // Refresh indicator
  b += "<div id='refresh-indicator' class='refresh-indicator'>🔄 Auto-refresh in 5s</div>";
  
  // DHT11 Temperature Card
  b += "<div class='sensor-card' style='border-left-color:#FF6B6B;'>";
  b += "<div class='sensor-label'>🌡️ DHT11 Temperature</div>";
  b += "<div class='sensor-value' style='color:#FF6B6B;'>" + String(t, 1) + " °C</div>";
  b += "<div class='bar-graph'>";
  // Temperature range: -40°C to 60°C (100°C range)
  float tempPercent = constrain(map(t, -40.0, 60.0, 0.0, 100.0), 0.0, 100.0);
  b += "<div class='bar-label'>" + String(t, 1) + "°C</div>";
  // Temperature gradient: Blue (cold) -> Green -> Yellow -> Red (hot)
  b += "<div class='bar-fill' style='width:" + String(tempPercent, 1) + "%;background:linear-gradient(90deg,#2196F3,#4CAF50,#FFEB3B,#FF9800,#F44336);'></div>";
  b += "</div></div>";
  
  // DHT11 Humidity Card
  b += "<div class='sensor-card' style='border-left-color:#4ECDC4;'>";
  b += "<div class='sensor-label'>💧 DHT11 Humidity</div>";
  b += "<div class='sensor-value' style='color:#4ECDC4;'>" + String(h, 0) + " %</div>";
  b += "<div class='bar-graph'>";
  int humidPercent = constrain((int)h, 0, 100);
  b += "<div class='bar-label'>" + String(humidPercent) + "%</div>";
  b += "<div class='bar-fill' style='width:" + String(humidPercent) + "%;background:linear-gradient(90deg,#4ECDC4,#44A08D);'></div>";
  b += "</div></div>";
  
  // DS18B20 Temperature Card
  b += "<div class='sensor-card' style='border-left-color:#FFA726;'>";
  b += "<div class='sensor-label'>🌡️ DS18B20 Waterproof Temperature</div>";
  b += "<div class='sensor-value' style='color:#FFA726;'>" + String(t2, 2) + " °C</div>";
  b += "<div class='bar-graph'>";
  // Temperature range: -40°C to 60°C (100°C range)
  float temp2Percent = constrain(map(t2, -40.0, 60.0, 0.0, 100.0), 0.0, 100.0);
  b += "<div class='bar-label'>" + String(t2, 1) + "°C</div>";
  // Temperature gradient: Blue (cold) -> Green -> Yellow -> Red (hot)
  b += "<div class='bar-fill' style='width:" + String(temp2Percent, 1) + "%;background:linear-gradient(90deg,#2196F3,#4CAF50,#FFEB3B,#FF9800,#F44336);'></div>";
  b += "</div></div>";
  
  b += "<p style='margin-top:20px;'><a href='/'>← Back to Home</a> | <a href='/relays'>View Relays →</a></p>";
  
  server.send(200, "text/html", htmlTemplate("Live Sensors", b));
}

void handleRelaysPage() {
  if (!checkWebAuth()) {
    requireAuth();
    return;
  }
  
  String b = "<style>";
  b += ".relay-card{background:#f9f9f9;padding:20px;border-radius:8px;margin-bottom:15px;border-left:4px solid #2196F3;display:flex;align-items:center;justify-content:space-between;}";
  b += ".relay-label{font-size:18px;font-weight:500;}";
  b += ".relay-status{font-weight:bold;font-size:14px;padding:5px 10px;border-radius:15px;}";
  b += ".relay-status.on{background:#4CAF50;color:white;}";
  b += ".relay-status.off{background:#f44336;color:white;}";
  b += ".toggle-switch{position:relative;display:inline-block;width:60px;height:34px;}";
  b += ".toggle-switch input{display:none;}";
  b += ".toggle-slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background-color:#ccc;border-radius:34px;transition:.4s;}";
  b += ".toggle-slider:before{position:absolute;content:'';height:26px;width:26px;left:4px;bottom:4px;background-color:white;border-radius:50%;transition:.4s;}";
  b += "input:checked+.toggle-slider{background-color:#2196F3;}";
  b += "input:checked+.toggle-slider:before{transform:translateX(26px);}";
  b += ".toggle-slider:active{background-color:#2196F3;}";
  b += "</style>";
  
  b += "<form method='POST' action='/relays/set'>";
  
  // Relay controls
  for (int i=0;i<8;i++) {
    b += "<div class='relay-card'>";
    b += "<div class='relay-label'>🔌 Relay " + String(i+1) + "</div>";
    b += "<div style='display:flex;align-items:center;gap:15px;'>";
    b += "<div class='relay-status " + String(relayState[i] ? "on" : "off") + "'>" + String(relayState[i] ? "ON" : "OFF") + "</div>";
    b += "<label class='toggle-switch'>";
    String checkedAttr = relayState[i] ? "checked" : "";
    b += "<input type='checkbox' name='r" + String(i+1) + "' " + checkedAttr + ">";
    b += "<span class='toggle-slider'></span>";
    b += "</label>";
    b += "</div></div>";
  }

  b += "<br/><input id='updateRelays' type='submit' value='💾 Update Relays' style='width:100%;padding:15px;font-size:16px;margin-top:10px;display:none;'>";
  b += "<script>$('.toggle-switch').on('click', function() {$('#updateRelays').trigger('click');});</script>";
  b += "</form>";
  b += "<p style='margin-top:20px;'><a href='/'>← Back to Home</a> | <a href='/sensors'>View Sensors →</a></p>";
  
  server.send(200, "text/html", htmlTemplate("Relay Control", b));
}

void handleRelaysSet() {
  if (!checkWebAuth()) {
    requireAuth();
    return;
  }
  
  // Handle form submission - update all relays
  for (int i = 0; i < 8; i++) {
    String key = "r" + String(i + 1);
    bool checked = server.hasArg(key);
    relayState[i] = checked ? 1 : 0;
    setRelayState(i, relayState[i]);
  }
  
  // Submit updated relay status to backend
  postRelayUpdateToBackend(0, 0); // Index not used, submits all relays
  
  server.sendHeader("Location", "/relays?updated=1");
  server.send(302);
}

void handleConfigPage() {
  if (!checkWebAuth()) {
    requireAuth();
    return;
  }
  
  String b = "<form method='POST' action='/config/save'>";
  
  b += "<div class='form-group'>";
  b += "<label>📱 Device ID (Read-only)</label>";
  b += "<input name='device_id' value='" + device_id + "' readonly title='Device ID cannot be changed'>";
  b += "</div>";
  
  b += "<div class='form-group'>";
  b += "<label>🔐 JWT Secret Key</label>";
  b += "<input name='jwtSecret' type='password' value='" + jwtSecretKey + "' required>";
  b += "</div>";
  
  b += "<div class='form-group'>";
  b += "<label>👤 Web Username</label>";
  b += "<input name='webUser' value='" + webUsername + "' required>";
  b += "</div>";
  
  b += "<div class='form-group'>";
  b += "<label>🔒 Web Password</label>";
  b += "<input name='webPass' type='password' value='" + webPassword + "' required>";
  b += "</div>";
  
  b += "<div class='form-group'>";
  b += "<label>⏱️ Sensor Interval (milliseconds)</label>";
  b += "<input name='sensInt' type='number' value='" + String(sensorInterval) + "' min='1000' max='300000' required>";
  b += "</div>";
  
  b += "<input type='submit' value='💾 Save Configuration' style='width:100%;padding:15px;font-size:16px;margin-top:10px;'>";
  b += "</form><p style='margin-top:20px;'><a href='/'>← Back to Home</a></p>";
  
  server.send(200, "text/html", htmlTemplate("Configuration", b));
}

void handleConfigSave() {
  if (!checkWebAuth()) {
    requireAuth();
    return;
  }
  
  // Device ID is read-only and cannot be changed from UI
  // if (server.hasArg("device_id")) device_id = server.arg("device_id");
  
  if (server.hasArg("jwtSecret")) jwtSecretKey = server.arg("jwtSecret");
  if (server.hasArg("webUser")) webUsername = server.arg("webUser");
  if (server.hasArg("webPass")) webPassword = server.arg("webPass");
  if (server.hasArg("sensInt")) sensorInterval = server.arg("sensInt").toInt();
  
  // Clear cached JWT when secret key changes
  cachedJWT = "";
  jwtGeneratedAt = 0;
  
  savePrefs();
  
  server.sendHeader("Location", "/config?success=1");
  server.send(302);
}

void setupWebServerRoutes() {
  // Tell the server to collect the Cookie header
  const char* headerKeys[] = {"Cookie"};
  server.collectHeaders(headerKeys, 1);
  
  server.on("/", HTTP_GET, handleRoot);
  server.on("/login", HTTP_GET, handleLogin);
  server.on("/login", HTTP_POST, handleLoginPost);
  server.on("/logout", HTTP_GET, handleLogout);
  server.on("/diagnostics", HTTP_GET, handleDiagnostics);
  server.on("/sensors", HTTP_GET, handleSensorsPage);
  server.on("/relays", HTTP_GET, handleRelaysPage);
  server.on("/relays/set", HTTP_POST, handleRelaysSet);
  server.on("/config", HTTP_GET, handleConfigPage);
  server.on("/config/save", HTTP_POST, handleConfigSave);
  server.onNotFound([](){
    server.send(404, "text/plain", "Not found");
  });
  server.begin();
}

void saveConfigCallback () {
}


// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(500);
  
  // Initialize random seed for session tokens
  randomSeed(analogRead(0));

  // Pins
  pinMode(LED_WIFI_PIN, OUTPUT);
  pinMode(BTN_WIFI_RESET, INPUT_PULLUP);
  pinMode(BTN_OK, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW); // Initialize buzzer OFF

  for (int i=0;i<8;i++) {
    pinMode(relayPins[i], OUTPUT);
    setRelayState(i, 0); // Initialize all relays to OFF
  }

  // Start peripherals
  dht.begin();
  ds18b20.begin();
  lcd.init();
  lcd.backlight();

  // Initialize LittleFS for sensor data storage
  if (!LittleFS.begin(true)) {
  } else {
  }

  loadPrefs();

  // Display startup splash screen - Step 1: Company branding
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Orbiq Labs IoT");
  lcd.setCursor(0,1);
  lcd.print("iot.orbiqlabs.com");
  delay(5000); // Show for 5 seconds

  // Display startup splash screen - Step 2: Device info
  lcd.clear();
  lcd.setCursor(0,0);
  String deviceLine = "ID:" + device_id;
  if (deviceLine.length() > 16) deviceLine = deviceLine.substring(0, 16);
  lcd.print(deviceLine);
  lcd.setCursor(0,1);
  String versionLine = "Ver:" + String(FIRMWARE_VERSION);
  if (versionLine.length() > 16) versionLine = versionLine.substring(0, 16);
  lcd.print(versionLine);
  delay(3000); // Show for 3 seconds

  // Show WiFi configuration info (only if not already connected)
  if (WiFi.status() != WL_CONNECTED) {
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("SSID:ESP32_Cfg");
    lcd.setCursor(0,1);
    lcd.print("Pass:12345678");
  }
  
  // WiFiManager for setup
  wifiManager.setTimeout(600); // captive portal times out after 10 minutes

  wifiManager.setSaveConfigCallback(saveConfigCallback);
  bool res = wifiManager.autoConnect("ESP32_ConfigAP", "12345678");  // AP SSID, password

  if (!res) {
    Serial.println("❌ Failed to connect. Restarting...");
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("WiFi Failed!");
    lcd.setCursor(0,1);
    lcd.print("Restarting...");
    delay(2000);
    ESP.restart();
  } else {
    Serial.println("✅ WiFi connected");
    Serial.print("SSID: "); Serial.println(WiFi.SSID());
    Serial.print("IP: ");   Serial.println(WiFi.localIP());
    Serial.print("Version: "); Serial.println(FIRMWARE_VERSION);
    digitalWrite(LED_WIFI_PIN, HIGH);

    // Display WiFi connection success
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print(WiFi.SSID());
    lcd.setCursor(0,1);
    lcd.print(WiFi.localIP());
    delay(2000); // Show IP for 2 seconds

    // Display sensor initialization message and actively read sensors
    Serial.println("🔄 Initializing sensors and syncing with backend...");
    lcd.setCursor(0,0);
    lcd.print("Sensors loading");
    lcd.setCursor(0,1);
    lcd.print("Please Wait.....");
    
    unsigned long sensorInitStart = millis();
    
    while (millis() - sensorInitStart < 30000) { // 30 seconds
      // Read sensors
      float t = dht.readTemperature();
      float h = dht.readHumidity();
      ds18b20.requestTemperatures();
      float t2 = ds18b20.getTempCByIndex(0);
      // Wait 5 seconds before next reading
      delay(5000);
      }
      
    digitalWrite(BUZZER_PIN, HIGH);
    delay(500);
    digitalWrite(BUZZER_PIN, LOW);
  }

  Serial.println("Connected to WiFi: " + WiFi.SSID());
  digitalWrite(LED_WIFI_PIN, HIGH);

  // Configure NTP time synchronization
  Serial.println("⏰ Configuring NTP time synchronization...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("✅ NTP configured");

  // Start web server
  setupWebServerRoutes();

  // Connect to MQTT broker
  connectToMQTT();

  // Sync relay states from backend on startup
  Serial.println("🔄 Initial relay state sync from backend...");
  fetchRelayStateFromBackend();

  // Check for and send any stored sensor data from previous sessions
  checkAndSendStoredDataOnStartup();

  // ensure we have credentials persisted
  savePrefs();

  lastSensorMillis = millis() - sensorInterval; // immediate read
  
  // Initialize connection state tracking
  wasWiFiConnected = (WiFi.status() == WL_CONNECTED);
  wasMqttConnected = mqttClient.connected();
}

// ---------- Loop ----------
void cycleLCD() {
  static unsigned long lastCycle = 0;
  static int slide = 0;
  
  // If in info message mode, show the info message and don't cycle
  if (infoMessageMode) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("INFO:");
    lcd.setCursor(0, 1);
    // Display up to 16 characters of the info message
    if (infoMessage.length() <= 16) {
      lcd.print(infoMessage);
    } else {
      lcd.print(infoMessage.substring(0, 16));
    }
    delay(2000); // Show info message for 2 seconds
    return; // Don't cycle through normal screens
  }
  
  // If in custom message mode, show the custom message
  if (customMessageMode) {
    // Check if custom message has expired (5 minutes)
    if (millis() - customMessageReceivedAt >= CUSTOM_MESSAGE_TIMEOUT) {
      customMessageMode = false;
      customMessage = "";
      Serial.println("⏱️ Custom message timeout - returning to normal display");
      lcd.clear(); // Clear the message before returning to normal mode
      lastCycle = 0; // Reset cycle to immediately show normal display
      return;
    }
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("MSG:");
    lcd.setCursor(0, 1);
    // Display up to 16 characters of the custom message
    if (customMessage.length() <= 16) {
      lcd.print(customMessage);
    } else {
      lcd.print(customMessage.substring(0, 16));
    }
    return; // Don't cycle through normal screens
  }
  
  if (millis() - lastCycle < 5000) return; // change every 5s
  lastCycle = millis();

  // Get current date and time
  struct tm timeinfo;
  char dateTimeStr[16]; // "12Nov15:30" = 9 chars + null terminator + space for WiFi
  if (getLocalTime(&timeinfo)) {
    strftime(dateTimeStr, sizeof(dateTimeStr), "%d%b%y %H:%M", &timeinfo);
  } else {
    strcpy(dateTimeStr, "00XXX00:00");
  }

  // Get WiFi connection indicator (only show if connected)
  String wifiIndicator = "";
  if (WiFi.status() == WL_CONNECTED) {
    wifiIndicator = " W"; // Simple WiFi connected symbol
  }

  // Line 1: Always show date/time and WiFi indicator
  lcd.clear();
  lcd.setCursor(0,0);
  String displayStr = String(dateTimeStr) + wifiIndicator;
  if (displayStr.length() > 16) displayStr = displayStr.substring(0, 16);
  lcd.print(displayStr);
  
  // Line 2: Cycle through single values
  switch (slide) {
    case 0: {
      // Show temperature in both C and F
      ds18b20.requestTemperatures();
      float t2 = ds18b20.getTempCByIndex(0);
      if (t2 != DEVICE_DISCONNECTED_C) {
        float tempF = ds18b20.toFahrenheit(t2);
        String tempStr = "Temp:" + String(t2, 1) + "C " + String(tempF, 1) + "F";
        if (tempStr.length() > 16) tempStr = tempStr.substring(0, 16);
        lcd.setCursor(0,1);
        lcd.print(tempStr);
      } else {
        lcd.setCursor(0,1);
        lcd.print("Temp:--.-C --.-F");
      }
      break;
    }
    case 1: {
      // Show humidity
      float h = dht.readHumidity();
      if (!isnan(h)) {
        String humStr = "Humidity: " + String(h, 0) + "%";
        if (humStr.length() > 16) humStr = humStr.substring(0, 16);
        lcd.setCursor(0,1);
        lcd.print(humStr);
      } else {
        lcd.setCursor(0,1);
        lcd.print("Humidity: --%");
      }
      break;
    }
    case 2: {
      // Show relay status
      String relayBits = "";
      for (int i = 0; i < 8; i++) relayBits += String(relayState[i]);
      String relayStr = "Relays:" + relayBits;
      if (relayStr.length() > 16) relayStr = relayStr.substring(0, 16);
      lcd.setCursor(0,1);
      lcd.print(relayStr);
      break;
    }
    case 3: {
      // Show IP address
      String ipStr = "IP:" + WiFi.localIP().toString();
      if (ipStr.length() > 16) ipStr = ipStr.substring(0, 16);
      lcd.setCursor(0,1);
      lcd.print(ipStr);
      break;
    }
  }
  
  slide = (slide + 1) % 4; // Cycle through 4 different displays
}

void handleButtons() {
  
  //Serial.println("handleButtons called");
  // WiFi reset long press (BTN_WIFI_RESET) - simplified logic
  static unsigned long buttonPressStart = 0;
  static bool buttonWasPressed = false;

  bool buttonPressed = (digitalRead(BTN_WIFI_RESET) == LOW); // active low

  if (buttonPressed && !buttonWasPressed) {
    // Button just pressed
    buttonPressStart = millis();
    buttonWasPressed = true;
    Serial.println("[" + getTimestamp() + "] WiFi reset button pressed - hold for 5 seconds to reset");
  } else if (!buttonPressed && buttonWasPressed) {
    // Button just released
    buttonWasPressed = false;
    unsigned long pressDuration = millis() - buttonPressStart;

    if (pressDuration >= WIFI_RESET_HOLD_MS) {
      // Long press detected - reset WiFi
      Serial.println("[" + getTimestamp() + "] WiFi reset confirmed - resetting settings");
      resetWifiSettings();
    } else {
      Serial.println("[" + getTimestamp() + "] Button released too quickly - no reset");
    }
  } else if (buttonPressed && buttonWasPressed) {
    // Button still being held
    unsigned long holdTime = millis() - buttonPressStart;
    if (holdTime >= WIFI_RESET_HOLD_MS && holdTime - (holdTime / 1000) * 1000 < 50) {
      // Print progress every second while holding
      Serial.println("[" + getTimestamp() + "] Holding... " + String(holdTime / 1000) + "s / 5s");
    }
  }

  // OK button handling for Info messages
  static bool okButtonWasPressed = false;
  bool okButtonPressed = (digitalRead(BTN_OK) == LOW); // active low

  if (okButtonPressed && !okButtonWasPressed && infoMessageMode) {
    // OK button just pressed while in info mode
    okButtonWasPressed = true;
    Serial.println("[" + getTimestamp() + "] OK button pressed - exiting Info message mode");
    infoMessageMode = false;
    infoMessage = "";
    Serial.println("✅ Info message acknowledged - resuming normal LCD cycling");
  } else if (!okButtonPressed && okButtonWasPressed) {
    // OK button just released
    okButtonWasPressed = false;
  }
}


void loop() {
  // Check WiFi connection status and handle reconnection
  bool isWiFiConnected = (WiFi.status() == WL_CONNECTED);
  if (!wasWiFiConnected && isWiFiConnected) {
    Serial.println("📶 WiFi reconnected! SSID: " + WiFi.SSID() + "   IP: " + WiFi.localIP().toString() + "⏰ Reconfiguring NTP time synchronization...");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    fetchRelayStateFromBackend();
  }
  wasWiFiConnected = isWiFiConnected;

  // Maintain MQTT connection and detect reconnection
  bool isMqttConnected = mqttClient.connected();
  if (!isMqttConnected && (millis() - lastMqttReconnectAttempt >= MQTT_RECONNECT_INTERVAL)) {
    lastMqttReconnectAttempt = millis();
    connectToMQTT();
    isMqttConnected = mqttClient.connected();
  }
  
  // MQTT connection state is now tracked inside connectToMQTT() 
  // which calls fetchRelayStateFromBackend() on successful reconnection
  
  mqttClient.loop();
  
  server.handleClient();
  handleButtons();
  cycleLCD();

  // LED wifi indicator
  if (WiFi.status() == WL_CONNECTED) digitalWrite(LED_WIFI_PIN, HIGH); else digitalWrite(LED_WIFI_PIN, LOW);

  // Periodic sensor read & save to file
  if (millis() - lastSensorMillis >= sensorInterval) {
    lastSensorMillis = millis();
    Serial.println("[" + getTimestamp() + "] Reading sensors...");
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    ds18b20.requestTemperatures();
    float t2 = ds18b20.getTempCByIndex(0);

    // Validate readings (DHT can sometimes return NAN)
    if (!isnan(t)){
      SensorReading reading = {SENSOR_DHT11_TEMP, t, "°C"};
      saveSensorReadingToFile(reading);
      Serial.println("[" + getTimestamp() + "] DHT11_Temperature: " + String(t, 2) + " °C");
    } 
    if (!isnan(h)) {  
      SensorReading reading = {SENSOR_DHT11_HUMIDITY, h, "%"};
      saveSensorReadingToFile(reading);
      Serial.println("[" + getTimestamp() + "] DHT11_Humidity: " + String(h, 1) + " %");
    }
    if (t2 != DEVICE_DISCONNECTED_C)
    {
      SensorReading reading = {SENSOR_DS18B20_TEMP, t2, "°C"};
      saveSensorReadingToFile(reading);
      float tempF = ds18b20.toFahrenheit(t2);
      Serial.println("[" + getTimestamp() + "] DS18B20 Temperature: " + String(t2, 2) + " °C / " + String(tempF, 2) + " °F");
    }
  }

  // Periodic data sync (every 5 minutes)
  if (millis() - lastDataSyncMillis >= dataSyncInterval) {
    lastDataSyncMillis = millis();
    Serial.println("[" + getTimestamp() + "] 🔄 Periodic data sync - sending stored sensor data");
    if (sendStoredSensorData()) {
      Serial.println("[" + getTimestamp() + "] ✅ Periodic data sync completed successfully");
    } else {
      Serial.println("[" + getTimestamp() + "] ❌ Periodic data sync failed - will retry later");
    }
  }

  // slight delay
  delay(10);
}
