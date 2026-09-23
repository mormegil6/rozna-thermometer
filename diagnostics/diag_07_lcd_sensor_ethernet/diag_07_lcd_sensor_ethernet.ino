/*
 * diag_07_lcd_sensor_ethernet -- combined Stage A diagnostic
 *
 * Merges diag_06_sensor_live (LCD + live DS18B20 detection) with
 * diag_04_ethernet (ENC28J60 bring-up, static IP, link state) into one
 * sketch, so sensor wiring and network reachability can both be checked
 * without re-flashing between them.
 *
 * Instead of relying on ICMP ping (which EtherCard answers automatically,
 * but which is not guaranteed to pass through every router/switch/firewall
 * configuration), this serves a tiny real HTTP page. Open
 * http://192.168.1.200/ in a browser, or `curl 192.168.1.200`, that is a
 * more meaningful test since it is the actual feature the thermometer needs.
 *
 * Wiring: LCD as in diag_02_lcd/diag_06. DS18B20 as in diag_03/diag_06.
 * ENC28J60 as in diag_04 (SPI via ICSP, CS -> D8, module on 3.3V).
 *
 * LCD:
 *   Row 0: "Sensors: N"        (live DS18B20 count, rescanned every cycle)
 *   Row 1: "Waiting..." / temperature / "seen, read FAIL"  (as diag_06)
 *
 * Serial (9600 baud): reports ENC28J60 detection, IP/GW/mask once at boot,
 * then link state and sensor count every 2s.
 */

#include <LiquidCrystal.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <EtherCard.h>

LiquidCrystal lcd(7, 6, 5, 4, 3, 2);

#define ONE_WIRE_BUS 10
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

#define CS_PIN 8
static byte mymac[] = { 0x02, 0x52, 0x6F, 0x7A, 0x6E, 0x61 };  // "02 R o z n a"
static byte myip[]  = { 192, 168, 1, 200 };
static byte gwip[]  = { 192, 168, 1, 1 };
static byte mask[]  = { 255, 255, 255, 0 };

byte Ethernet::buffer[900];

const char page[] PROGMEM =
"HTTP/1.0 200 OK\r\n"
"Content-Type: text/plain\r\n"
"\r\n"
"Rozna Thermo -- diag_07 -- Ethernet OK, this page was served over HTTP.\r\n";

void setup() {
  Serial.begin(9600);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}
  Serial.println(F("=== diag_07_lcd_sensor_ethernet ==="));

  lcd.begin(16, 2);
  lcd.print(F("diag_07 boot"));

  sensors.begin();

  uint8_t rev = ether.begin(sizeof Ethernet::buffer, mymac, CS_PIN);
  if (rev == 0) {
    Serial.println(F("FAIL: ether.begin() returned 0 (ENC28J60 not found over SPI)."));
  } else {
    Serial.print(F("ENC28J60 found, rev "));
    Serial.println(rev);
    ether.staticSetup(myip, gwip, 0, mask);
    ether.printIp(F("IP:   "), ether.myip);
    ether.printIp(F("GW:   "), ether.gwip);
    ether.printIp(F("Mask: "), ether.netmask);
    Serial.println(F("Open http://192.168.1.200/ in a browser, or curl it."));
  }
  delay(500);
}

unsigned long lastReport = 0;

void loop() {
  if (ether.packetLoop(ether.packetReceive())) {
    memcpy_P(ether.tcpOffset(), page, sizeof page);
    ether.httpServerReply(sizeof page - 1);
  }

  sensors.begin();  // safe every cycle -- re-enumerates the OneWire bus fresh
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

  if (millis() - lastReport > 2000) {
    lastReport = millis();
    Serial.print(F("Link: "));
    Serial.print(ether.isLinkUp() ? F("UP") : F("DOWN"));
    Serial.print(F("   Sensors: "));
    Serial.println(n);
  }
}
