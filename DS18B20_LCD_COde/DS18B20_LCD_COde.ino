#include <WiFi.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "time.h"

// === WiFi Credentials ===
const char* ssid = "No internet";
const char* password = "nopstation121";

// === DS18B20 Setup ===
#define ONE_WIRE_BUS 5  // GPIO4 connected to DS18B20 data pin
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// === LCD Setup (16x2) ===
LiquidCrystal_I2C lcd(0x27, 16, 2);  // LCD at address 0x27, 16x2 display

// === NTP Setup ===
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 6 * 3600;  // GMT+6 (Bangladesh)
const int daylightOffset_sec = 0;

// === Get Current Time ===
String getCurrentTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "No Time";
  }
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo);
  return String(buffer);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nESP32 + DS18B20 + LCD Test");

  // === Initialize I2C & LCD ===
  Wire.begin(21, 22); // SDA = 21, SCL = 22
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(1, 0);
  lcd.print("ESP32 + DS18B20");
  lcd.setCursor(2, 1);
  lcd.print("Connecting WiFi");

  // === Connect WiFi ===
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" Connected!");
    lcd.clear();
    lcd.print("WiFi Connected");
  } else {
    Serial.println(" Failed!");
    lcd.clear();
    lcd.print("WiFi Failed");
  }

  // === Initialize DS18B20 ===
  sensors.begin();

  // === Setup Time from NTP ===
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  delay(1000);

  lcd.clear();
  lcd.print("Time Sync Done");
  delay(1500);
}

void loop() {
  // === Read Temperature ===
  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);
  float tempF = sensors.toFahrenheit(tempC);
  if (tempC == DEVICE_DISCONNECTED_C) {
    Serial.println("Error: DS18B20 not detected!");
    lcd.clear();
    lcd.print("Sensor Error!");
    delay(2000);
    return;
  }

  // === Get Current Time ===
  String currentTime = getCurrentTime();

  // === Print to Serial ===
  Serial.print("[" + currentTime + "] ");
  Serial.print("Temp: ");
  Serial.print(tempC, 1);
  Serial.print(" °C / ");
  Serial.print(tempF, 1);
  Serial.println(" °F");

  // === Display on LCD ===
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Time:");
  lcd.print(currentTime);

  lcd.setCursor(0, 1);
  lcd.print(tempC, 1);
  lcd.print((char)223);
  lcd.print("C ");
  lcd.print(tempF, 1);
  lcd.print((char)223);
  lcd.print("F");

  delay(2000);
}
