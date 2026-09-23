/*
 * diag_05_lcd_continuity -- Stage A diagnostic: raw pin-level continuity
 * check for the 16x2 LCD control/data lines, with NO dependency on the LCD
 * actually working. For when diag_02_lcd still shows only blocks/blank
 * after reflowing the solder joints, to find out whether each wire is
 * really reaching the LCD, one line at a time.
 *
 * Does NOT use the LiquidCrystal library, just raw digitalWrite() so a
 * multimeter can confirm each signal arrives at the LCD header pin, not
 * just the Arduino pin (a cold joint can look fine at the Arduino end and
 * still be open at the LCD end).
 *
 * Method: this sketch holds ONE line HIGH for 3s, then LOW for 1s, then
 * moves to the next line, announcing each step over Serial (9600 baud).
 * Set a multimeter to DC volts, black probe on Arduino GND, red probe on
 * the LCD-SIDE header pin named in the current message (not the Arduino
 * pin). It should read ~5V then ~0V in step with the messages. A pin that
 * stays at 0V or floats at the LCD end while the message says HIGH means
 * that wire or joint is still broken, even if it reads fine at the
 * Arduino pin itself.
 *
 * RW, VSS, VDD and V0 are not driven by any Arduino pin (RW -> GND,
 * VSS -> GND, VDD -> 5V, V0 -> contrast pot wiper), so check those with
 * the multimeter in continuity/resistance mode, power OFF, separately.
 *
 * Pin 13 (built-in LED) blinks throughout as a "sketch is really running"
 * heartbeat. If it is not blinking, the upload did not take, or the board
 * is not running this sketch, check that before anything else.
 */

#include <Arduino.h>

struct LcdLine {
  uint8_t arduinoPin;
  const char *lcdName;
  uint8_t lcdPin;
};

const LcdLine LINES[] = {
  {7, "RS", 4},
  {6, "E",  6},
  {5, "D4", 11},
  {4, "D5", 12},
  {3, "D6", 13},
  {2, "D7", 14},
};
const uint8_t N = sizeof(LINES) / sizeof(LINES[0]);

static void heartbeatDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    digitalWrite(LED_BUILTIN, ((millis() / 250) % 2) ? HIGH : LOW);
  }
}

static void driveAndAnnounce(const LcdLine &l, bool high) {
  digitalWrite(l.arduinoPin, high ? HIGH : LOW);
  Serial.print(F("Driving "));
  Serial.print(l.lcdName);
  Serial.print(F(" (Arduino D"));
  Serial.print(l.arduinoPin);
  Serial.print(F(" -> LCD pin "));
  Serial.print(l.lcdPin);
  Serial.print(F(") = "));
  Serial.println(high ? F("HIGH (~5V)") : F("LOW (~0V)"));
}

void setup() {
  Serial.begin(9600);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}
  Serial.println(F("=== diag_05_lcd_continuity ==="));
  Serial.println(F("Probe the LCD-SIDE pin named below with a multimeter (DC volts,"));
  Serial.println(F("black lead on Arduino GND). It should track each HIGH/LOW below."));
  Serial.println();

  pinMode(LED_BUILTIN, OUTPUT);
  for (uint8_t i = 0; i < N; i++) {
    pinMode(LINES[i].arduinoPin, OUTPUT);
    digitalWrite(LINES[i].arduinoPin, LOW);
  }
}

void loop() {
  for (uint8_t i = 0; i < N; i++) {
    driveAndAnnounce(LINES[i], true);
    heartbeatDelay(3000);
    driveAndAnnounce(LINES[i], false);
    heartbeatDelay(1000);
  }
  Serial.println(F("--- cycle complete, repeating ---"));
  Serial.println();
}
