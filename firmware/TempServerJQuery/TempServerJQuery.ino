/*
 * Rožna Arduino Thermometer — modernized core sketch (2026 revival)
 * Original built 2015 (Ljubljana). Assembled by Bartłomiej Mróz.
 *
 * Board: Arduino Leonardo (ATmega32u4).  Ceilings: ~28 KB flash, 2560 B RAM.
 * Hardware:
 *   - DS18B20 on D10 (OneWire + DallasTemperature), 4.7k pull-up DATA->5V
 *   - 16x2 HD44780 LCD, 4-bit: RS=D7 E=D6 D4=D5 D5=D4 D6=D3 D7=D2, RW=GND
 *   - ENC28J60 (EtherCard): SPI via the Leonardo ICSP header, CS=D8, RST=D9
 *       NOTE: do NOT rely on EtherCard's default CS — it drifted from pin 8
 *       (old) to SS=pin 17 (current) on the Leonardo. We force CS=8 here to
 *       match the known-good 2015 wiring (see diagnostics/diag_04_ethernet).
 *       RST->D9 is REQUIRED: the module needs a hardware-reset pulse or its PHY
 *       won't link (this was the actual 2015 "never appears on the router" bug).
 *
 * What changed vs the 2015 sketch:
 *   - MAC is now locally-administered (02:..) instead of DE:AD:BE:EF:FE:ED.
 *   - Office static IP 192.168.1.200 / gw .1 / mask /24, DNS = gateway.
 *   - Serial time-sync (which froze the board at boot waiting for a PC) is
 *     replaced by NTP over EtherCard; LCD falls back to uptime if NTP is
 *     unavailable, so the board NEVER blocks at boot.
 *   - Adafruit SleepyDog watchdog (safe on the 32u4 bootloader).
 *   - ENC28J60 resilience: a hardware RST pulse (D9) plus a bounded SPI probe
 *     BEFORE ether.begin() (which otherwise spins forever on an unresponsive
 *     chip). If the NIC is missing/flaky the board runs OFFLINE (LCD + sensor
 *     only) and keeps retrying in the background — temperature monitoring never
 *     depends on the network. This is the "adapter dies, needs a restart" fix.
 *   - Web assets (jQuery / jQuery-Mobile / date.format) load from the
 *     code.jquery.com CDN over HTTPS; the Arduino still serves only the tiny
 *     HTML shell + main.css + main.js + /list.json (the 284 KB of JS/CSS can
 *     never fit in 28 KB of flash).
 *   - Dead s3.postimg.org footer image removed; the "Assembled by …" credit,
 *     uptime and free-RAM footer are kept.
 *
 * Alerting (Telegram) is intentionally NOT here: the ENC28J60 can't do TLS,
 * so a poller on the Raspberry Pi reads /list.json and sends alerts. The
 * Arduino stays "dumb". See README.
 */

#define REQUIRESALARMS false        // DallasTemperature: no alarm code

#include <OneWire.h>
#include <DallasTemperature.h>
#include <EtherCard.h>
#include <SPI.h>
#include <LiquidCrystal.h>
#include <TimeLib.h>
#include <Adafruit_SleepyDog.h>
#include <avr/wdt.h>

// ----------------------------- configuration -----------------------------
#define ETH_ENABLED     1           // set 0 to run as a pure thermometer (no NIC access at all)
#define USE_NTP         1           // set 0 to skip NTP entirely (LCD shows uptime; web never stalls)
#define DEBUG           0           // 1 = print a 3 s Serial heartbeat; 0 = quiet + leaner for deployment
#define CS_PIN          8           // ENC28J60 chip-select (NOT the SS default!)
#define ETH_RST_PIN     9           // ENC28J60 RST (active-low) -> wire to D9. Comment out if unwired.
#define ONE_WIRE_BUS    10          // DS18B20 data pin

// Office network (192.168.1.x). Edit here if your subnet differs.
static byte mymac[] = { 0x02, 0x52, 0x6F, 0x7A, 0x6E, 0x61 };   // locally-administered "02 R o z n a"
static byte myip[]  = { 192, 168, 1, 200 };
static byte gwip[]  = { 192, 168, 1, 1 };
static byte dnsip[] = { 192, 168, 1, 1 };   // router usually serves DNS; use 8.8.8.8 if not
static byte mask[]  = { 255, 255, 255, 0 };

// LCD clock zone: standard (winter) offset, Poland/CET = +1 h. EU summer time is added in localNow().
#define UTC_STD_OFFSET_SEC  (1L * 3600L)
// Fixed NTP server IP (Google Public NTP), not a hostname: ether.dnsLookup()
// can block for ~30 s with the watchdog paused if a query never gets a reply,
// freezing the LCD along with everything else. Skipping DNS entirely removes
// that risk; the actual time request below is already non-blocking.
static const byte NTP_SERVER_IP[] = { 216, 239, 35, 4 };
#define NTP_SRCPORT     0x42
#define NTP_SYNC_MS     3600000UL           // resync once an hour after first success
#define NTP_RETRY_MS    20000UL             // retry this often until first success
#define ETH_REINIT_MS   30000UL             // link never came up this long: re-init in place
#define ETH_RETRY_MS    15000UL             // when offline, re-probe + bring up the NIC this often
#define LINK_CHECK_MS   2000UL              // how often to read PHY link status
#define PROBE_MS        30000UL             // link up: ARP the gateway this often
#define AUDIT_MS        10000UL             // read the chip's key registers this often once the boot audit passed
#define SILENT_RESTART_MS 300000UL          // link up and no gateway ARP reply this long: restart the MCU
#define SILENT_FAST_MS  120000UL            // same, but once the chip has flagged a receive overflow (jam evidence)
#define LINK_DOWN_RESTART_MS 180000UL       // link down this long after it was up: restart the MCU
#define BACKOFF_MAX     2                   // consecutive restarts double the windows, up to 4x (was 16x: a bad night on 2026-09-26/27 showed a dead chip waiting up to 160 min for LINK_DOWN_RESTART_MS<<4)
#define STREAK_CLEAR_MS 1800000UL           // this much healthy uptime forgets earlier restarts
#define MIN_UPTIME_MS   60000UL             // minimum uptime before a NIC-fault restart
#define WDT_MS          8000                // SleepyDog window (AVR max ~8s)
#define LCD_REFRESH_MS  500                 // repaint ~2x/s so the seconds clock never skips
#define SENSOR_RES      11                  // DS18B20 resolution bits 9..12 (11 = 0.125 C, ~375 ms)
#define SENSOR_INTERVAL_MS 1500             // start a new (async) conversion this often
#define SENSOR_RESCAN_MS 3000UL             // no probe known: look for a newly plugged one this often
#define CONV_DELAY_MS   ((750 >> (12 - SENSOR_RES)) + 40)   // wait for conversion before reading
#define NTP_EPOCH_1900  2208988800UL        // seconds between 1900 and 1970

