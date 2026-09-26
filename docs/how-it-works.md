# How it works

The detail behind the summary in the [README](../README.md#how-it-works).

## Web UI and API

The Arduino serves only a small HTML shell plus `main.css`, `main.js`, `det.js` and
`/list.json`. The heavy assets (jQuery 1.8.2, jQuery-Mobile 1.2.0 js+css,
`date.format.js`) are loaded by the browser from `code.jquery.com` over HTTPS; at
about 284 KB they would never fit in 28 KB of flash. This is the original 2015
architecture, preserved.

- `GET /` serves the HTML shell (header "Rožna Arduino Thermometer", inset
  listview, footer).
- `GET /main.css`, `GET /main.js`, `GET /det.js` are served from PROGMEM.
- `GET /list.json` returns `{"list":[{"id","name","val","ifnegative"}],"uptime","free","flash","res","pin","ntp","now","rst","why","dbg"}`,
  where `val` is degrees C times 100 (the JS divides by 100), `free` is free RAM and
  `flash` the program size in bytes (read from the linker, so it matches the IDE's
  "Sketch uses N bytes" and is never stale), `now` is local time,
  `rst` counts self-restarts since power-up, `why` is the reason code of the last
  self-restart (0 none, 1 no link, 2 no gateway ARP reply, 3 NIC re-init after
  traffic, 4 chip registers changed) and `dbg` is the chip snapshot taken just
  before it (see [resilience.md](resilience.md)). The footer shows the
  "Assembled by Bartłomiej Mróz" credit, uptime, and memory use (RAM used / total in
  bytes, flash used / total in KB, both read from the running board).
- Tapping a sensor row slides open an inline detail panel (and rotates the row's
  arrow 90 degrees): ROM id, C and F, resolution, data pin, the NTP clock (same as
  the LCD), uptime, RAM use and NTP status. Tap again to collapse.

**Images.** The browser-tab icon is a self-contained thermometer emoji served as a
tiny SVG from its own `/favicon.svg` route. The footer logo (`ardu.png`) is a raster
image, browser-loaded from an external host (`FOOTER_LOGO_URL`, currently ImgBB);
the Arduino never serves image bytes. Static assets are cache-busted with a `?v=`
version (`ASSET_VER`), so changes reach the browser without a manual cache clear.

## NTP clock

Replaces the original serial time-sync, which froze the board at boot waiting for a
PC. `serviceTime()` sends an NTP request to a fixed server IP (Google Public NTP,
`NTP_SERVER_IP`) rather than a hostname: `ether.dnsLookup()` can block for about 30 s
when a query gets no reply, and that froze the whole loop, LCD included. The answer
sets the Time library clock, which holds UTC. `localNow()` adds the Polish offset
(CET, plus EU summer time from 01:00 UTC on the last Sunday of March to 01:00 UTC on
the last Sunday of October) for the LCD and the JSON `now`. It re-syncs hourly, and
until the first answer the LCD shows uptime.

## Performance

- **Non-blocking sensor.** DS18B20 conversions run asynchronously
  (`waitForConversion=false`), started on a timer and read about `CONV_DELAY_MS`
  later. Neither the LCD nor `/list.json` waits on a conversion, so `/list.json`
  answers in about 40 ms. The old code blocked roughly 750 ms, twice.
- **Browser caching.** `main.css`, `main.js`, `det.js` and `favicon.svg` are sent
  with `Cache-Control: max-age=86400`; only `/list.json` is `no-cache`.
- **LCD in place.** It repaints without `lcd.clear()` (no flicker), about twice a
  second so the seconds never skip, and temperatures are formatted with integer
  math so the float-to-string code is never linked.
