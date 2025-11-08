#include "DHT.h"

#define DHTPIN 4
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

void setup() {
  Serial.begin(115200);
  dht.begin();
  Serial.println("DHT11 Test");
}

void loop() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (isnan(t) || isnan(h)) {
    Serial.println("DHT11 not responding!");
  } else {
    Serial.print("Temp: "); Serial.print(t); Serial.print(" C, ");
    Serial.print("Humidity: "); Serial.print(h); Serial.println(" %");
  }
  delay(2000);
}