// The footer logo is a real raster image, so it's browser-loaded from an external
// host (the Arduino can't serve binaries: 28 KB flash + a ~1 KB page buffer).
// Currently on ImgBB; swap for your own host / public-repo jsDelivr URL anytime.
#define FOOTER_LOGO_URL "https://i.ibb.co/BKN52Q7w/arduino-logo-hugodemiglio.png"  // 256x256, downscaled in CSS
#define ASSET_VER       "9"         // cache-bust: bump when main.css/main.js/det.js change so browsers refetch
// The favicon stays the self-contained 🌡️ emoji, but is served from its OWN tiny
// route (/favicon.svg) and referenced by a SHORT href — so it never bloats the
// home page. (Inlining it as a data: URI overflowed the buffer and reset the board.)

// ------------------------------- objects ---------------------------------
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
LiquidCrystal lcd(7, 6, 5, 4, 3, 2);        // RS, E, D4, D5, D6, D7
byte Ethernet::buffer[1150];                // TCP/IP send+receive buffer: must hold the whole frame (Eth/IP/TCP headers + the largest HTTP response). With the detail JS split into det.js the largest response is the home page (~1 KB), so this fits with margin.
static BufferFiller bfill;

// -------------------------------- state ----------------------------------
static uint8_t  ntpServerIp[IP_LEN];
static bool     haveNtpServer = false;
static bool     timeSynced    = false;
static uint32_t lastNtpReq    = 0;
static uint32_t lastLinkUp    = 0;
static uint32_t lastLcd       = 0;
#if DEBUG
static uint32_t lastStatus    = 0;
#endif
static uint32_t lastEthTry    = 0;
static uint32_t lastLinkChk   = 0;
static uint32_t lastAck       = 0;       // last ARP reply from the gateway, or last link DOWN to UP edge
static uint32_t lastProbe     = 0;
static bool     probeMiss     = false;   // a probe went unanswered (noted once on Serial)
static bool     ovfSeen       = false;   // the RX overflow flag was noted once on Serial
static bool     auditOk       = false;   // the boot audit passed, so a later mismatch is drift, not a misread
static uint8_t  auditBad      = 0;       // consecutive periodic audits that failed
static uint32_t lastAudit     = 0;
static bool     rxSeen        = false;   // link came up or a frame was read: never re-init the chip in place
static bool     ethReady      = false;   // true only when ether.begin() has succeeded
static bool     linkUp        = false;   // cached PHY link state (updated on a timer)
float celsius, fahrenheit;
static bool     haveReading   = false;   // true once we have a valid reading
static bool     converting    = false;   // an async DS18B20 conversion is in progress
static uint32_t convStart     = 0;
static uint32_t lastConvReq   = 0;
static uint32_t lastRescan    = 0;

// Survive a watchdog restart (.noinit is neither cleared nor initialised at startup).
#define NV_MAGIC 0xB007
static uint16_t nvMagic  __attribute__((section(".noinit")));   // NV_MAGIC after restartMcu() only
static uint8_t  nvStreak __attribute__((section(".noinit")));   // consecutive self-restarts
static uint8_t  nvTotal  __attribute__((section(".noinit")));   // self-restarts since power-up (JSON, Serial)
static uint32_t nvTime   __attribute__((section(".noinit")));   // UTC clock across a restart, 0 = none
static uint8_t  nvWhy    __attribute__((section(".noinit")));   // reason code of the last self-restart, 0 = none
#define DBG_LEN 12                       // chip snapshot bytes, see encSnapshot()
static uint8_t  nvDbg[DBG_LEN] __attribute__((section(".noinit")));   // chip snapshot taken just before it

// ------------------------------- helpers ---------------------------------
// Caterina already clears MCUSR and stops the watchdog; this covers a bootloader that does not.
void wdtEarlyOff() __attribute__((naked, used, section(".init3")));
void wdtEarlyOff() { MCUSR = 0; wdt_disable(); }

static void encSnapshot(uint8_t out[DBG_LEN]);

// Chip snapshot bytes: ECON1 ESTAT EIR MACON1 MACON3 ERXFCON MAC-mismatch-mask EPKTCNT ERXRDPTL/H ERXWRPTL/H.
static void printBytes(const uint8_t *p) {       // one USB write, so a host that is not reading stalls us once, not per byte
  char b[3 * DBG_LEN + 2], *q = b;
  for (uint8_t i = 0; i < DBG_LEN; i++) {
    uint8_t hi = p[i] >> 4, lo = p[i] & 15;
    *q++ = ' '; *q++ = hi < 10 ? '0' + hi : 'A' + hi - 10; *q++ = lo < 10 ? '0' + lo : 'A' + lo - 10;
  }
  *q++ = '\r'; *q++ = '\n';
  Serial.write((const uint8_t *)b, q - b);
}

static void printChip(const __FlashStringHelper *what, const uint8_t *s) {
  Serial.print(what); Serial.print(F(" at ")); Serial.print(millis() / 1000UL); Serial.print(F(" s, chip:"));
  printBytes(s);
}

static void printWhy() {
  Serial.print(F(", why=")); Serial.print(nvWhy); Serial.print(F(", chip:"));
  printBytes(nvDbg);
}

