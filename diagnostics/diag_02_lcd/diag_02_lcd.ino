/*
 * diag_02_lcd  --  Stage A bring-up #2 (Rozna Arduino Thermometer)
 *
 * Goal: verify the 16x2 HD44780 LCD wiring + contrast, in 4-bit mode, using
 * the exact pin map from the original sketch.
 *
 * Wiring (matches original  LiquidCrystal lcd(7,6,5,4,3,2) ):
 *   LCD RS -> D7      LCD E  -> D6
 *   LCD D4 -> D5      LCD D5 -> D4
 *   LCD D6 -> D3      LCD D7 -> D2
 *   LCD RW -> GND
 *   LCD VSS-> GND     LCD VDD -> 5V
 *   LCD V0 -> wiper of the contrast potentiometer (pot ends to 5V and GND)
 *   LCD A  -> 5V (through ~220R)   LCD K -> GND     (backlight)
 *
 * Serial: 9600 baud.
 *
 * ---- PASS looks like ----
 *   - Row 0: "Rozna Thermo OK"
 *   - Row 1: "t=NN  23.4{deg}C" with NN counting up, and a block in the last
 *     column that toggles every second (proves the panel is refreshing).
 *   - Serial prints "LCD refresh #N".
 *
 * ---- If it FAILS ----
 *   - Backlight on but blank / faint: turn the contrast pot until text is
 *     crisp. This is the #1 cause and is NOT a wiring fault.
 *   - One row of solid bright blocks, no text: contrast too high OR the panel
 *     never got initialised -- check E (D6) and RS (D7), and RW must be GND.
 *   - Garbage characters: a data line (D2..D5) is swapped or loose.
 */

#include <LiquidCrystal.h>

// RS, E, D4, D5, D6, D7
LiquidCrystal lcd(7, 6, 5, 4, 3, 2);

void setup() {
  Serial.begin(9600);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) { }
  Serial.println(F("=== diag_02_lcd ==="));
  Serial.println(F("If LCD is blank or shows blocks: adjust the contrast pot."));

  lcd.begin(16, 2);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Rozna Thermo OK"));
}

byte n = 0;

void loop() {
  lcd.setCursor(0, 1);
  lcd.print(F("t="));
  if (n < 10) lcd.print('0');
  lcd.print(n);
  lcd.print(F("  23.4"));
  lcd.write((uint8_t)223);   // degree symbol, same glyph the app uses
  lcd.print(F("C  "));

  // Walking block in the last cell so a refresh is obvious.
  lcd.setCursor(15, 1);
  lcd.write((n % 2) ? (uint8_t)0xFF : (uint8_t)' ');

  Serial.print(F("LCD refresh #"));
  Serial.println(n);
  n++;
  delay(1000);
}
