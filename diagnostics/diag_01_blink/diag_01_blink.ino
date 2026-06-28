/*
 * diag_01_blink  --  Stage A bring-up #1 (Rozna Arduino Thermometer)
 *
 * Goal: confirm the Leonardo is alive, can be flashed, and that the USB
 * Serial link works. Nothing else is touched.
 *
 * Wiring: none. Uses the on-board "L" LED (pin 13).
 * Serial: 9600 baud.
 *
 * ---- PASS looks like ----
 *   - The on-board LED marked "L" (next to pin 13) blinks at ~1 Hz.
 *   - Serial Monitor @ 9600 prints the banner, then "tick N" once per
 *     second with N counting up forever.
 *
 * ---- If it FAILS ----
 *   - Upload error / no port: hold nothing, just double-tap RESET right as
 *     the IDE says "Uploading" (Leonardo bootloader window is short), and
 *     make sure the board is selected as "Arduino Leonardo".
 *   - LED blinks but no Serial text: the Monitor must be opened AFTER upload
 *     and set to 9600 baud; on Leonardo the port number can change on reset.
 */

#define LED_PIN 13

void setup() {
  pinMode(LED_PIN, OUTPUT);
  Serial.begin(9600);
  // Leonardo USB-Serial takes a moment to enumerate. Wait a little, but do
  // NOT block forever, so the LED still blinks with no Serial Monitor open.
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) { }
  Serial.println(F("=== diag_01_blink ==="));
  Serial.println(F("Board alive. LED on pin 13 should blink ~1 Hz."));
}

unsigned long tick = 0;

void loop() {
  digitalWrite(LED_PIN, HIGH);
  delay(500);
  digitalWrite(LED_PIN, LOW);
  delay(500);
  Serial.print(F("tick "));
  Serial.println(tick++);
}