// The only way to reset EtherCard's private RX pointers (function-local statics in packetReceive()).
static void restartMcu(const __FlashStringHelper *why, uint8_t code) __attribute__((noreturn));
static void restartMcu(const __FlashStringHelper *why, uint8_t code) {
  nvWhy = code;
  encSnapshot(nvDbg);
  nvTime = (timeStatus() == timeNotSet) ? 0 : (uint32_t)now();
  if (nvStreak < 200) nvStreak++;            // all bookkeeping before the prints: a watchdog reset during a stalled
  if (nvTotal < 250) nvTotal++;              // print must still look like a self-restart and keep the evidence
  nvMagic = NV_MAGIC;
  Serial.print(F("Restarting MCU: "));
  Serial.print(why);
  printWhy();
  Serial.flush();                            // cli() below stops the 1 ms USB flush, so hand the FIFO over now
  cli();
  *(volatile uint16_t *)0x0800 = 0;          // boot key 0x7777 would hold Caterina for 8 s
  *(volatile uint16_t *)(RAMEND - 1) = 0;    // same key at the newer bootloader location
  wdt_enable(WDTO_15MS);
  for (;;) {}
}

// Shift that lengthens every restart window after consecutive self-restarts, so a restart loop slows down.
static uint8_t backoff() {
  return nvStreak > BACKOFF_MAX ? BACKOFF_MAX : nvStreak;
}

// End of the flash image (.text + .data), the same figure the IDE prints as "Sketch uses N bytes".
extern "C" char __data_load_end;
static uint16_t flashUsed() { return (uint16_t)(uintptr_t)&__data_load_end; }

static int freeRam() {
  extern int __heap_start, *__brkval;
  int v;
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval);
}

// Pulse the ENC28J60 hardware reset line (active-low). Harmless if RST isn't
// wired to ETH_RST_PIN; if it is, this gives the chip a clean reset, which
// helps recover a locked-up module far more reliably than a software re-init.
static void hardResetEthernet() {
#ifdef ETH_RST_PIN
  pinMode(ETH_RST_PIN, OUTPUT);
  digitalWrite(ETH_RST_PIN, LOW);    // assert active-low reset
  delay(5);
  digitalWrite(ETH_RST_PIN, HIGH);   // release
  delay(100);                        // let the oscillator/PHY start (proven timing)
#endif
}

// Write a byte to the ENC28J60 EWRPTL scratch register (bank 0) and read it
// back. EWRPTL is harmless to clobber (ether.begin() reinitialises it).
static uint8_t encScratch(uint8_t val) {
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(0x40 | 0x02);   // WCR | EWRPTL
  SPI.transfer(val);
  digitalWrite(CS_PIN, HIGH);
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(0x00 | 0x02);   // RCR | EWRPTL
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(CS_PIN, HIGH);
  return v;
}

// Read a control register (RCR). Assumes an SPI transaction is already active.
static uint8_t encReadReg(uint8_t addr) {
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(0x00 | (addr & 0x1F));   // RCR opcode
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(CS_PIN, HIGH);
  return v;
}

// Bounded probe used BEFORE ether.begin(). Two checks, neither of which can
// hang:
//   1) SPI link solid? (write-readback two patterns — floating MISO can't fake)
//   2) Oscillator actually running? (poll ESTAT.CLKRDY for up to ~50 ms)
// Check (2) is the important one: SPI register access is clocked by SCK and
// works even if the ENC's 25 MHz crystal is dead, so without it ether.begin()
// would spin forever on CLKRDY. If this returns false we stay OFFLINE and keep
// running the thermometer.
static bool enc28j60Responds() {
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);
  SPI.begin();
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  bool spiOk = (encScratch(0xAB) == 0xAB) && (encScratch(0x54) == 0x54);
  bool clkrdy = false;
  for (uint8_t i = 0; spiOk && !clkrdy && i < 50; i++) {
    uint8_t es = encReadReg(0x1D);                  // ESTAT
    if (es != 0xFF && (es & 0x01)) clkrdy = true;   // CLKRDY (oscillator ready)
    else delay(1);
  }
  SPI.endTransaction();
  if (spiOk && !clkrdy)
    Serial.println(F("ENC oscillator not ready (CLKRDY low) - crystal/power fault"));
  return spiOk && clkrdy;
}

// Same idea, but usable WHILE EtherCard is running: saves/restores the SPI
// registers the driver relies on. Use this instead of ether.isLinkUp() to
// decide whether it's safe to touch the chip — isLinkUp()'s MII busy-wait can
// hang forever on a flaky chip; this never blocks.
static bool enc28j60Healthy() {
  uint8_t spcr = SPCR, spsr = SPSR;
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  uint8_t e1 = encReadReg(0x1F);                          // ECON1 (same in every bank)
  uint8_t es = encReadReg(0x1D);                          // ESTAT
  // Register 0x02 is MACON3 or MAADR3 in banks 2 and 3 (isLinkUp() leaves bank 2): do not write it there.
  bool ok = (e1 & 0x03) > 1 || ((encScratch(0xAB) == 0xAB) && (encScratch(0x54) == 0x54));
  SPI.endTransaction();
  SPCR = spcr;                                            // restore EtherCard's SPI config
  SPSR = (SPSR & ~_BV(SPI2X)) | (spsr & _BV(SPI2X));
  return ok && (es != 0xFF) && (es & 0x01) && (e1 & 0x04);   // SPI ok, oscillator running, RXEN still set
}

// ECON1.BSEL only: EtherCard caches the selected bank and never re-reads it.
static void encBsel(uint8_t bank) {
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(0xA0 | 0x1F);            // BFC ECON1
  SPI.transfer(0x03);
  digitalWrite(CS_PIN, HIGH);
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(0x80 | 0x1F);            // BFS ECON1
  SPI.transfer(bank & 0x03);
  digitalWrite(CS_PIN, HIGH);
}

// reg is the driver's id (addr | bank << 5 | 0x80 for MAC/MII); the caller holds the SPI transaction.
static uint8_t encReadBanked(uint8_t reg) {
  uint8_t bsel = encReadReg(0x1F) & 0x03;
  encBsel(reg >> 5);
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(reg & 0x1F);
  if (reg & 0x80) SPI.transfer(0x00);   // MAC and MII registers return a dummy byte first
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(CS_PIN, HIGH);
  encBsel(bsel);
  if ((encReadReg(0x1F) & 0x03) != bsel) encBsel(bsel);
  return v;
}

