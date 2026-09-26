# Hardware notes

Wiring gotchas that cost time during bring-up. The pin maps are in the [README](../README.md#hardware-and-wiring).

**CS gotcha.** EtherCard's `begin()` default chip-select drifted from pin 8 (old)
to `SS`, which is pin 17 on the Leonardo (current library). The 2015 sketch relied
on the old default, so the real wiring is CS=8. The sketch sets `CS_PIN 8`
explicitly.

**RST gotcha (the actual 2015 bug).** The ENC28J60 needs a hardware reset pulse or
its PHY will not link. That was the original "never appears on the router" symptom.
Wire `RST` to D9; the firmware pulses it before every `ether.begin()`. Note that SPI
register reads are clocked by the Arduino, so the chip answers SPI even when its own
25 MHz crystal is not running. The firmware therefore also checks the
oscillator-ready bit (`CLKRDY`) before trusting it.

**Solid connections matter.** On a breadboard with dupont wires, bumping the board
can knock the ENC's SPI or power loose (this happened repeatedly during bring-up).
For a permanent install, solder or use locking connectors on the ENC's SPI, power
and RST lines.
