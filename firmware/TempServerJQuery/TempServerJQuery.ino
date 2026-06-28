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

// ----------------------------- configuration -----------------------------
#define ETH_ENABLED     1           // set 0 to run as a pure thermometer (no NIC access at all)
#define USE_NTP         1           // set 0 to skip NTP/DNS entirely (LCD shows uptime; web never stalls)
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

// Time zone for the LCD clock. Poland/CET: winter (CET)=+1h, summer (CEST)=+2h.
// No automatic DST — set this for the season you deploy in.
#define UTC_OFFSET_SEC  (2L * 3600L)        // CEST (summer). Use 1*3600 for winter.
const char NTP_HOST[] PROGMEM = "pool.ntp.org";
#define NTP_SRCPORT     0x42
#define NTP_SYNC_MS     3600000UL           // resync once an hour after first success
#define NTP_RETRY_MS    20000UL             // retry this often until first success
#define NTP_MAX_DNS_FAILS 5                 // give up on NTP after this many DNS failures (stops the stalls)
#define ETH_REINIT_MS   30000UL             // re-init ENC28J60 if link is down this long
#define ETH_RETRY_MS    15000UL             // when offline, re-probe + bring up the NIC this often
#define LINK_CHECK_MS   2000UL              // how often to read PHY link status
#define WDT_MS          8000                // SleepyDog window (AVR max ~8s)
#define LCD_REFRESH_MS  500                 // repaint ~2x/s so the seconds clock never skips
#define SENSOR_RES      11                  // DS18B20 resolution bits 9..12 (11 = 0.125 C, ~375 ms)
#define SENSOR_INTERVAL_MS 1500             // start a new (async) conversion this often
#define CONV_DELAY_MS   ((750 >> (12 - SENSOR_RES)) + 40)   // wait for conversion before reading
#define NTP_EPOCH_1900  2208988800UL        // seconds between 1900 and 1970

// The footer logo is a real raster image, so it's browser-loaded from an external
// host (the Arduino can't serve binaries: 28 KB flash + a ~1 KB page buffer).
// Currently on ImgBB; swap for your own host / public-repo jsDelivr URL anytime.
#define FOOTER_LOGO_URL "https://i.ibb.co/BKN52Q7w/arduino-logo-hugodemiglio.png"  // 256x256, downscaled in CSS
#define FLASH_KB        "24"        // approx program-storage used (of 28), shown in the footer (update per build)
#define ASSET_VER       "8"         // cache-bust: bump when main.css/main.js/det.js change so browsers refetch
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
static uint32_t lastDnsTry    = 0;
static uint32_t lastLinkUp    = 0;
static uint32_t lastLcd       = 0;
#if DEBUG
static uint32_t lastStatus    = 0;
#endif
static uint32_t lastEthTry    = 0;
static uint32_t lastLinkChk   = 0;
static bool     ethReady      = false;   // true only when ether.begin() has succeeded
static bool     linkUp        = false;   // cached PHY link state (updated on a timer)
static uint8_t  dnsFails      = 0;        // consecutive NTP DNS failures (for backoff/give-up)
float celsius, fahrenheit;
static bool     haveReading   = false;   // true once we have a valid reading
static bool     converting    = false;   // an async DS18B20 conversion is in progress
static uint32_t convStart     = 0;
static uint32_t lastConvReq   = 0;

// ------------------------------- helpers ---------------------------------
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
  bool ok = (encScratch(0xAB) == 0xAB) && (encScratch(0x54) == 0x54);
  uint8_t es = encReadReg(0x1D);                          // ESTAT
  SPI.endTransaction();
  SPCR = spcr;                                            // restore EtherCard's SPI config
  SPSR = (SPSR & ~_BV(SPI2X)) | (spsr & _BV(SPI2X));
  return ok && (es != 0xFF) && (es & 0x01);              // SPI ok AND oscillator running
}

// Probe -> hardware reset -> ether.begin -> static IP. Returns true only if the
// chip is present and begin() succeeds. Safe to call from setup() or loop():
// it can't hang, because we never call ether.begin() on an unresponsive chip.
static bool bringUpEthernet() {
#if !ETH_ENABLED
  return false;                      // Ethernet disabled at compile time
#else
  hardResetEthernet();
  if (!enc28j60Responds()) {
    Serial.println(F("ENC28J60 not responding on SPI -> OFFLINE (LCD only)"));
    return false;
  }
  if (ether.begin(sizeof Ethernet::buffer, mymac, CS_PIN) == 0) {
    Serial.println(F("ether.begin() failed -> OFFLINE"));
    return false;
  }
  ether.staticSetup(myip, gwip, dnsip, mask);
  ether.printIp(F("IP: "), ether.myip);
  haveNtpServer = false;          // must re-resolve the NTP host after a (re)init
  lastLinkUp = millis();
  Serial.println(F("Ethernet UP"));
  return true;
#endif
}