// ESTAT reads 0xFF only when no chip answers; then every byte is 0xFF.
static void encSnapshot(uint8_t out[DBG_LEN]) {
  // MACON1 MACON3 ERXFCON, MAADR5..0 (= mymac[0..5]), then EPKTCNT and the RX read and write pointers
  static const uint8_t REGS[] PROGMEM = { 0xC0, 0xC2, 0x38, 0xE4, 0xE5, 0xE2, 0xE3, 0xE0, 0xE1, 0x39, 0x0C, 0x0D, 0x0E, 0x0F };
  uint8_t spcr = SPCR, spsr = SPSR;
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  out[0] = encReadReg(0x1F);
  out[1] = encReadReg(0x1D);
  out[2] = encReadReg(0x1C);
  if (out[1] == 0xFF) {
    memset(out, 0xFF, DBG_LEN);
  } else {
    out[6] = 0;
    for (uint8_t i = 0; i < 14; i++) {
      uint8_t v = encReadBanked(pgm_read_byte(REGS + i));
      if (i < 3) out[3 + i] = v;
      else if (i < 9) { if (v != mymac[i - 3]) out[6] |= 1 << (i - 3); }
      else out[i - 2] = v;              // EPKTCNT -> 7, ERXRDPTL/H -> 8, 9, ERXWRPTL/H -> 10, 11
    }
  }
  SPI.endTransaction();
  SPCR = spcr;                          // restore EtherCard's SPI config
  SPSR = (SPSR & ~_BV(SPI2X)) | (spsr & _BV(SPI2X));
}

// Errata DS80349C item 14: an even ERXRDPT can corrupt the receive buffer (next packet pointer and status vector).
// EtherCard's initialize() leaves it at 0. Use ERXND (odd), which is what its own packetReceive() writes when the
// next packet is at 0, so the driver's pointer stays consistent. Low byte first: it is buffered until the high byte.
static void encFixRxPtr() {
  uint8_t spcr = SPCR, spsr = SPSR;
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  uint8_t bsel = encReadReg(0x1F) & 0x03;
  encBsel(0);
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(0x40 | 0x0C);            // WCR ERXRDPTL
  SPI.transfer(RXSTOP_INIT & 0xFF);
  digitalWrite(CS_PIN, HIGH);
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(0x40 | 0x0D);            // WCR ERXRDPTH
  SPI.transfer(RXSTOP_INIT >> 8);
  digitalWrite(CS_PIN, HIGH);
  encBsel(bsel);
  if ((encReadReg(0x1F) & 0x03) != bsel) encBsel(bsel);
  SPI.endTransaction();
  SPCR = spcr;                          // restore EtherCard's SPI config
  SPSR = (SPSR & ~_BV(SPI2X)) | (spsr & _BV(SPI2X));
}

// What EtherCard's initialize() leaves in the chip: RXEN, MACON1, MACON3, ERXFCON, and every MAC byte right.
static bool chipGood(const uint8_t *s) {
  return (s[0] & 0x04) && s[3] == 0x01 && s[4] == 0x32 && s[5] == 0xB1 && s[6] == 0;
}

// Probe -> hardware reset -> ether.begin -> static IP. Returns true only if the
// chip is present and begin() succeeds. Safe to call from setup() or loop():
// it can't hang, because we never call ether.begin() on an unresponsive chip.
// Once rxSeen is set it restarts the MCU instead of re-initialising in place.
static bool bringUpEthernet() {
#if !ETH_ENABLED
  return false;                      // Ethernet disabled at compile time
#else
  if (rxSeen) restartMcu(F("NIC re-init after traffic"), 3);
  hardResetEthernet();
  if (!enc28j60Responds()) {
    Serial.println(F("ENC28J60 not responding on SPI -> OFFLINE (LCD only)"));
    return false;
  }
  if (ether.begin(sizeof Ethernet::buffer, mymac, CS_PIN) == 0) {
    Serial.println(F("ether.begin() failed -> OFFLINE"));
    return false;
  }
  encFixRxPtr();
  ether.staticSetup(myip, gwip, dnsip, mask);
  ether.printIp(F("IP: "), ether.myip);
  haveNtpServer = false;          // re-adopt the fixed NTP server IP after a (re)init
  lastLinkUp = millis();
  Serial.println(F("Ethernet UP"));
  return true;
#endif
}

// Hand-built ARP who-has for the gateway: EtherCard's LAN ping may use a MAC it never learned.
static const uint8_t ARP_REQ_HDR[] PROGMEM = { 0, 1, 8, 0, 6, 4, 0, 1 };   // Ethernet, IPv4, 6, 4, request
static void probeGateway() {
  uint8_t *b = Ethernet::buffer;
  memset(b + ETH_DST_MAC, 0xFF, ETH_LEN);
  memcpy(b + ETH_SRC_MAC, mymac, ETH_LEN);
  b[ETH_TYPE_H_P] = ETHTYPE_ARP_H_V;
  b[ETH_TYPE_H_P + 1] = ETHTYPE_ARP_L_V;
  memcpy_P(b + ETH_ARP_P, ARP_REQ_HDR, sizeof ARP_REQ_HDR);
  memcpy(b + ETH_ARP_SRC_MAC_P, mymac, ETH_LEN);
  memcpy(b + ETH_ARP_SRC_IP_P, myip, IP_LEN);
  memset(b + ETH_ARP_DST_MAC_P, 0, ETH_LEN);
  memcpy(b + ETH_ARP_DST_IP_P, gwip, IP_LEN);
  ether.packetSend(42);
}

// Only this reply proves a short frame got out; long NTP answers would mask that.
static bool isGatewayArpReply(const uint8_t *b, uint16_t len) {
  return len >= 42 && b[ETH_TYPE_H_P] == ETHTYPE_ARP_H_V && b[ETH_TYPE_H_P + 1] == ETHTYPE_ARP_L_V &&
         b[ETH_ARP_OPCODE_L_P] == ETH_ARP_OPCODE_REPLY_L_V &&
         memcmp(b + ETH_ARP_SRC_IP_P, gwip, IP_LEN) == 0 && memcmp(b + ETH_ARP_DST_IP_P, myip, IP_LEN) == 0;
}

