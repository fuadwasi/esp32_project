/*
  ESP32 IoT System - Single-file sketch
  Version: 1.0.3
  Last Updated: 2025-11-09
  
  Changelog:
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
  
  Pin assignments (change if needed):
    - I2C SDA = 21, SCL = 22
    - DHT11_PIN = 19
    - DS18B20_PIN = 4
    - LED_WIFI_PIN = 2
    - Buttons: BTN_WIFI_RESET = 35, BTN_NAV = 0, BTN_CFG = 5, BTN_TOGGLE = 18
    - Relays: relayPins[8] = {13, 12, 14, 27, 26, 25, 33, 32}
*/

#define FIRMWARE_VERSION "1.0.3"

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

// ---------- CONFIG ----------
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

const int BTN_WIFI_RESET = 23; // input only (use with pullup)
const int BTN_NAV = 19;
const int BTN_CFG = 17;
const int BTN_TOGGLE = 18;

// Relay pins (change to match your wiring)
const int relayPins[8] = {12, 13, 14, 27, 26, 25, 33, 32};

// Relay module type: true = Active LOW (most common), false = Active HIGH
// Active LOW means: LOW = relay ON, HIGH = relay OFF
const bool RELAY_ACTIVE_LOW = true;

// Sensor intervals (ms)
unsigned long sensorInterval = 30UL * 1000UL; // default 30s; configurable
unsigned long lastSensorMillis = 0;

// WiFi and MQTT connection state tracking
bool wasWiFiConnected = false;
bool wasMqttConnected = false;

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

// Local app modes & UI state
bool navMode = false;
unsigned long navModeStarted = 0;
const unsigned long NAV_TIMEOUT = 30UL * 1000UL; // 30s

bool cfgMode = false;

// Buttons debounce & press detection
unsigned long btnWifiPressStart = 0;
bool btnWifiPressed = false;
const unsigned long WIFI_RESET_HOLD_MS = 5000; // 5 seconds

// Sensor buffer for retrying posts
struct SensorReading {
  String name;
  float value;
  String unit;
};
std::vector<SensorReading> pendingReadings;

// Session management
String sessionToken = "";
unsigned long sessionCreatedAt = 0;
const unsigned long SESSION_TIMEOUT = 3600UL * 1000UL; // 1 hour session timeout

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
    Serial.println("Session validation failed: empty token");
    return false;
  }
  if (token != sessionToken) {
    Serial.println("Session validation failed: token mismatch");
    Serial.println("Expected: " + sessionToken);
    Serial.println("Received: " + token);
    return false;
  }
  
  // Check if session has expired
  unsigned long currentTime = millis();
  if (currentTime - sessionCreatedAt > SESSION_TIMEOUT) {
    Serial.println("Session validation failed: expired");
    sessionToken = ""; // Clear expired session
    return false;
  }
  
  Serial.println("Session validation successful");
  return true;
}

