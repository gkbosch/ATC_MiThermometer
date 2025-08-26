// Adafruit MAX31855 test on ESP32-S3 with software SPI
#include <Arduino.h>
#include <Adafruit_MAX31855.h>

// Your wiring on ESP32-S3
const int PIN_CS   = 5;   // CS
const int PIN_MISO = 4;   // DO from MAX31855
const int PIN_SCK  = 18;  // SCK

Adafruit_MAX31855 thermo(PIN_SCK, PIN_CS, PIN_MISO);

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Adafruit MAX31855 (soft SPI) on ESP32-S3");
  Serial.print("Pins CS="); Serial.print(PIN_CS);
  Serial.print(" DO="); Serial.print(PIN_MISO);
  Serial.print(" SCK="); Serial.println(PIN_SCK);

  if (!thermo.begin()) {
    Serial.println("MAX31855 not found. Check wiring.");
  }
}

void loop() {
  double intC = thermo.readInternal();
  double extC = thermo.readCelsius();
  uint8_t f = thermo.readError(); // bitmask: OC=1, SCG=2, SCV=4

  Serial.print("INT: "); Serial.print(intC, 2); Serial.print(" C  ");
  Serial.print("EXT: "); Serial.print(extC, 2); Serial.print(" C  ");
  Serial.print("FAULT: ");
  if (f == 0) Serial.print("none");
  else {
    if (f & 0x01) Serial.print("OC ");
    if (f & 0x02) Serial.print("SCG ");
    if (f & 0x04) Serial.print("SCV ");
  }
  Serial.println();
  delay(1000);
}
