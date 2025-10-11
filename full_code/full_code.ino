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
    - ArduinoJson (optional but not required for very simple parsing)
  
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

#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LiquidCrystal_I2C.h>

// ---------- CONFIG ----------
#define BACKEND_BASE_URL "http://your-backend.example.com/" // <-- change to your backend
#define AUTH_ENDPOINT "baseapi/Auth"
#define SENSOR_POST_ENDPOINT "baseapi/sensorReading"
#define GET_RELAY_ENDPOINT "baseapi/getRelayStatus"
#define SET_RELAY_ENDPOINT "baseapi/setRelayStatus"

#define DEVICE_ID_DEFAULT "esp32-default-device"

// Pins
const int I2C_SDA = 21;
const int I2C_SCL = 22;
const int DHT11_PIN = 13;
const int DS18B20_PIN = 5;
const int LED_WIFI_PIN = 2;

const int BTN_WIFI_RESET = 35; // input only (use with pullup)
const int BTN_NAV = 0;
const int BTN_CFG = 5;
const int BTN_TOGGLE = 18;

// Relay pins (change to match your wiring)
const int relayPins[8] = {33, 12, 14, 27, 26, 25, 33, 32};

// Sensor intervals (ms)
unsigned long sensorInterval = 30UL * 1000UL; // default 30s; configurable
unsigned long lastSensorMillis = 0;

// DHT
#define DHTTYPE DHT11
DHT dht(DHT11_PIN, DHTTYPE);

// DS18B20
OneWire oneWire(DS18B20_PIN);
DallasTemperature ds18b20(&oneWire);

// LCD (address 0x27 typical)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Web server
WebServer server(80);
WiFiManager wifiManager;
Preferences prefs;

// Device state
String device_id = DEVICE_ID_DEFAULT;
String apiUsername = "admin";
String apiPassword = "admin";
String webUsername = "admin";
String webPassword = "admin";

String jwtToken = "";
unsigned long jwtObtainedAt = 0;
unsigned long jwtExpiresIn = 60 * 60 * 1000UL; // assume 1 hour by default

// Relay states (0=off,1=on)
int relayState[8] = {0,0,0,0,0,0,0,0};

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

// ---------- Helpers ----------
String headerBasicAuthEncoded(const String &user, const String &pass) {
  // not using Basic, but helper if needed
  return "";
}

void loadPrefs() {
  prefs.begin("iotcfg", false);
  device_id = prefs.getString("device_id", DEVICE_ID_DEFAULT);
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
  prefs.putString("apiUser", apiUsername);
  prefs.putString("apiPass", apiPassword);
  prefs.putString("webUser", webUsername);
  prefs.putString("webPass", webPassword);
  prefs.putULong("sensInterval", sensorInterval);
  prefs.end();
}

void resetWifiSettings() {
  Serial.println("Resetting Wi-Fi settings and clearing saved Wi-Fi...");
  wifiManager.resetSettings();
  // Optionally also clear stored API/web credentials?
  // prefs.begin("iotcfg", false); prefs.clear(); prefs.end();
  ESP.restart();
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

bool ensureJWT() {
  if (jwtToken.length() == 0 || (millis() - jwtObtainedAt) > (jwtExpiresIn - 30UL*1000UL)) {
    // Request new token
    Serial.println("Requesting JWT token from backend...");
    HTTPClient http;
    String url = String(BACKEND_BASE_URL) + AUTH_ENDPOINT;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    String payload = "{\"username\":\"" + apiUsername + "\",\"password\":\"" + apiPassword + "\"}";
    int code = http.POST(payload);
    if (code > 0) {
      String resp = http.getString();
      Serial.printf("Auth response (%d): %s\n", code, resp.c_str());
      if (code == 200 || code == 201) {
        // Try to find token field in response
        int tpos = resp.indexOf("token");
        if (tpos >= 0) {
          int colon = resp.indexOf(':', tpos);
          if (colon > 0) {
            int firstQuote = resp.indexOf('"', colon);
            int secondQuote = resp.indexOf('"', firstQuote+1);
            if (firstQuote > 0 && secondQuote > firstQuote) {
              jwtToken = resp.substring(firstQuote+1, secondQuote);
              jwtObtainedAt = millis();
              // try to find expiresIn numeric (ms)
              int epos = resp.indexOf("expiresIn");
              if (epos > 0) {
                int ec = resp.indexOf(':', epos);
                if (ec > 0) {
                  int comma = resp.indexOf(',', ec);
                  String num = resp.substring(ec+1, (comma>0?comma:resp.length()));
                  num.trim();
                  unsigned long val = num.toInt();
                  if (val > 0) jwtExpiresIn = val * 1000UL; // if secs
                }
              }
              http.end();
              return true;
            }
          }
        }
        // fallback: maybe entire response is token plain
        jwtToken = resp;
        jwtObtainedAt = millis();
        http.end();
        return true;
      } else {
        Serial.println("Auth failed");
      }
    } else {
      Serial.printf("HTTP error on Auth: %d\n", code);
    }
    http.end();
    return false;
  }
  return true;
}

bool postSensorReadings(const std::vector<SensorReading> &readings) {
  if (readings.empty()) return true;
  if (!ensureJWT()) return false;

  HTTPClient http;
  String url = String(BACKEND_BASE_URL) + SENSOR_POST_ENDPOINT;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + jwtToken);
  http.addHeader("device_id", device_id);

  // Build JSON array
  String body = "[";
  for (size_t i=0;i<readings.size();i++) {
    body += "{\"name\":\"" + readings[i].name + "\",\"value\":" + String(readings[i].value, (readings[i].name.indexOf("Humidity")>=0?0:2)) + ",\"unit\":\"" + readings[i].unit + "\"}";
    if (i+1 < readings.size()) body += ",";
  }
  body += "]";
  int code = http.POST(body);
  if (code > 0) {
    String resp = http.getString();
    Serial.printf("Sensor POST (%d): %s\n", code, resp.c_str());
    http.end();
    if (code == 200 || code == 201) return true;
    return false;
  } else {
    Serial.printf("Sensor POST failed, code: %d\n", code);
    http.end();
    return false;
  }
}