String getSessionCookie() {
  // Debug: Print all headers
  Serial.println("=== All HTTP Headers ===");
  Serial.println("Headers count: " + String(server.headers()));
  for (int i = 0; i < server.headers(); i++) {
    Serial.println("Header[" + String(i) + "]: " + server.headerName(i) + " = " + server.header(i));
  }
  Serial.println("========================");
  
  if (server.hasHeader("Cookie")) {
    String cookies = server.header("Cookie");
    Serial.println("Cookies received: " + cookies);
    int idx = cookies.indexOf("ESPSESSION=");
    if (idx >= 0) {
      int endIdx = cookies.indexOf(';', idx);
      if (endIdx < 0) endIdx = cookies.length();
      String token = cookies.substring(idx + 11, endIdx);
      token.trim(); // Remove any whitespace
      Serial.println("Session token from cookie: " + token);
      return token;
    }
  }
  Serial.println("No session cookie found");
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
  
  Serial.println("Generated JWT: " + jwt);
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
    Serial.println("New JWT generated");
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

void checkAndResetWifiSettings() {
  unsigned long held = millis() - btnWifiPressStart;
    if (held >= WIFI_RESET_HOLD_MS) {
      // do resetlcd.clear();
      resetWifiSettings();
    }
}

// ---------- IoT Device API Functions ----------

bool postSensorReading(const String &sensorId, float numericValue, const String &textValue) {
  HTTPClient http;
  String url = String(BACKEND_BASE_URL) + SENSOR_POST_ENDPOINT;
  
  Serial.println("Posting sensor reading to: " + url);
  Serial.println("Device ID: " + device_id);
  
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-ESP32-Device-Id", device_id);
  
  String jwt = getValidJWT();
  http.addHeader("X-IOT-Token", jwt);
  Serial.println("JWT Token: " + jwt.substring(0, 20) + "...");
  
  StaticJsonDocument<256> doc;
  doc["sensorIdentifier"] = sensorId;
  doc["numericValue"] = numericValue;
  doc["textValue"] = textValue;
  
  String payload;
  serializeJson(doc, payload);
  
  Serial.println("Request payload: " + payload);
  
  int code = http.POST(payload);
  if (code > 0) {
    String resp = http.getString();
    Serial.printf("Sensor POST response (%d): %s\n", code, resp.c_str());
    http.end();
    
    if (code == 404) {
      Serial.println("⚠️ 404 Error - Device may not be registered in backend or wrong endpoint");
      Serial.println("Please verify:");
      Serial.println("1. Device ID '" + device_id + "' is registered in the backend");
      Serial.println("2. JWT secret matches the backend configuration");
      Serial.println("3. API endpoint is correct: " + url);
    }
    
    return (code >= 200 && code < 300);
  } else {
    Serial.printf("Sensor POST failed, error: %s\n", http.errorToString(code).c_str());
    http.end();
    return false;
  }
}

bool getDeviceStatus(String &statusOut, bool &onlineOut) {
  HTTPClient http;
  String url = String(BACKEND_BASE_URL) + GET_STATUS_ENDPOINT;
  
  Serial.println("Getting device status from: " + url);
  
  http.begin(url);
  http.addHeader("X-ESP32-Device-Id", device_id);
  http.addHeader("X-IOT-Token", getValidJWT());
  
  int code = http.GET();
  if (code > 0) {
    String resp = http.getString();
    Serial.printf("Get Status response (%d): %s\n", code, resp.c_str());
    
    if (code == 200) {
      // Parse JSON response
      StaticJsonDocument<512> doc;
      DeserializationError error = deserializeJson(doc, resp);
      
      if (!error) {
        statusOut = doc["status"].as<String>();
        onlineOut = doc["online"].as<bool>();
        http.end();
        return true;
      }
    }
  } else {
    Serial.printf("Get Status failed, error: %s\n", http.errorToString(code).c_str());
  }
  http.end();
  return false;
}

bool submitDeviceStatus(const String &statusString) {
  HTTPClient http;
  String url = String(BACKEND_BASE_URL) + POST_STATUS_ENDPOINT;
  
  Serial.println("Submitting device status to: " + url);
  
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-ESP32-Device-Id", device_id);
  http.addHeader("X-IOT-Token", getValidJWT());
  
  StaticJsonDocument<128> doc;
  doc["status"] = statusString;
  
  String payload;
  serializeJson(doc, payload);
  
  Serial.println("Status payload: " + payload);
  
  int code = http.POST(payload);
  if (code > 0) {
    String resp = http.getString();
    Serial.printf("Submit Status response (%d): %s\n", code, resp.c_str());
    http.end();
    return (code >= 200 && code < 300);
  } else {
    Serial.printf("Submit Status failed, error: %s\n", http.errorToString(code).c_str());
    http.end();
    return false;
  }
}

bool pingDevice() {
  HTTPClient http;
  String url = String(BACKEND_BASE_URL) + PING_ENDPOINT;
  
  Serial.println("Pinging device to: " + url);
  
  http.begin(url);
  http.addHeader("X-ESP32-Device-Id", device_id);
  http.addHeader("X-IOT-Token", getValidJWT());
  
  int code = http.POST("");
  if (code > 0) {
    String resp = http.getString();
    Serial.printf("Ping response (%d): %s\n", code, resp.c_str());
    http.end();
    return (code >= 200 && code < 300);
  } else {
    Serial.printf("Ping failed, error: %s\n", http.errorToString(code).c_str());
    http.end();
    return false;
  }
}

void setRelaysFromBackendJSON(const String &json) {
  // naive parser expecting "Relay": { "1": 0, "2": 1, ... }
  // This is simplistic, in production use ArduinoJson.
  for (int i=1;i<=8;i++) {
    String key = "\"" + String(i) + "\":";
    int idx = json.indexOf(key);
    if (idx >= 0) {
      int valStart = idx + key.length();
      if (valStart < (int)json.length()) {
        char c = json[valStart];
        if (c == ' ' || c == '\t') {
          // skip whitespace
          valStart++;
          if (valStart < (int)json.length()) c = json[valStart];
        }
        int v = (c == '1') ? 1 : 0;
        relayState[i-1] = v;
        setRelayState(i-1, v);
      }
    }
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
    // Trigger immediate sensor reading and send ping to API
    Serial.println("🏓 Ping received - triggering sensor read and API ping");
    lastSensorMillis = 0; // Force immediate sensor read in loop
    
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
              
              // If nested type is "Info", get the message and display on LCD
              if (nestedType == "Info") {
                int nestedMIdx = nestedData.indexOf("\"m\":");
                if (nestedMIdx >= 0) {
                  int nestedMsgStart = nestedData.indexOf('"', nestedMIdx + 4);
                  int nestedMsgEnd = nestedData.indexOf('"', nestedMsgStart + 1);
                  if (nestedMsgStart >= 0 && nestedMsgEnd >= 0) {
                    String infoMessage = nestedData.substring(nestedMsgStart + 1, nestedMsgEnd);
                    Serial.println("� Info message: " + infoMessage);
                    
                    // Display on LCD for 5 minutes
                    customMessage = infoMessage;
                    customMessageMode = true;
                    customMessageReceivedAt = millis();
                    Serial.println("✅ Info message displayed on LCD (will auto-clear in 5 minutes)");
                  }
                }
              } else {
                Serial.println("ℹ️ Nested type '" + nestedType + "' - no action taken");
              }
            }
          }
        }
      } else {
        // Data is a simple value, not an object
        Serial.println("📦 Simple data: " + msgData);
      }
    }
  }
  else {
    Serial.println("⚠️ Unknown message type: " + msgType);
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

bool ensureJWT() {
  // Deprecated - JWT is now generated locally
  // Kept for backward compatibility but always returns true
  return true;
}

bool postSensorReadings(const std::vector<SensorReading> &readings) {
  if (readings.empty()) return true;

  // Post each reading individually using the new API
  bool allSuccess = true;
  for (const auto &reading : readings) {
    bool success = postSensorReading(reading.name, reading.value, 
                                     String(reading.value, 2) + " " + reading.unit);
    if (!success) {
      allSuccess = false;
      Serial.println("Failed to post reading: " + reading.name);
    } else {
      Serial.println("Successfully posted: " + reading.name);
    }
    delay(100); // Small delay between requests
  }
  
  return allSuccess;
}

bool fetchRelayStateFromBackend() {
  // Get device status which includes relay states
  String status;
  bool online;
  
  if (getDeviceStatus(status, online)) {
    Serial.println("Device status: " + status + ", Online: " + String(online));
    
    // Parse status string (up to 8 characters representing relay states)
    if (status.length() > 0) {
      int numRelays = min(8, (int)status.length());
      Serial.println("Updating " + String(numRelays) + " relays from status string");
      
      for (int i = 0; i < numRelays; i++) {
        int val = (status[i] == '1') ? 1 : 0;
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
  Serial.println("Auth check result: " + String(isValid ? "PASS" : "FAIL"));
  return isValid;
}

void requireAuth() {
  if (!checkWebAuth()) {
    Serial.println("Authentication required - redirecting to login");
    server.sendHeader("Location", "/login");
    server.send(302);
  }
}

String htmlTemplate(const String &title, const String &body) {
  String s = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>" + title + "</title>";
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
  
  Serial.println("Login attempt for user: " + username);
  
  if (username == webUsername && password == webPassword) {
    // Create new session
    sessionToken = generateSessionToken();
    sessionCreatedAt = millis();
    
    Serial.println("Login successful! Session token: " + sessionToken);
    
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
    
    Serial.println("Cookie set: " + cookie);
    Serial.println("Redirecting to /sensors via JavaScript");
  } else {
    // Login failed
    Serial.println("Login failed - invalid credentials");
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
  
  Serial.println("User logged out");
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
  Serial.println("Root page requested");
  if (!checkWebAuth()) {
    requireAuth();
    return;
  }
  
  Serial.println("Auth passed - showing home page");
  
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
  Serial.println("Sensors page requested");
  if (!checkWebAuth()) {
    requireAuth();
    return;
  }
  
  Serial.println("Auth passed - showing sensors page");
  
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
  int tempPercent = constrain(map((int)t, 0, 50, 0, 100), 0, 100);
  b += "<div class='bar-label'>" + String(tempPercent) + "%</div>";
  b += "<div class='bar-fill' style='width:" + String(tempPercent) + "%;background:linear-gradient(90deg,#FF6B6B,#FF8E53);'></div>";
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
  int temp2Percent = constrain(map((int)t2, 0, 50, 0, 100), 0, 100);
  b += "<div class='bar-label'>" + String(temp2Percent) + "%</div>";
  b += "<div class='bar-fill' style='width:" + String(temp2Percent) + "%;background:linear-gradient(90deg,#FFA726,#FB8C00);'></div>";
  b += "</div></div>";
  
  b += "<p style='margin-top:20px;'><a href='/'>← Back to Home</a> | <a href='/relays'>View Relays →</a></p>";
  
  server.send(200, "text/html", htmlTemplate("Live Sensors", b));
}

void handleRelaysPage() {
  Serial.println("Relays page requested");
  if (!checkWebAuth()) {
    requireAuth();
    return;
  }
  
  Serial.println("Auth passed - showing relays page");
  
  String b = "<form method='POST' action='/relays/set'>";
  b += "<div style='display:grid;grid-template-columns:1fr;gap:10px;'>";
  for (int i=0;i<8;i++) {
    b += "<div style='padding:10px;background:#f9f9f9;border-radius:5px;'>";
    b += "<label style='display:flex;align-items:center;cursor:pointer;'>";
    b += "<input type='checkbox' name='r" + String(i+1) + "' " + (relayState[i] ? "checked" : "") + " style='margin-right:10px;width:20px;height:20px;'>";
    b += "<span style='font-size:16px;'>Relay " + String(i+1) + " - <strong>" + String(relayState[i] ? "ON" : "OFF") + "</strong></span>";
    b += "</label></div>";
  }
  b += "</div>";
  b += "<br/><input type='submit' value='Update Relays' style='width:100%;padding:15px;font-size:16px;'></form>";
  b += "<p style='margin-top:20px;'><a href='/'>← Back to Home</a> | <a href='/sensors'>View Sensors →</a></p>";
  
  server.send(200, "text/html", htmlTemplate("Relay Control", b));
}

void handleRelaysSet() {
  Serial.println("Relay update requested");
  if (!checkWebAuth()) {
    requireAuth();
    return;
  }
  
  Serial.println("Auth passed - updating relays");
  
  // update relayState according to checkboxes
  for (int i=0;i<8;i++) {
    String key = "r" + String(i+1);
    bool checked = server.hasArg(key);
    relayState[i] = checked ? 1 : 0;
    setRelayState(i, relayState[i]);
    Serial.println("Relay " + String(i+1) + " set to: " + String(relayState[i]));
  }
  
  // Submit updated relay status to backend
  postRelayUpdateToBackend(0, 0); // Index not used, submits all relays
  
  Serial.println("Redirecting to /relays with success notification");
  server.sendHeader("Location", "/relays?updated=1");
  server.send(302);
}

void handleConfigPage() {
  Serial.println("Config page requested");
  if (!checkWebAuth()) {
    requireAuth();
    return;
  }
  
  Serial.println("Auth passed - showing config page");
  
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
  Serial.println("Config save requested");
  if (!checkWebAuth()) {
    requireAuth();
    return;
  }
  
  Serial.println("Auth passed - saving config");
  
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
  
  Serial.println("Config saved successfully");
  Serial.println("Redirecting to /config with success notification");
  
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
  Serial.println("WiFi credentials were just saved!");
}


// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  Serial.println("ESP32 IoT System");
  Serial.println("Version: " + String(FIRMWARE_VERSION));
  Serial.println("Starting up...");
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

  // Initialize random seed for session tokens
  randomSeed(analogRead(0));

  // Pins
  pinMode(LED_WIFI_PIN, OUTPUT);
  pinMode(BTN_WIFI_RESET, INPUT_PULLUP);
  pinMode(BTN_NAV, INPUT_PULLUP);
  pinMode(BTN_CFG, INPUT_PULLUP);
  pinMode(BTN_TOGGLE, INPUT_PULLUP);


  for (int i=0;i<8;i++) {
    pinMode(relayPins[i], OUTPUT);
    setRelayState(i, 0); // Initialize all relays to OFF
  }

  // Start peripherals
  dht.begin();
  ds18b20.begin();
  lcd.init();
  lcd.backlight();

  loadPrefs();

  // Display startup splash screen
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.write(0xF4); // Globe/world icon (custom character or built-in)
  lcd.print(" OrbiqLabs");
  lcd.setCursor(0,1);
  lcd.print("IoT Device 001");
  delay(2000); // Show splash for 2 seconds

  // Show WiFi configuration info
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("SSID:ESP32_Cfg");
  lcd.setCursor(0,1);
  lcd.print("Pass:12345678");
  
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
  }

  Serial.println("Connected to WiFi: " + WiFi.SSID());
  digitalWrite(LED_WIFI_PIN, HIGH);

  // Start web server
  setupWebServerRoutes();

  // Connect to MQTT broker
  connectToMQTT();

  // Sync relay states from backend on startup
  Serial.println("🔄 Initial relay state sync from backend...");
  fetchRelayStateFromBackend();

  // ensure we have credentials persisted
  savePrefs();

  lastSensorMillis = millis() - sensorInterval; // immediate read
  
  // Initialize connection state tracking
  wasWiFiConnected = (WiFi.status() == WL_CONNECTED);
  wasMqttConnected = mqttClient.connected();
  
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  Serial.println("✅ System Ready!");
  Serial.println("Firmware Version: " + String(FIRMWARE_VERSION));
  Serial.println("Device ID: " + device_id);
  Serial.println("IP Address: " + WiFi.localIP().toString());
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
}

// ---------- Loop ----------
void cycleLCD() {
  static unsigned long lastCycle = 0;
  static int slide = 0;
  
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
  
  if (millis() - lastCycle < 4000) return; // change every 4s
  lastCycle = millis();

  if (navMode) {
    // show relay statuses one by one
    int idx = (millis() - navModeStarted) / 3000; // each 3s
    idx = idx % 8;
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Relay " + String(idx+1));
    lcd.setCursor(0,1);
    lcd.print(relayState[idx] ? "ON" : "OFF");
    return;
  }

  // normal cycling: sensors -> relays
  if (slide == 0) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("DHT " + String(t,1) + "C");
    lcd.setCursor(0,1);
    lcd.print(String(h,0) + "% RH");
    slide = 1;
  } else if (slide == 1) {
    ds18b20.requestTemperatures();
    float t2 = ds18b20.getTempCByIndex(0);
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("DS18B20:");
    lcd.setCursor(0,1);
    lcd.print(String(t2,2) + " C");
    slide = 2;
  } else {
    // show summary of relays as bits
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("R:");
    String bits = "";
    for (int i=0;i<8;i++) bits += (relayState[i] ? "1" : "0");
    lcd.setCursor(2,0);
    lcd.print(bits);
    slide = 0;
  }
}

void handleButtons() {
  // WiFi reset long press (BTN_WIFI_RESET)
  bool pressed = (digitalRead(BTN_WIFI_RESET) == LOW); // active low
  if (pressed && !btnWifiPressed) {
    btnWifiPressed = true;
    btnWifiPressStart = millis();
    Serial.println("WiFi reset long press");
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("WiFi reset. press");
    lcd.setCursor(0,1);
    lcd.print("5s to reset");
  } else if (!pressed && btnWifiPressed) {
    btnWifiPressed = false;
    checkAndResetWifiSettings();
  } else if (pressed && btnWifiPressed) {
    lcd.clear();
    // still holding — nothing to do here; actual resetting is on release if hold met
    Serial.println("still holding — nothing to do here; actual resetting is on release if hold met");
    checkAndResetWifiSettings();
  }

  // Navigation button: BTN_NAV
  if (digitalRead(BTN_NAV) == LOW) {
  Serial.println("digitalRead(BTN_NAV) == LOW");
  Serial.println(digitalRead(BTN_NAV) == LOW);
    // start nav mode
    navMode = true;
    navModeStarted = millis();
  }
  if (navMode && (millis() - navModeStarted > NAV_TIMEOUT)) {
    navMode = false;
  }

  // Config mode: BTN_CFG toggles cfgMode
  static bool lastCfgState = HIGH;
  bool cfgState = digitalRead(BTN_CFG);
  if (cfgState == LOW && lastCfgState == HIGH) {
    // pressed
    cfgMode = !cfgMode;
    Serial.println(String("CFG mode: ") + (cfgMode ? "ON":"OFF"));
  }
  lastCfgState = cfgState;

  // Toggle button: BTN_TOGGLE toggles currently selected relay in nav/cfg mode
  // Also clears custom message mode when pressed
  static bool lastToggleState = HIGH;
  bool tState = digitalRead(BTN_TOGGLE);
  if (tState == LOW && lastToggleState == HIGH) {
    // pressed
    if (customMessageMode) {
      // Exit custom message mode
      customMessageMode = false;
      customMessage = "";
      Serial.println("Custom message mode cleared by BTN_TOGGLE");
    } else if (cfgMode) {
      // in cfg mode toggle currently focused relay (if nav mode use focused idx)
      int focusIdx = 0;
      if (navMode) {
        focusIdx = ((millis() - navModeStarted) / 3000) % 8;
      } else {
        // default focus 0
        focusIdx = 0;
      }
      relayState[focusIdx] = relayState[focusIdx] ? 0 : 1;
      setRelayState(focusIdx, relayState[focusIdx]);
      // Update backend with new relay state
      postRelayUpdateToBackend(focusIdx, relayState[focusIdx]);
      Serial.println("Relay " + String(focusIdx) + " toggled locally to " + String(relayState[focusIdx]));
    } else {
      Serial.println("Toggle pressed but not in cfgMode or customMessageMode.");
    }
  }
  lastToggleState = tState;

  
}


void loop() {
  // Check WiFi connection status and handle reconnection
  bool isWiFiConnected = (WiFi.status() == WL_CONNECTED);
  if (!wasWiFiConnected && isWiFiConnected) {
    // WiFi just reconnected
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.println("📶 WiFi reconnected!");
    Serial.println("   SSID: " + WiFi.SSID());
    Serial.println("   IP: " + WiFi.localIP().toString());
    Serial.println("🔄 Syncing relay states from backend after WiFi reconnection...");
    fetchRelayStateFromBackend();
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  }
  wasWiFiConnected = isWiFiConnected;

  // Maintain MQTT connection and detect reconnection
  bool isMqttConnected = mqttClient.connected();
  if (!isMqttConnected) {
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

  // Periodic sensor read & send
  if (millis() - lastSensorMillis >= sensorInterval) {
    lastSensorMillis = millis();
    Serial.println("Reading sensors...");
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    ds18b20.requestTemperatures();
    float t2 = ds18b20.getTempCByIndex(0);

    // Validate readings (DHT can sometimes return NAN)
    std::vector<SensorReading> readings;
    if (!isnan(t)){
      readings.push_back( {"DHT11_Temperature", t, "°C"} );
      Serial.print("DHT11_Temperature: ");
      Serial.print(t);
      Serial.println(" °C");
    } 
    if (!isnan(h)) {  
      readings.push_back( {"DHT11_Humidity", h, "%"} );
      Serial.print("DHT11_Humidity: ");
      Serial.print(h);
      Serial.println(" %");
    }
    if (t2 != DEVICE_DISCONNECTED_C)
    {
      readings.push_back( {"DS18B20_Temperature", t2, "°C"} );
       
      float tempF = ds18b20.toFahrenheit(t2);
      Serial.print("DS18B20 Temperature: ");
      Serial.print(t2);
      Serial.println(" °C");
      Serial.print("DS18B20 Temperature: ");
      Serial.print(tempF);
      Serial.println(" °F");
    }

    // try send
    bool ok = postSensorReadings(readings);
    if (!ok) {
      // buffer readings for retry
      Serial.println("Buffering sensor readings for retry.");
      for (auto &r : readings) pendingReadings.push_back(r);
    } else {
      // if succeeded attempt send pending buffer too
      if (!pendingReadings.empty()) {
        std::vector<SensorReading> buffer = pendingReadings;
        if (postSensorReadings(buffer)) {
          pendingReadings.clear();
        } else {
          Serial.println("Still failed to send buffered readings.");
        }
      }
    }
  }

  // slight delay
  delay(10);
}
