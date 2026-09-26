# What changed since the 2015 code

The 2015 architecture was sound and is largely preserved: the web design,
`/list.json`, the HTTP parser, sensor enumeration and LCD layout are essentially
unchanged. The changes fall into three buckets.

**Trivial modernisation**

- MAC changed from the textbook `DE:AD:BE:EF:FE:ED` to a locally-administered `02:..`
- Explicit `CS=8` (EtherCard's default CS drifted to `SS`, pin 17, on the Leonardo)
- `<Time.h>` to `<TimeLib.h>`; verified against current DallasTemperature, OneWire and EtherCard
- CDN tags switched from protocol-relative `//` to `https://`
- Removed the dead `s3.postimg.org` footer image; kept the credit, uptime and free-RAM footer

**One feature swap**

- NTP replaces serial time-sync, so the board no longer freezes at boot waiting for
  a connected PC and keeps real time headless.

**Reliability, the bulk of the new code and the real 2015 fix**

- Hardware reset of the ENC28J60 over `RST` to D9, the missing piece that made the
  PHY actually link (the original "never appears on the router" bug).
- An SPI plus CLKRDY probe before `ether.begin()`, so a missing, dead or
  half-connected NIC can never hang the board; it degrades to OFFLINE and retries.
- SleepyDog watchdog (safe on the 32u4).
- A fixed NTP server IP instead of a DNS lookup, because a blocking lookup froze the
  whole loop, LCD included, whenever a query got no answer.
- No in-place NIC re-init after traffic (EtherCard cannot resynchronise its receive
  pointers), and a restart ladder for a dead receive path (see [resilience.md](resilience.md)).
- Automatic EU summer time for the LCD clock.
- Temperature and LCD decoupled from Ethernet; a Serial heartbeat for observability.
- Async sensor reads, browser-cached static assets with cache-busting, integer
  formatting, 11-bit DS18B20, and the expandable per-sensor detail panel.
