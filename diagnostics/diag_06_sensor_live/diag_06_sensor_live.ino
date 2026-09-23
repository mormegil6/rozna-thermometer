/*
 * diag_06_sensor_live -- Stage A diagnostic: live DS18B20 detection on the LCD
 *
 * Combines diag_02_lcd and diag_03_ds18b20 into one sketch that shows sensor
 * detection directly on the LCD, rescanning continuously. Meant for probing
 * or reseating the DS18B20 while watching the panel, no Serial monitor
 * needed to see the result.
 *
 * Wiring: LCD as in diag_02_lcd (RS,E,D4,D5,D6,D7 = 7,6,5,4,3,2, RW -> GND).
 *         DS18B20 DATA -> D10, 4.7k pull-up DATA->5V, VDD->5V, GND->GND.
 *
 * Row 0: "Sensors: N"  (N = live device count, rescanned every cycle)
 * Row 1: "Waiting..." while N==0, otherwise the temperature from sensor 0,
 *        or "seen, read FAIL" if it's on the bus but a read comes back
 *        disconnected (-127), e.g. a marginal connection.
 *
 * No lcd.clear() in the loop (matches the main firmware's approach), each
 * field is overwritten in place with trailing spaces so it doesn't flicker.
 */

#include <LiquidCrystal.h>
#include <OneWire.h>
#include <DallasTemperature.h>

LiquidCrystal lcd(7, 6, 5, 4, 3, 2);

#define ONE_WIRE_BUS 10
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

void setup() {
  Serial.begin(9600);
  lcd.begin(16, 2);
  lcd.print(F("diag_06 boot"));
  delay(500);
}

void loop() {
  sensors.begin();  // safe to call every cycle -- re-enumerates the bus fresh
  uint8_t n = sensors.getDeviceCount();

  lcd.setCursor(0, 0);
  lcd.print(F("Sensors: "));
  lcd.print(n);
  lcd.print(F("   "));

  lcd.setCursor(0, 1);
  if (n == 0) {
    lcd.print(F("Waiting...      "));
  } else {
    sensors.requestTemperatures();
    float c = sensors.getTempCByIndex(0);
    if (c == DEVICE_DISCONNECTED_C) {
      lcd.print(F("seen, read FAIL "));
    } else {
      lcd.print(c, 1);
      lcd.write((uint8_t)223);
      lcd.print(F("C          "));
    }
  }

  Serial.print(F("Sensors: "));
  Serial.println(n);

  delay(500);
}
