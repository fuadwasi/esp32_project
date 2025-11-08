/*
  ESP32 IoT System - Single-file sketch
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
#define BACKEND_BASE_URL "https://iot.orbiqlabs.com/api-iot/device/" // IoT Device API base URL
#define SENSOR_POST_ENDPOINT "sensor-readings"
#define GET_STATUS_ENDPOINT "status"
#define POST_STATUS_ENDPOINT "status"
#define PING_ENDPOINT "ping"

#define DEVICE_ID_DEFAULT "esp32-default-device"
#define JWT_SECRET_DEFAULT "orbiqlabs_iot_key"
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
const int relayPins[8] = {13, 12, 14, 27, 26, 25, 33, 32};

// Sensor intervals (ms)
unsigned long sensorInterval = 30UL * 1000UL; // default 30s; configurable
unsigned long lastSensorMillis = 0;

// Relay status sync interval
unsigned long relaySyncInterval = 120UL * 1000UL; // 120s
unsigned long lastRelaySyncMillis = 0;

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
  if (token.length() == 0 || sessionToken.length() == 0) return false;
  if (token != sessionToken) return false;
  
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
      return cookies.substring(idx + 11, endIdx);
    }
  }
  return "";
}

// ---------- Helpers ----------

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
  
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-ESP32-Device-Id", device_id);
  http.addHeader("X-IOT-Token", getValidJWT());
  
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
        digitalWrite(relayPins[i-1], v ? HIGH : LOW);
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
  
  Serial.print("MQTT message received on topic: ");
  Serial.print(topic);
  Serial.print(" | Message: ");
  Serial.println(message);

  // Parse JSON: {"t":"Type","d":"Data"}
  // Find type field
  int tIdx = message.indexOf("\"t\":");
  if (tIdx < 0) return;
  
  int typeStart = message.indexOf('"', tIdx + 4);
  int typeEnd = message.indexOf('"', typeStart + 1);
  if (typeStart < 0 || typeEnd < 0) return;
  
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

  Serial.print("Type: ");
  Serial.print(msgType);
  Serial.print(" | Data: ");
  Serial.println(msgData);

  // Handle message types
  if (msgType == "Status") {
    // Status: "01001001" - 8 relay states
    if (msgData.length() >= 8) {
      for (int i = 0; i < 8; i++) {
        int val = (msgData[i] == '1') ? 1 : 0;
        relayState[i] = val;
        digitalWrite(relayPins[i], val ? HIGH : LOW);
      }
      Serial.println("Relay states updated from MQTT Status message");
    }
  } 
  else if (msgType == "Message") {
    // Custom message to display on LCD
    customMessage = msgData;
    customMessageMode = true;
    customMessageReceivedAt = millis();
    Serial.println("Custom message set: " + customMessage);
  }
  else if (msgType == "Ping") {
    // Trigger immediate sensor reading and send ping to API
    Serial.println("Ping received - triggering sensor read and API ping");
    lastSensorMillis = 0; // Force immediate sensor read in loop
    
    // Send ping to backend API
    if (pingDevice()) {
      Serial.println("✅ Ping sent successfully to backend");
    } else {
      Serial.println("❌ Failed to send ping to backend");
    }
  }
  else if (msgType == "Sync") {
    // Sync message - could be used for device sync operations
    Serial.println("Sync message received: " + msgData);
  }
}

// ---------- MQTT Connection ----------
void connectToMQTT() {
  if (mqttClient.connected()) return;
  
  Serial.println("Connecting to MQTT broker...");
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  
  String clientId = "ESP32-" + device_id;
  
  if (mqttClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD)) {
    Serial.println("✅ MQTT connected!");
    
    // Subscribe to device-specific topic: devices/{device_id}/state
    String topic = "devices/" + device_id + "/state";
    if (mqttClient.subscribe(topic.c_str())) {
      Serial.println("Subscribed to topic: " + topic);
    } else {
      Serial.println("Failed to subscribe to topic: " + topic);
    }
  } else {
    Serial.print("❌ MQTT connection failed, rc=");
    Serial.println(mqttClient.state());
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
    
    // Parse status string (8 characters representing relay states)
    if (status.length() >= 8) {
      for (int i = 0; i < 8 && i < status.length(); i++) {
        int val = (status[i] == '1') ? 1 : 0;
        relayState[i] = val;
        digitalWrite(relayPins[i], val ? HIGH : LOW);
      }
      Serial.println("Relay states updated from backend");
      return true;
    }
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
  return isSessionValid(cookieToken);
}

void requireAuth() {
  if (!checkWebAuth()) {
    server.sendHeader("Location", "/login");
    server.send(303);
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
  s += "input[type=text],input[type=password],input[type=number]{width:100%;padding:8px;margin:5px 0 15px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box;}";
  s += "input[type=submit],button{background:#4CAF50;color:white;padding:10px 20px;border:none;border-radius:4px;cursor:pointer;}";
  s += "input[type=submit]:hover,button:hover{background:#45a049;}";
  s += "input[type=checkbox]{margin-right:5px;}";
  s += ".logout{float:right;background:#f44336;}";
  s += ".logout:hover{background:#da190b;}";
  s += "</style>";
  s += "</head><body><div class='container'>";
  if (checkWebAuth()) {
    s += "<a href='/logout' class='logout' style='float:right;'>Logout</a>";
  }
  s += "<h3>" + title + "</h3>";
  s += body;
  s += "<hr><small>Device: " + device_id + "</small></div></body></html>";
  return s;
}

void handleLogin() {
  // If already logged in, redirect to home
  if (checkWebAuth()) {
    server.sendHeader("Location", "/");
    server.send(303);
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
    
    // Set session cookie
    String cookie = "ESPSESSION=" + sessionToken + "; Path=/; Max-Age=3600";
    server.sendHeader("Set-Cookie", cookie);
    server.sendHeader("Location", "/");
    server.send(303);
    
    Serial.println("User logged in successfully");
  } else {
    // Login failed
    server.sendHeader("Location", "/login?error=1");
    server.send(303);
    
    Serial.println("Login failed for user: " + username);
  }
}

void handleLogout() {
  // Clear session
  sessionToken = "";
  sessionCreatedAt = 0;
  
  // Clear cookie
  server.sendHeader("Set-Cookie", "ESPSESSION=; Path=/; Max-Age=0");
  server.sendHeader("Location", "/login");
  server.send(303);
  
  Serial.println("User logged out");
}

void handleRoot() {
  if (!checkWebAuth()) {
    requireAuth();
    return;
  }
  // show sensor readings and relay controls
  String body = "<p><a href='/sensors'>Live Sensors</a> | <a href='/relays'>Relays</a> | <a href='/config'>API Config</a></p>";
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
  String b = "<p>DHT11 Temperature: " + String(t) + " °C<br/>DHT11 Humidity: " + String(h) + " %<br/>DS18B20: " + String(t2) + " °C</p>";
  b += "<p><a href='/'>Back</a></p>";
  server.send(200, "text/html", htmlTemplate("Live Sensors", b));
}

void handleRelaysPage() {
  if (!checkWebAuth()) {
    requireAuth();
    return;
  }
  String b = "<form method='POST' action='/relays/set'>";
  for (int i=0;i<8;i++) {
    b += "Relay " + String(i+1) + ": <input type='checkbox' name='r" + String(i+1) + "' " + (relayState[i] ? "checked" : "") + "><br/>";
  }
  b += "<br/><input type='submit' value='Update'></form>";
  b += "<p><a href='/'>Back</a></p>";
  server.send(200, "text/html", htmlTemplate("Relays", b));
}

void handleRelaysSet() {
  if (!checkWebAuth()) {
    requireAuth();
    return;
  }
  // update relayState according to checkboxes
  for (int i=0;i<8;i++) {
    String key = "r" + String(i+1);
    bool checked = server.hasArg(key);
    relayState[i] = checked ? 1 : 0;
    digitalWrite(relayPins[i], relayState[i] ? HIGH : LOW);
  }
  
  // Submit updated relay status to backend
  postRelayUpdateToBackend(0, 0); // Index not used, submits all relays
  
  server.sendHeader("Location", "/relays");
  server.send(303);
}

void handleConfigPage() {
  if (!checkWebAuth()) {
    requireAuth();
    return;
  }
  String b = "<form method='POST' action='/config/save'>";
  b += "Device ID: <input name='device_id' value='" + device_id + "'><br/>";
  b += "JWT Secret Key: <input name='jwtSecret' type='password' value='" + jwtSecretKey + "'><br/>";
  b += "Web Username: <input name='webUser' value='" + webUsername + "'><br/>";
  b += "Web Password: <input name='webPass' type='password' value='" + webPassword + "'><br/>";
  b += "Sensor Interval (ms): <input name='sensInt' value='" + String(sensorInterval) + "'><br/>";
  b += "<input type='submit' value='Save'>";
  b += "</form><p><a href='/'>Back</a></p>";
  server.send(200, "text/html", htmlTemplate("Config", b));
}

void handleConfigSave() {
  if (!checkWebAuth()) {
    requireAuth();
    return;
  }
  if (server.hasArg("device_id")) device_id = server.arg("device_id");
  if (server.hasArg("jwtSecret")) jwtSecretKey = server.arg("jwtSecret");
  if (server.hasArg("webUser")) webUsername = server.arg("webUser");
  if (server.hasArg("webPass")) webPassword = server.arg("webPass");
  if (server.hasArg("sensInt")) sensorInterval = server.arg("sensInt").toInt();
  
  // Clear cached JWT when secret key changes
  cachedJWT = "";
  jwtGeneratedAt = 0;
  
  savePrefs();
  server.sendHeader("Location", "/config");
  server.send(303);
}

void setupWebServerRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/login", HTTP_GET, handleLogin);
  server.on("/login", HTTP_POST, handleLoginPost);
  server.on("/logout", HTTP_GET, handleLogout);
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
  Serial.println("Starting ESP32 IoT system...");

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
    digitalWrite(relayPins[i], LOW);
  }

  // Start peripherals
  dht.begin();
  ds18b20.begin();
  lcd.init();
  lcd.backlight();

  loadPrefs();

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("SSID: ESP32_ConfigAP");
  lcd.setCursor(0,1);
  lcd.print(WiFi.localIP());
  // WiFiManager for setup
  wifiManager.setTimeout(600); // captive portal times out after 3 minutes

  wifiManager.setSaveConfigCallback(saveConfigCallback);
  bool res = wifiManager.autoConnect("ESP32_ConfigAP", "12345678");  // AP SSID, password

  if (!res) {
    Serial.println("❌ Failed to connect. Restarting...");
    ESP.restart();
  } else {
    Serial.println("✅ WiFi connected");
    Serial.print("SSID: "); Serial.println(WiFi.SSID());
    Serial.print("IP: ");   Serial.println(WiFi.localIP());
    digitalWrite(LED_WIFI_PIN, HIGH);

    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print(WiFi.SSID());
    lcd.setCursor(0,1);
    lcd.print(WiFi.localIP());
    delay(1500);
  }

  // if (!wifiManager.autoConnect("ESP32-Config-AP")) {
  //   Serial.println("Failed to connect and timed out. Rebooting...");
  //   ESP.restart();
  // }

  Serial.println("Connected to WiFi: " + WiFi.SSID());
  digitalWrite(LED_WIFI_PIN, HIGH);

  // Start web server
  setupWebServerRoutes();

  // Connect to MQTT broker
  connectToMQTT();

  // ensure we have credentials persisted
  savePrefs();

  lastSensorMillis = millis() - sensorInterval; // immediate read
}

// ---------- Loop ----------
void cycleLCD() {
  static unsigned long lastCycle = 0;
  static int slide = 0;
  
  // If in custom message mode, show the custom message
  if (customMessageMode) {
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
      digitalWrite(relayPins[focusIdx], relayState[focusIdx] ? HIGH : LOW);
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
  // Maintain MQTT connection
  if (!mqttClient.connected()) {
    connectToMQTT();
  }
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

    // Relay states now controlled via MQTT and HTTP API
    // Removed: fetchRelayStateFromBackend()
  }

  // Periodic relay status sync from backend
  if (millis() - lastRelaySyncMillis >= relaySyncInterval) {
    lastRelaySyncMillis = millis();
    Serial.println("Syncing relay states from backend...");
    fetchRelayStateFromBackend();
  }

  // slight delay
  delay(10);
}
