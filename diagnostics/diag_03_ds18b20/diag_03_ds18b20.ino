/*
 * diag_03_ds18b20  --  Stage A bring-up #3 (Rozna Arduino Thermometer)
 *
 * Goal: enumerate the DS18B20 sensor(s) on the OneWire bus, print each ROM
 * address with a CRC check, and read temperature continuously.
 *
 * Wiring:
 *   DS18B20 DATA -> D10
 *   DS18B20 VDD  -> 5V        DS18B20 GND -> GND
 *   4.7k pull-up resistor between DATA and 5V   <-- REQUIRED, easy to forget
 *
 * Serial: 9600 baud.
 *
 * ---- PASS looks like ----
 *   - "Found N device(s)" with N >= 1
 *   - A ROM line like:  [0] ROM 28 FF 64 1E ...  CRC OK  (DS18B20)
 *     (DS18B20 family code is 0x28)
 *   - Plausible readings, e.g.  "Sensor 0: 24.50 C / 76.10 F"
 *
 * ---- If it FAILS ----
 *   - "Found 0 device(s)": missing 4.7k pull-up, or DATA not on D10, or no power.
 *   - CRC FAIL: noisy/long bus or marginal pull-up.
 *   - Reading -127.00 C: sensor seen on the bus but the read failed (loose wire).
 *   - Reading 85.00 C exactly on the very first read: power-on default; the
 *     next read should be correct.
 */

#include <OneWire.h>
#include <DallasTemperature.h>

#define ONE_WIRE_BUS 10
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

void printAddress(DeviceAddress a) {
  for (uint8_t i = 0; i < 8; i++) {
    if (a[i] < 16) Serial.print('0');
    Serial.print(a[i], HEX);
    if (i < 7) Serial.print(' ');
  }
}

void setup() {
  Serial.begin(9600);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) { }
  Serial.println(F("=== diag_03_ds18b20 ==="));

  sensors.begin();
  sensors.setResolution(12);

  Serial.print(F("Found "));
  Serial.print(sensors.getDeviceCount());
  Serial.println(F(" device(s)"));

  // Walk the raw bus to show ROM codes + verify CRC of each address.
  DeviceAddress addr;
  oneWire.reset_search();
  int idx = 0;
  while (oneWire.search(addr)) {
    Serial.print(F("  ["));
    Serial.print(idx);
    Serial.print(F("] ROM "));
    printAddress(addr);
    Serial.print(OneWire::crc8(addr, 7) == addr[7] ? F("  CRC OK") : F("  CRC FAIL"));
    Serial.println(addr[0] == 0x28 ? F("  (DS18B20)") : F("  (other family)"));
    idx++;
  }
  if (idx == 0)
    Serial.println(F("No ROMs found. Check the 4.7k pull-up on D10."));
}

void loop() {
  sensors.requestTemperatures();
  uint8_t n = sensors.getDeviceCount();
  for (uint8_t i = 0; i < n; i++) {
    float c = sensors.getTempCByIndex(i);
    Serial.print(F("Sensor "));
    Serial.print(i);
    Serial.print(F(": "));
    if (c == DEVICE_DISCONNECTED_C) {
      Serial.println(F("DISCONNECTED (-127)"));
    } else {
      Serial.print(c, 2);
      Serial.print(F(" C / "));
      Serial.print(DallasTemperature::toFahrenheit(c), 2);
      Serial.println(F(" F"));
    }
  }
  if (n == 0)
    Serial.println(F("No sensors. Check 4.7k pull-up + DATA on D10."));
  Serial.println();
  delay(2000);
}