// Called every loop. Only touches the network when the link is actually up,
// so the blocking dnsLookup is bounded (returns in <1 s with a link). The
// watchdog is paused only around that one blocking call.
static void serviceTime() {
#if USE_NTP
  // Resolve the NTP server only while the link is up (use the CACHED state — never
  // call the hang-prone isLinkUp() here). dnsLookup() blocks ~30 s on failure and
  // starves the whole loop (LCD + web), so we back off hard and then GIVE UP, which
  // stops the stalls entirely; the LCD just falls back to uptime.
  if (linkUp && !haveNtpServer && dnsFails < NTP_MAX_DNS_FAILS) {
    uint32_t dnsInterval = (dnsFails < 2) ? NTP_RETRY_MS : 300000UL;   // 20 s, then 5 min
    if (lastDnsTry == 0 || millis() - lastDnsTry > dnsInterval) {
      lastDnsTry = millis();
      Watchdog.disable();
      bool ok = ether.dnsLookup(NTP_HOST);
      Watchdog.enable(WDT_MS);
      if (ok) {
        ether.copyIp(ntpServerIp, ether.hisip);
        haveNtpServer = true;
        dnsFails = 0;
        lastNtpReq = 0;             // ask for time promptly
        ether.printIp(F("NTP: "), ntpServerIp);
      } else {
        dnsFails++;
        Serial.print(F("NTP DNS failed ")); Serial.print(dnsFails);
        Serial.println(dnsFails >= NTP_MAX_DNS_FAILS ? F(" - giving up (uptime mode)") : F(" - backing off"));
      }
    }
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

// Repaint the LCD from the CACHED reading, writing fixed-width fields IN PLACE
// (no lcd.clear(), so no flicker). 16x2 layout, each field a constant width:
//   row0: "TTTTT" C  "HH:MM:SS"     row1: "TTTTT" F  "DD.MM.YY"   (TTTTT = 5 cells)
static void updateLcd() {
  lcd.setCursor(0, 0);
  if (haveReading) lcdTempField(celsius, 5); else lcd.print(F(" --.-"));
  lcd.write((uint8_t)223); lcd.write('C'); lcd.write(' ');   // degree glyph + unit
  if (timeSynced) {
    lcdDigits(hour()); lcd.write(':'); lcdDigits(minute()); lcd.write(':'); lcdDigits(second());
  } else {
    unsigned long up = millis() / 1000UL;             // uptime fallback (8 cells)
    lcdDigits((int)((up / 3600UL) % 100)); lcd.write(':');
    lcdDigits((int)((up / 60UL) % 60UL));  lcd.write(':');
    lcdDigits((int)(up % 60UL));
  }

  lcd.setCursor(0, 1);
  if (haveReading) lcdTempField(fahrenheit, 5); else lcd.print(F(" --.-"));
  lcd.write((uint8_t)223); lcd.write('F'); lcd.write(' ');
  if (timeSynced) {
    lcdDigits(day()); lcd.write('.'); lcdDigits(month()); lcd.write('.'); lcdDigits(year() % 100);
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
    "$('#info').html('<img class=\"image\" src=\"" FOOTER_LOGO_URL "\"><br>Assembled by Bartłomiej Mróz<br>Uptime: '+e.format('UTC:HH:MM:ss')+' (RAM '+(2560-c.free)+'/2560 B, flash ~" FLASH_KB "/28 KB)')});setTimeout(reload,14974)}"));
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
  buf.emit_p(PSTR("],\"uptime\":$L,\"free\":$D,\"res\":$D,\"pin\":$D,\"ntp\":$D,\"now\":$L}"),
             millis(), freeRam(), SENSOR_RES, ONE_WIRE_BUS, timeSynced ? 1 : 0, (long)now());
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

  // Watchdog as an early backstop. 8 s is well above Caterina's startup time, so
  // a hang recovers via reset+retry instead of freezing/trapping the bootloader.
  Watchdog.enable(WDT_MS);

  updateLcd();                      // real temperature on screen

  // Bring up the NIC. SPI + CLKRDY are probed first, so ether.begin() only runs
  // on a genuinely-ready chip; a missing chip or a dead oscillator drops to
  // OFFLINE and the thermometer keeps running.
  ethReady = bringUpEthernet();
}

// --------------------------------- loop ----------------------------------
void loop() {
  Watchdog.reset();

  if (ethReady) {
    // --- network is up: service the stack, web, NTP, link health ---
    word len = ether.packetReceive();
    word pos = ether.packetLoop(len);

    // NTP answer (UDP) arrives outside the TCP path; check the raw buffer.
    uint32_t ntp = 0;
    if (len && ether.ntpProcessAnswer(&ntp, NTP_SRCPORT)) {
      setTime((time_t)(ntp - NTP_EPOCH_1900 + UTC_OFFSET_SEC));
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

    // On a timer: first prove the chip still responds (bounded), THEN it's safe
    // to read the PHY link. If it stopped responding, drop offline and re-init.
    if (millis() - lastLinkChk > LINK_CHECK_MS) {
      lastLinkChk = millis();
      if (!enc28j60Healthy()) {
        Serial.println(F("NIC stopped responding -> re-init"));
        ethReady = bringUpEthernet();      // probe-protected; may drop to OFFLINE
        linkUp = false;
      } else {
        linkUp = ether.isLinkUp();          // safe now: chip just proved responsive
        if (linkUp) lastLinkUp = millis();
      }
    }
    // Link down too long (cable out / dead port) -> re-init the chip.
    if (ethReady && !linkUp && millis() - lastLinkUp > ETH_REINIT_MS) {
      Serial.println(F("Link down too long -> re-init ENC28J60"));
      ethReady = bringUpEthernet();
      lastLinkUp = millis();
    }
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
    Serial.print(F(" ntp="));       Serial.print(timeSynced ? F("ok") : F("no"));
    Serial.print(F(" temp="));      Serial.print(c10 / 10); Serial.write('.'); Serial.print(abs(c10 % 10));
    Serial.print(F(" free="));      Serial.println(freeRam());
  }
#endif
}
