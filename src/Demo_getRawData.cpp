//
//    FILE: Demo_getRawData.ino
//  AUTHOR: FabioBrondo
// PURPOSE: thermocouple lib demo application
//    DATE: 2020-08-24
//     URL: https://github.com/RobTillaart/MAX31855_RT


#include "MAX31855.h"


const int selectPin = 5;   // CS
const int dataPin   = 4;   // DO (MISO)
const int clockPin  = 18;  // SCK

// Try two constructors to handle possible pin-order differences in the library
// Variant A: (SCLK, CS, MISO)
MAX31855 thermoCouple(clockPin, selectPin, dataPin);
// Variant B: (CS, SCLK, MISO)
MAX31855 thermoCouple_alt(selectPin, clockPin, dataPin);


void setup ()
{
  Serial.begin(115200);
  Serial.println("hello");
  Serial.println(__FILE__);
  Serial.print("MAX31855_VERSION : ");
  Serial.println(MAX31855_VERSION);
  Serial.println();
  delay(250);

  // Print chosen pins for quick wiring verification
  Serial.print("CS="); Serial.print(selectPin);
  Serial.print("  DO="); Serial.print(dataPin);
  Serial.print("  SCK="); Serial.println(clockPin);

  // Try to get an initial sense of DO line state with different pulls to spot a floating line
  pinMode(dataPin, INPUT);
  delay(2);
  Serial.print("DOUT idle (no pull): ");
  Serial.println(digitalRead(dataPin));
  pinMode(dataPin, INPUT_PULLUP);
  delay(2);
  Serial.print("DOUT idle (PULLUP): ");
  Serial.println(digitalRead(dataPin));
  pinMode(dataPin, INPUT_PULLDOWN);
  delay(2);
  Serial.print("DOUT idle (PULLDOWN): ");
  Serial.println(digitalRead(dataPin));

  // On ESP32, specify SPI pins explicitly to avoid default pin routing issues
  SPI.begin(clockPin, dataPin, -1, selectPin);

  thermoCouple.begin();
  thermoCouple_alt.begin();
}


void loop ()
{
  // Quiet the library; we'll rely on manual reads below.
  (void)thermoCouple;

  // Average a few manual frames for stability
  const int N = 4;
  double sumExt = 0, sumInt = 0; int faultCount = 0;
  for (int s = 0; s < N; ++s) {
    // Manual bit-banged read
    uint32_t manual = 0;
    pinMode(selectPin, OUTPUT);
    pinMode(clockPin, OUTPUT);
    pinMode(dataPin, INPUT);
    digitalWrite(selectPin, HIGH);
    digitalWrite(clockPin, HIGH);
    delayMicroseconds(5);
    digitalWrite(selectPin, LOW);
    delayMicroseconds(5);
    for (int i = 31; i >= 0; --i) {
      digitalWrite(clockPin, LOW);
      delayMicroseconds(3);
      int bit = digitalRead(dataPin);
      manual |= (bit ? 1UL : 0UL) << i;
      digitalWrite(clockPin, HIGH);
      delayMicroseconds(3);
    }
    digitalWrite(selectPin, HIGH);

    // Decode manual frame
    bool faultM = manual & 0x00010000; faultCount += faultM ? 1 : 0;
    int32_t t14m = (int32_t)((manual >> 18) & 0x00003FFF); if (t14m & 0x2000) t14m |= 0xFFFFC000; double extCm = (double)t14m * 0.25;
    int32_t i12m = (int32_t)((manual >> 4)  & 0x00000FFF); if (i12m & 0x0800) i12m |= 0xFFFFF000; double intCm = (double)i12m * 0.0625;
    sumExt += extCm; sumInt += intCm;
    delay(5);
  }
  double extAvg = sumExt / N;
  double intAvg = sumInt / N;

  Serial.print("EXT(manual): "); Serial.print(extAvg, 2); Serial.print(" C  ");
  Serial.print("INT(manual): "); Serial.print(intAvg, 2); Serial.print(" C  ");
  Serial.print("FAULT(manual): "); Serial.println(faultCount ? "yes" : "none");

  // Manual bit-banged read for cross-check (datasheet: data valid after falling edge, MSB first)
  uint32_t manual = 0;
  pinMode(selectPin, OUTPUT);
  pinMode(clockPin, OUTPUT);
  pinMode(dataPin, INPUT);
  digitalWrite(selectPin, HIGH);
  digitalWrite(clockPin, HIGH);  // idle high so first transition is a falling edge
  delayMicroseconds(5);
  digitalWrite(selectPin, LOW);
  delayMicroseconds(5);
  for (int i = 31; i >= 0; --i) {
    // Generate falling edge, then sample a short time later
    digitalWrite(clockPin, LOW);
    delayMicroseconds(3);
    int bit = digitalRead(dataPin);
    manual |= (bit ? 1UL : 0UL) << i;
    digitalWrite(clockPin, HIGH);
    delayMicroseconds(3);
  }
  digitalWrite(selectPin, HIGH);
  Serial.print("RAW(manual) HEX:\t0x");
  Serial.println(manual, HEX);

  // Decode manual frame as well
  bool faultM = manual & 0x00010000;
  int32_t t14m = (int32_t)((manual >> 18) & 0x00003FFF); if (t14m & 0x2000) t14m |= 0xFFFFC000; double extCm = (double)t14m * 0.25;
  int32_t i12m = (int32_t)((manual >> 4)  & 0x00000FFF); if (i12m & 0x0800) i12m |= 0xFFFFF000; double intCm = (double)i12m * 0.0625;
  Serial.print("TMP(manual):\t");
  Serial.print(extCm, 3);
  Serial.print(" C\tINT(manual):\t");
  Serial.print(intCm, 3);
  Serial.print(" C\tFAULT(manual): ");
  if (faultM) {
    bool scvM = manual & 0x00000004; bool scgM = manual & 0x00000002; bool ocM = manual & 0x00000001;
    if (scvM) Serial.print("SCV "); if (scgM) Serial.print("SCG "); if (ocM) Serial.print("OC ");
  } else {
    Serial.print("none");
  }
  Serial.println();

  delay(1000);
}


//  -- END OF FILE --