// Grace after link-up or bring-up: the first probe leaves 5 s from now.
static void armProbe() {
  lastAck = millis();
  lastProbe = lastAck - PROBE_MS + 5000UL;
}

// Called every loop. Never blocks: the NTP server is a fixed IP (see
// NTP_SERVER_IP above), so there is no DNS lookup here to stall on, and
// ether.ntpRequest() below is itself non-blocking.
static void serviceTime() {
#if USE_NTP
  // Adopt the fixed server IP once the link is up (use the CACHED state —
  // never call the hang-prone isLinkUp() here).
  if (linkUp && !haveNtpServer) {
    ether.copyIp(ntpServerIp, NTP_SERVER_IP);
    haveNtpServer = true;
    ether.printIp(F("NTP: "), ntpServerIp);
  }

  if (haveNtpServer) {
    uint32_t interval = timeSynced ? NTP_SYNC_MS : NTP_RETRY_MS;
    if (lastNtpReq == 0 || millis() - lastNtpReq > interval) {
      ether.ntpRequest(ntpServerIp, NTP_SRCPORT);   // non-blocking
      lastNtpReq = millis();
    }
  }
#endif
}

static void lcdDigits(int v) {        // zero-padded 2-digit field
  if (v < 10) lcd.print('0');
  lcd.print(v);
}

// Format a temperature to 1 decimal, RIGHT-justified in `width` cells, using
// integer math only — so the heavy float-to-string code is never linked in.
static void lcdTempField(float t, uint8_t width) {
  int t10 = (int)(t * 10 + (t < 0 ? -0.5f : 0.5f));   // round to tenths
  bool neg = t10 < 0;
  unsigned v = neg ? -t10 : t10;
  char b[8]; uint8_t n = 0;
  b[n++] = '0' + (v % 10); v /= 10;                   // fractional digit
  b[n++] = '.';
  do { b[n++] = '0' + (v % 10); v /= 10; } while (v); // whole digits
  if (neg) b[n++] = '-';
  while (n < width) b[n++] = ' ';                     // left-pad to width
  while (n) lcd.write(b[--n]);                        // emit reversed -> right-justified
}

// Non-blocking DS18B20 service: start a conversion on a timer and read the result
// ~CONV_DELAY_MS later. waitForConversion is false (set in setup), so this never
// stalls the loop (the old code blocked ~750 ms here AND again in /list.json).
static void serviceSensor() {
  // DallasTemperature counts devices only in begin(), so a probe plugged in after boot is
  // invisible to getTempCByIndex(). begin() sleeps 150 ms with nothing attached: probe first.
  if (!converting && sensors.getDeviceCount() == 0 && millis() - lastRescan > SENSOR_RESCAN_MS) {
    lastRescan = millis();
    uint8_t rom[8];
    oneWire.reset_search();
    if (oneWire.search(rom) && OneWire::crc8(rom, 7) == rom[7]) {
      sensors.begin();
      DeviceAddress a;
      // A fresh probe is 12-bit; setResolution() writes its EEPROM, so only when it differs.
      if (sensors.getAddress(a, 0) && sensors.getResolution(a) != SENSOR_RES) sensors.setResolution(SENSOR_RES);
    }
  }
  if (!converting && (lastConvReq == 0 || millis() - lastConvReq > SENSOR_INTERVAL_MS)) {
    sensors.requestTemperatures();        // returns immediately (async)
    converting = true;
    convStart = millis();
    lastConvReq = millis();
  }
  if (converting && millis() - convStart >= CONV_DELAY_MS) {
    converting = false;
    float c = sensors.getTempCByIndex(0); // just reads the scratchpad now (fast)
    haveReading = (c != DEVICE_DISCONNECTED_C);
    if (haveReading) {
      celsius = c;
      fahrenheit = DallasTemperature::toFahrenheit(c);
    }
  }
}

// EU summer time: 01:00 UTC on the last Sunday of March until 01:00 UTC on the last Sunday of October.
static time_t localNow() {
  time_t u = now();
  uint8_t m = month(u), d = day(u), w = weekday(u);
  bool past = d - w >= 24 && (w > 1 || hour(u));   // last Sunday 01:00 UTC reached (weekday 1 = Sunday)
  if ((m > 3 && m < 10) || (m == 3 ? past : m == 10 && !past)) u += 3600L;
  return u + UTC_STD_OFFSET_SEC;
}

// One LCD cell of network state: O = NIC offline, L = no link, S = link up but no gateway ARP reply for 90 s, blank = fine.
static char netChar() {
#if !ETH_ENABLED
  return ' ';
#else
  if (!ethReady) return 'O';
  if (!linkUp) return 'L';
  return (millis() - lastAck > 3 * PROBE_MS) ? 'S' : ' ';
#endif
}

// Repaint the LCD from the CACHED reading, writing fixed-width fields IN PLACE
// (no lcd.clear(), so no flicker). 16x2 layout, each field a constant width:
//   row0: "TTTTT" C r "HH:MM:SS"    row1: "TTTTT" F n "DD.MM.YY"   (TTTTT = 5 cells)
// r = recent self-restarts (1-9, + for more; blank when healthy); n = netChar(), blank when fine.
static void updateLcd() {
  time_t t = localNow();
  bool haveClock = timeStatus() != timeNotSet;              // NTP, or carried across a self-restart
  lcd.setCursor(0, 0);
  if (haveReading) lcdTempField(celsius, 5); else lcd.print(F(" --.-"));
  lcd.write((uint8_t)223); lcd.write('C');                  // degree glyph + unit
  lcd.write((uint8_t)(nvStreak == 0 ? ' ' : nvStreak < 10 ? '0' + nvStreak : '+'));
  if (haveClock) {
    lcdDigits(hour(t)); lcd.write(':'); lcdDigits(minute(t)); lcd.write(':'); lcdDigits(second(t));
  } else {
    unsigned long up = millis() / 1000UL;             // uptime fallback (8 cells)
    lcdDigits((int)((up / 3600UL) % 100)); lcd.write(':');
    lcdDigits((int)((up / 60UL) % 60UL));  lcd.write(':');
    lcdDigits((int)(up % 60UL));
  }

  lcd.setCursor(0, 1);
  if (haveReading) lcdTempField(fahrenheit, 5); else lcd.print(F(" --.-"));
  lcd.write((uint8_t)223); lcd.write('F'); lcd.write(netChar());
  if (haveClock) {
    lcdDigits(day(t)); lcd.write('.'); lcdDigits(month(t)); lcd.write('.'); lcdDigits(year(t) % 100);
  } else {
    lcd.print(F("no NTP  "));                          // 8 cells, matches the date field
  }
}