bool fetchRelayStateFromBackend() {
  if (!ensureJWT()) return false;
  HTTPClient http;
  String url = String(BACKEND_BASE_URL) + GET_RELAY_ENDPOINT;
  http.begin(url);
  http.addHeader("Authorization", "Bearer " + jwtToken);
  http.addHeader("device_id", device_id);
  int code = http.GET();
  if (code > 0) {
    String resp = http.getString();
    Serial.printf("GetRelay (%d): %s\n", code, resp.c_str());
    if (code == 200) {
      setRelaysFromBackendJSON(resp);
      http.end();
      return true;
    }
  } else {
    Serial.printf("GetRelay failed code: %d\n", code);
  }
  http.end();
  return false;
}

bool postRelayUpdateToBackend(int index, int val) {
  if (!ensureJWT()) return false;
  HTTPClient http;
  String url = String(BACKEND_BASE_URL) + SET_RELAY_ENDPOINT;
  http.begin(url);
  http.addHeader("Authorization", "Bearer " + jwtToken);
  http.addHeader("device_id", device_id);
  http.addHeader("Content-Type", "application/json");

  String body = "{\"Relay\": {\"" + String(index+1) + "\":" + String(val) + "}}";
  int code = http.POST(body);
  if (code > 0) {
    String resp = http.getString();
    Serial.printf("SetRelay (%d): %s\n", code, resp.c_str());
    http.end();
    return (code == 200 || code == 201);
  } else {
    Serial.printf("SetRelay failed code: %d\n", code);
  }
  http.end();
  return false;
}

