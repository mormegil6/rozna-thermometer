/*
 * diag_04_ethernet  --  Stage A bring-up #4 (Rozna Arduino Thermometer)
 *
 * Goal: bring up the ENC28J60 over SPI, set the office static IP, and report
 * chip revision + link state. The board also answers ping while this runs.
 *
 * >>> Leonardo SPI gotcha <<<
 *   Hardware SPI on the Leonardo is on the ICSP header, NOT pins 11/12/13.
 *   The ENC28J60 shield must take MISO/MOSI/SCK from the ICSP header.
 *   Chip-select (CS) is an ordinary GPIO. The 2015 sketch used EtherCard's
 *   default CS, which back then was pin 8. Today EtherCard defaults CS to SS,
 *   which on a Leonardo is pin 17 -- so we set CS = 8 EXPLICITLY here to match
 *   the original, known-good wiring.
 *
 * Wiring:
 *   ENC28J60 shield seated; SPI via ICSP header; CS -> D8; module on 3.3V.
 *
 * Serial: 9600 baud.
 *
 * ---- PASS looks like ----
 *   - "ENC28J60 found, rev N"   (N is 1..7; means ether.begin() succeeded)
 *   - IP/GW/Mask printed as 192.168.1.200 / 192.168.1.1 / 255.255.255.0
 *   - "Link: UP"  (cable in a live switch/router port)
 *   - From a PC on 192.168.1.x:   ping 192.168.1.200   -> gets replies
 *
 * ---- If it FAILS ----
 *   - "begin() returned 0": SPI isn't reaching the chip. On Leonardo this is
 *     almost always (a) SPI not routed through ICSP, or (b) wrong CS pin.
 *     Change CS_PIN below and re-try in this order: 8 -> 10 -> 17 -> 12.
 *   - rev printed but "Link: DOWN": dead cable / switch port, or the module
 *     isn't getting 3.3V.
 *   - Link UP but no ping reply: IP/subnet/gateway mismatch, or some other
 *     host already owns 192.168.1.200.
 *
 * NOTE: a static-IP ENC28J60 does NOT request a DHCP lease, so it will NOT
 * show up in the router's "connected devices"/DHCP list. Verify with ping/ARP,
 * not the router UI. (That alone may explain the 2015 "never appears" symptom.)
 */

#include <EtherCard.h>

#define CS_PIN 8

// Locally-administered MAC (02:..) so it can't collide with the textbook
// DE:AD:BE:EF:FE:ED that every other ENC28J60 example ships with.
static byte mymac[] = { 0x02, 0x52, 0x6F, 0x7A, 0x6E, 0x61 };  // "02 R o z n a"
static byte myip[]  = { 192, 168, 1, 200 };
static byte gwip[]  = { 192, 168, 1, 1 };
static byte mask[]  = { 255, 255, 255, 0 };

byte Ethernet::buffer[700];

void setup() {
  Serial.begin(9600);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) { }
  Serial.println(F("=== diag_04_ethernet ==="));
  Serial.print(F("Using CS pin D"));
  Serial.println(CS_PIN);

  uint8_t rev = ether.begin(sizeof Ethernet::buffer, mymac, CS_PIN);
  if (rev == 0) {
    Serial.println(F("FAIL: ether.begin() returned 0 (ENC28J60 not found over SPI)."));
    Serial.println(F("Check SPI via ICSP, the CS pin, and 3.3V power to the module."));
    return;
  }
  Serial.print(F("ENC28J60 found, rev "));
  Serial.println(rev);

  ether.staticSetup(myip, gwip, 0, mask);
  ether.printIp(F("IP:   "), ether.myip);
  ether.printIp(F("GW:   "), ether.gwip);
  ether.printIp(F("Mask: "), ether.netmask);
  Serial.println(F("Now ping 192.168.1.200 from a PC on the same subnet."));
}

unsigned long lastReport = 0;

void loop() {
  // Service the stack so the board answers ARP + ICMP echo (ping).
  ether.packetLoop(ether.packetReceive());

  if (millis() - lastReport > 2000) {
    lastReport = millis();
    Serial.print(F("Link: "));
    Serial.println(ether.isLinkUp() ? F("UP") : F("DOWN"));
  }
}