// ------------------------------- web pages -------------------------------
static void writeHeaders(BufferFiller& buf) {           // dynamic content: never cache
  buf.print(F("HTTP/1.0 200 OK\r\nPragma: no-cache\r\n"));
}

static void writeHeadersCached(BufferFiller& buf) {     // static assets: let the browser cache a day
  buf.print(F("HTTP/1.0 200 OK\r\nCache-Control: max-age=86400\r\n"));
}

static void homePage(BufferFiller& buf) {
  writeHeaders(buf);
  buf.println(F("Content-Type: text/html\r\n"));
  buf.print(F(
    "<!DOCTYPE html><html><head>"
    "<meta charset=\"utf-8\">"
    "<meta name='viewport' content='width=device-width, initial-scale=1' />"
    "<title>Rožna Arduino Thermometer</title>"
    "<link rel='icon' href='favicon.svg'>"
    "<link rel='stylesheet' href='https://code.jquery.com/mobile/1.2.0/jquery.mobile-1.2.0.min.css' />"
    "<link rel='stylesheet' href='main.css?v=" ASSET_VER "' />"
    "<script src='https://code.jquery.com/jquery-1.8.2.min.js'></script>"
    "<script src='https://code.jquery.com/mobile/1.2.0/jquery.mobile-1.2.0.min.js'></script>"
    "<script src='https://stevenlevithan.com/assets/misc/date.format.js'></script>"
    "<script src='main.js?v=" ASSET_VER "'></script>"
    "<script src='det.js?v=" ASSET_VER "'></script>"
    "</head><body><div id='main' data-role='page'>"
    "<div data-role='header' data-position='fixed'><h3>Rožna Arduino<br>Thermometer</h3></div>"
    "<div data-role='content'>"
    "<ul id='list' data-role='listview' data-inset='true'></ul>"
    "<p id='info'></p></div></div></body></html>"));
}

static void mainCss(BufferFiller& buf) {
  writeHeadersCached(buf);
  buf.println(F("Content-Type: text/css\r\n"));
  buf.print(F(
    /* our own row layout (jQM never reorders these): name left, temp right, vertically centered */
    "#list .row{display:-webkit-flex;display:flex;-webkit-align-items:center;align-items:center;-webkit-justify-content:space-between;justify-content:space-between;}"
    "#list .tp{font-weight:bold;font-size:xx-large;white-space:nowrap;}"
    "#list .tp>sup{font-size:large;}"
    "IMG.image{display:block;margin-left:auto;margin-right:auto;width:96px;}"
    "#list .ui-icon{-webkit-transition:-webkit-transform .2s;transition:transform .2s;}"
    "#list li.op .ui-icon{-webkit-transform:rotate(90deg);transform:rotate(90deg);}"
    "#info{bottom:0;position:fixed;text-align:center;width:100%;margin-top:10px;font-size:small;}"));
}

// main.js: list rendering + polling + footer. The detail-panel logic lives in
// det.js (split out so neither response is large enough to need a big buffer).
// Globals D (latest data) and C (open row index, -1 = none) are shared with det.js.
static void mainJs(BufferFiller& buf) {
  writeHeadersCached(buf);
  buf.println(F("Content-Type: application/javascript\r\n"));
  buf.print(F(
    "var D,C=-1;$(document).ready(function(){reload()});"
    "function reload(){$.getJSON('list.json',function(c){D=c;var d=[];"
    "$.each(c.list,function(a,b){d.push('<li><a href=\"#\" onclick=\"S('+a+');return false;\"><div class=\"row\"><span class=\"nm\">🌡️ '+b.name+'</span>"
    "<span class=\"tp\">'+b.ifnegative+''+b.val.toFixed(1)/100+'<sup>&deg;C</sup></span></div></a></li>')});"
    "$('#list').html(d.join('')).trigger('create').listview('refresh');if(C>=0)R(C);var e=new Date(c.uptime);"
    "$('#info').html('<img class=\"image\" src=\"" FOOTER_LOGO_URL "\"><br>Assembled by Bartłomiej Mróz<br>Uptime: '+e.format('UTC:HH:MM:ss')+' (RAM '+(2560-c.free)+'/2560 B, flash '+(c.flash/1024).toFixed(1)+'/28 KB)')});setTimeout(reload,14974)}"));
}

// det.js: the expandable detail panel. Injects the panel div, and R()/S() render
// + toggle it (and rotate the row's arrow 90deg via the 'op' class).
static void detJs(BufferFiller& buf) {
  writeHeadersCached(buf);
  buf.println(F("Content-Type: application/javascript\r\n"));
  buf.print(F(
    "$(document).ready(function(){$('div[data-role=content]').append('<div id=\"det\" class=\"ui-body ui-body-a ui-corner-all\" style=\"display:none;margin:6px 15px;padding:8px\"></div>')});"
    "function R(i){var b=D.list[i];if(!b)return;var c=b.val/100,n=new Date(D.now*1000),t=D.ntp?n.format('UTC:HH:MM:ss')+' '+n.format('UTC:dd.mm.yy'):'no NTP';$('#det').html('<h3>🌡️ '+b.name+'</h3><p>ROM '+b.id+'<br>'+c.toFixed(2)+'&deg;C / '+(c*9/5+32).toFixed(2)+'&deg;F<br>'+D.res+'-bit, pin D'+D.pin+'<br>clock '+t+'<br>up '+new Date(D.uptime).format('UTC:HH:MM:ss')+', RAM '+(2560-D.free)+'/2560 B<br>NTP '+(D.ntp?'ok':'no')+'</p>');$('#list li').removeClass('op').eq(i).addClass('op')}"
    "function S(i){if(C==i){C=-1;$('#det').slideUp();$('#list li').removeClass('op')}else{C=i;R(i);$('#det').slideDown()}}"));
}