// ---------- Web UI ----------
bool checkWebAuth() {
  // very simple credential check
  if (!server.authenticate(webUsername.c_str(), webPassword.c_str())) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

String htmlTemplate(const String &title, const String &body) {
  String s = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>" + title + "</title></head><body><h3>" + title + "</h3>";
  s += body;
  s += "<hr><small>Device: " + device_id + "</small></body></html>";
  return s;
}

void handleRoot() {
  if (!checkWebAuth()) return;
  // show sensor readings and relay controls
  String body = "<p><a href='/sensors'>Live Sensors</a> | <a href='/relays'>Relays</a> | <a href='/config'>API Config</a></p>";
  server.send(200, "text/html", htmlTemplate("ESP32 - Home", body));
}

void handleSensorsPage() {
  if (!checkWebAuth()) return;
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  ds18b20.requestTemperatures();
  float t2 = ds18b20.getTempCByIndex(0);
  String b = "<p>DHT11 Temperature: " + String(t) + " °C<br/>DHT11 Humidity: " + String(h) + " %<br/>DS18B20: " + String(t2) + " °C</p>";
  b += "<p><a href='/'>Back</a></p>";
  server.send(200, "text/html", htmlTemplate("Live Sensors", b));
}

void handleRelaysPage() {
  if (!checkWebAuth()) return;
  String b = "<form method='POST' action='/relays/set'>";
  for (int i=0;i<8;i++) {
    b += "Relay " + String(i+1) + ": <input type='checkbox' name='r" + String(i+1) + "' " + (relayState[i] ? "checked" : "") + "><br/>";
  }
  b += "<br/><input type='submit' value='Update'></form>";
  b += "<p><a href='/'>Back</a></p>";
  server.send(200, "text/html", htmlTemplate("Relays", b));
}

void handleRelaysSet() {
  if (!checkWebAuth()) return;
  // update relayState according to checkboxes
  for (int i=0;i<8;i++) {
    String key = "r" + String(i+1);
    bool checked = server.hasArg(key);
    relayState[i] = checked ? 1 : 0;
    digitalWrite(relayPins[i], relayState[i] ? HIGH : LOW);
    // send to backend (fire and forget ideally)
    if (!postRelayUpdateToBackend(i, relayState[i])) {
      Serial.println("Warning: failed to post relay update for index " + String(i));
    }
  }
  server.sendHeader("Location", "/relays");
  server.send(303);
}

void handleConfigPage() {
  if (!checkWebAuth()) return;
  String b = "<form method='POST' action='/config/save'>";
  b += "API Username: <input name='apiUser' value='" + apiUsername + "'><br/>";
  b += "API Password: <input name='apiPass' type='password' value='" + apiPassword + "'><br/>";
  b += "Web Username: <input name='webUser' value='" + webUsername + "'><br/>";
  b += "Web Password: <input name='webPass' type='password' value='" + webPassword + "'><br/>";
  b += "Device ID: <input name='device_id' value='" + device_id + "'><br/>";
  b += "Sensor Interval (ms): <input name='sensInt' value='" + String(sensorInterval) + "'><br/>";
  b += "<input type='submit' value='Save'>";
  b += "</form><p><a href='/'>Back</a></p>";
  server.send(200, "text/html", htmlTemplate("Config", b));
}

void handleConfigSave() {
  if (!checkWebAuth()) return;
  if (server.hasArg("apiUser")) apiUsername = server.arg("apiUser");
  if (server.hasArg("apiPass")) apiPassword = server.arg("apiPass");
  if (server.hasArg("webUser")) webUsername = server.arg("webUser");
  if (server.hasArg("webPass")) webPassword = server.arg("webPass");
  if (server.hasArg("device_id")) device_id = server.arg("device_id");
  if (server.hasArg("sensInt")) sensorInterval = server.arg("sensInt").toInt();
  savePrefs();
  server.sendHeader("Location", "/config");
  server.send(303);
}

void setupWebServerRoutes() {
  server.on("/", HTTP_GET, handleRoot);
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

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Starting ESP32 IoT system...");

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

  // WiFiManager for setup
  wifiManager.setTimeout(180); // captive portal times out after 3 minutes
  if (!wifiManager.autoConnect("ESP32-Config-AP")) {
    Serial.println("Failed to connect and timed out. Rebooting...");
    ESP.restart();
  }

  Serial.println("Connected to WiFi: " + WiFi.SSID());
  digitalWrite(LED_WIFI_PIN, HIGH);

  // Start web server
  setupWebServerRoutes();

  // ensure we have credentials persisted
  savePrefs();

  lastSensorMillis = millis() - sensorInterval; // immediate read
}

// ---------- Loop ----------
void cycleLCD() {
  static unsigned long lastCycle = 0;
  static int slide = 0;
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
  } else if (!pressed && btnWifiPressed) {
    btnWifiPressed = false;
    unsigned long held = millis() - btnWifiPressStart;
    if (held >= WIFI_RESET_HOLD_MS) {
      // do reset
      resetWifiSettings();
    }
  } else if (pressed && btnWifiPressed) {
    // still holding — nothing to do here; actual resetting is on release if hold met
  }

  // Navigation button: BTN_NAV
  if (digitalRead(BTN_NAV) == LOW) {
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
  static bool lastToggleState = HIGH;
  bool tState = digitalRead(BTN_TOGGLE);
  if (tState == LOW && lastToggleState == HIGH) {
    // pressed
    if (cfgMode) {
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
      // send to backend (best effort)
      if (!postRelayUpdateToBackend(focusIdx, relayState[focusIdx])) {
        Serial.println("Failed to post relay update (toggle).");
      }
    } else {
      Serial.println("Toggle pressed but not in cfgMode.");
    }
  }
  lastToggleState = tState;
}

void loop() {
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
    if (!isnan(t)) readings.push_back( {"DHT11_Temperature", t, "°C"} );
    if (!isnan(h)) readings.push_back( {"DHT11_Humidity", h, "%"} );
    if (t2 != DEVICE_DISCONNECTED_C) readings.push_back( {"DS18B20_Temperature", t2, "°C"} );

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

    // also fetch relay states from backend and apply
    if (!fetchRelayStateFromBackend()) {
      Serial.println("Failed to sync relay state from backend (will retry later).");
    }
  }

  // slight delay
  delay(10);
}