// Tiny SVG favicon (the 🌡️ emoji) on its own route, so it never bloats the home
// page. Self-contained — no external host, renders on mobile too.
static void faviconSvg(BufferFiller& buf) {
  writeHeadersCached(buf);
  buf.println(F("Content-Type: image/svg+xml\r\n"));
  buf.print(F("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><text y='14' font-size='14'>🌡️</text></svg>"));
}

static void listJson(BufferFiller& buf) {
  writeHeaders(buf);
  buf.println(F("Content-Type: application/json\r\n"));
  buf.print(F("{\"list\":["));

  int index = 1;
  DeviceAddress addr;
  // No requestTemperatures() here — serviceSensor() runs conversions in the
  // background, so this just reads the latest scratchpad value (fast, no stall).
  oneWire.reset_search();
  while (oneWire.search(addr)) {
    if (index != 1) buf.write(',');
    float tempC = sensors.getTempC(addr);
    int tempCint = tempC * 100;
    const char* ifnegative = " ";
    if (tempCint < 0) { tempCint = -tempCint; ifnegative = "-"; }
    buf.emit_p(PSTR("{\"id\":\"$D$D$D$D$D$D$D$D\",\"name\":\"Sensor $D\",\"val\":$D,\"ifnegative\":\"$S\"}"),
               addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7],
               index, tempCint, ifnegative);
    index++;
  }
  buf.emit_p(PSTR("],\"uptime\":$L,\"free\":$D,\"flash\":$D,\"res\":$D,\"pin\":$D,\"ntp\":$D,\"now\":$L,\"rst\":$D,\"why\":$D,\"dbg\":[$D,$D,$D,$D,$D,$D,$D,$D,$D,$D,$D,$D]}"),
             millis(), freeRam(), (int)flashUsed(), SENSOR_RES, ONE_WIRE_BUS, timeSynced ? 1 : 0, (long)localNow(), nvTotal, nvWhy,
             nvDbg[0], nvDbg[1], nvDbg[2], nvDbg[3], nvDbg[4], nvDbg[5], nvDbg[6], nvDbg[7], nvDbg[8], nvDbg[9], nvDbg[10], nvDbg[11]);
}

static boolean checkUrl(const __FlashStringHelper *val, const char* data) {
  const char PROGMEM *p = (const char PROGMEM *)val;
  while (1) {
    char c = pgm_read_byte(p++);
    if (c == 0) break;
    if (*data != c) return false;
    data++;
  }
  return true;
}

// -------------------------------- setup ----------------------------------
void setup() {
  Serial.begin(9600);

  bool afterRestart = (nvMagic == NV_MAGIC);   // only restartMcu() sets it; anything else is a fresh start
  nvMagic = 0;
  if (!afterRestart) { nvStreak = nvTotal = nvWhy = 0; memset(nvDbg, 0, sizeof nvDbg); }
  else if (nvTime) setTime((time_t)nvTime);    // keep the wall clock through a self-restart

  // LCD + sensor first, with an instant splash, so the display is alive even
  // with no USB host and before the network is touched.
  sensors.begin();
  sensors.setResolution(SENSOR_RES);
  lcd.begin(16, 2);
  lcd.print(F("Rozna Thermo"));

  // One synchronous read so the splash shows a real temperature right away; from
  // here on conversions are async (non-blocking) — see serviceSensor().
  sensors.requestTemperatures();
  celsius = sensors.getTempCByIndex(0);
  haveReading = (celsius != DEVICE_DISCONNECTED_C);
  if (haveReading) fahrenheit = DallasTemperature::toFahrenheit(celsius);
  sensors.setWaitForConversion(false);

  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 1500) { }   // brief; never block headless
  Serial.println(F("=== Rozna Arduino Thermometer ==="));
  if (afterRestart) { Serial.print(F("Self-restart #")); Serial.print(nvTotal); printWhy(); }

  // Watchdog as an early backstop. 8 s is well above Caterina's startup time, so
  // a hang recovers via reset+retry instead of freezing/trapping the bootloader.
  Watchdog.enable(WDT_MS);

  updateLcd();                      // real temperature on screen

  // Bring up the NIC. SPI + CLKRDY are probed first, so ether.begin() only runs
  // on a genuinely-ready chip; a missing chip or a dead oscillator drops to
  // OFFLINE and the thermometer keeps running.
  ethReady = bringUpEthernet();
  if (ethReady) {
    armProbe();
    // Proves at every boot that the bank-safe reads agree with what the driver just configured.
    uint8_t s[DBG_LEN];
    encSnapshot(s);
    auditOk = chipGood(s);
    if (auditOk) Serial.println(F("Chip audit OK"));
    else { Serial.print(F("Chip audit BAD")); printBytes(s); }
  }
}

// --------------------------------- loop ----------------------------------
void loop() {
  Watchdog.reset();

  if (ethReady && Serial.available() && Serial.read() == 'd') {   // send "d" over USB for a chip snapshot on demand
    uint8_t s[DBG_LEN];
    encSnapshot(s);
    printChip(F("chip"), s);
  }

  if (ethReady) {
    // --- network is up: service the stack, web, NTP, link health ---
    word len = ether.packetReceive();
    if (len) {
      rxSeen = true;
      if (isGatewayArpReply(Ethernet::buffer, len)) {   // before packetLoop() touches the buffer
        lastAck = millis();
        if (probeMiss) { probeMiss = false; Serial.println(F("Gateway ARP answered again")); }
      }
    }
    word pos = ether.packetLoop(len);

    // NTP answer (UDP) arrives outside the TCP path; check the raw buffer.
    uint32_t ntp = 0;
    if (len && ether.ntpProcessAnswer(&ntp, NTP_SRCPORT)) {
      setTime((time_t)(ntp - NTP_EPOCH_1900));
      timeSynced = true;
      Serial.println(F("NTP sync OK"));
    }

    // Serve the web UI / API.
    if (pos) {
      bfill = ether.tcpOffset();
      char* data = (char *) Ethernet::buffer + pos;
      // Home is matched exactly (trailing space = root only). Assets are matched
      // as prefixes (no trailing space) so cache-busting query strings like
      // "GET /main.js?v=4" still route correctly.
      if      (checkUrl(F("GET / "),           data)) homePage(bfill);
      else if (checkUrl(F("GET /main.css"),    data)) mainCss(bfill);
      else if (checkUrl(F("GET /main.js"),     data)) mainJs(bfill);
      else if (checkUrl(F("GET /det.js"),      data)) detJs(bfill);
      else if (checkUrl(F("GET /favicon.svg"), data)) faviconSvg(bfill);
      else if (checkUrl(F("GET /list.json"),   data)) listJson(bfill);
      else bfill.print(F("HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n<h1>404 Not Found</h1>"));
      ether.httpServerReply(bfill.position());
    }

    serviceTime();

    // Probe the gateway on a timer; its reply proves a short frame got out and one came back.
    if (linkUp && millis() - lastProbe > PROBE_MS) {
      if (!probeMiss && (int32_t)(lastProbe - lastAck) > 0) {   // the previous probe got no reply
        probeMiss = true;
        uint8_t s[DBG_LEN];
        encSnapshot(s);
        printChip(F("Gateway ARP unanswered"), s);
      }
      lastProbe = millis();
      probeGateway();
    }

    // On a timer: first prove the chip still responds (bounded), THEN it's safe
    // to read the PHY link. If it stopped responding, re-init (MCU restart after traffic).
    if (millis() - lastLinkChk > LINK_CHECK_MS) {
      lastLinkChk = millis();
      if (!enc28j60Healthy()) {
        if (!rxSeen || millis() > (MIN_UPTIME_MS << backoff())) {
          Serial.println(F("NIC stopped responding"));
          ethReady = bringUpEthernet();      // probe-protected; may drop to OFFLINE
          linkUp = false;
        }
      } else {
        bool up = ether.isLinkUp();          // safe now: chip just proved responsive
        if (up != linkUp) { Serial.print(up ? F("Link UP at ") : F("Link DOWN at ")); Serial.println(millis() / 1000UL); }
        if (up && !linkUp) {
          rxSeen = true;                     // a linked chip may already hold frames
          armProbe();                        // link is back: restart the silence clock
          ether.setGwIp(gwip);               // re-arm the gateway MAC lookup so NTP has somewhere to go
        }
        linkUp = up;
        if (up) lastLinkUp = millis();
        if (auditOk && millis() - lastAudit > AUDIT_MS) {   // e.g. lost padding or filter bits: the link still looks fine
          lastAudit = millis();
          uint8_t s[DBG_LEN];
          encSnapshot(s);
          if (!ovfSeen && s[1] != 0xFF && ((s[1] & 0x40) || (s[2] & 0x01))) {   // ESTAT.BUFER or EIR.RXERIF since the chip reset
            ovfSeen = true;
            printChip(F("RX overflow flag"), s);
          }
          if (chipGood(s)) auditBad = 0;
          else if (++auditBad > 1 && millis() > (MIN_UPTIME_MS << backoff())) restartMcu(F("chip registers changed"), 4);
        }
      }
    }
    // Only while the link never came up: an in-place re-init after traffic desyncs EtherCard's RX pointers.
    if (ethReady && !linkUp && !rxSeen && millis() - lastLinkUp > ETH_REINIT_MS) {
      Serial.println(F("No link since boot -> re-init ENC28J60"));
      ethReady = bringUpEthernet();
      lastLinkUp = millis();
    }
    // Link lost for good after it was up: a restart is the only safe chip re-init.
    if (ethReady && !linkUp && rxSeen && millis() - lastLinkUp > (LINK_DOWN_RESTART_MS << backoff())) {
      restartMcu(F("no link"), 1);
    }
    // Link up but the gateway never answers our ARP: a short frame is not getting out or back.
    if (ethReady && linkUp && millis() - lastAck > ((ovfSeen ? SILENT_FAST_MS : SILENT_RESTART_MS) << backoff())) {
      restartMcu(F("no gateway ARP reply with link up"), 2);
    }

    // !probeMiss: the link-up grace in lastAck must not forget a streak while the gateway is still silent.
    if (nvStreak && millis() > STREAK_CLEAR_MS && !probeMiss && millis() - lastAck < 2 * PROBE_MS) nvStreak = 0;
  } else {
    // --- OFFLINE: retry bringing up the NIC periodically (probe-protected) ---
    linkUp = false;
    if (lastEthTry == 0 || millis() - lastEthTry > ETH_RETRY_MS) {
      lastEthTry = millis();
      ethReady = bringUpEthernet();
    }
  }

  // --- these always run, independent of Ethernet (the monitor's real job) ---

  serviceSensor();                  // async temperature read — never blocks

  if (millis() - lastLcd > LCD_REFRESH_MS) {
    lastLcd = millis();
    updateLcd();
  }

#if DEBUG
  // Status heartbeat over Serial (compiled out when DEBUG = 0). Integer temp
  // print, so it doesn't pull the float-to-string code back in.
  if (millis() - lastStatus > 3000) {
    lastStatus = millis();
    int c10 = (int)(celsius * 10);
    Serial.print(F("alive eth="));  Serial.print(ethReady ? F("up") : F("off"));
    Serial.print(F(" link="));      Serial.print(linkUp ? F("UP") : F("DOWN"));
    Serial.print(F(" ackage="));    Serial.print((millis() - lastAck) / 1000UL);
    Serial.print(F(" ntp="));       Serial.print(timeSynced ? F("ok") : F("no"));
    Serial.print(F(" temp="));      Serial.print(c10 / 10); Serial.write('.'); Serial.print(abs(c10 % 10));
    Serial.print(F(" free="));      Serial.println(freeRam());
  }
#endif
}
