# Resilience

How the firmware survives a flaky ENC28J60 and recovers without a person. The summary is in the
[README](../README.md#how-it-works).

## What it does

- Hardware reset of the ENC28J60 (`RST` to D9) before every bring-up.
- A bounded SPI plus CLKRDY probe before `ether.begin()`. `begin()` spins forever on
  an unresponsive chip, so it is never called unless the chip answers and its
  oscillator is running. A missing or dead NIC drops to OFFLINE: the LCD and sensor
  keep working and the firmware retries the NIC every 15 s.
- The NIC is never re-initialised in place once it may have received a frame.
  EtherCard keeps its receive pointers in function-local statics that
  `ether.begin()` cannot reset, so an in-place re-init leaves the receive path out of
  step with the chip: the board still transmits but never answers ARP or ping. A lost
  link is simply waited out (the receive ring is untouched), and if the chip does not
  resume when the link returns, the ladder below restarts the MCU.
- Recovery ladder. With the link up the board sends an ARP request to the gateway
  every 30 s, and only the gateway's reply counts as proof of life: it needs a short
  frame to leave the board and a reply to come back, whereas any received frame would
  hide a chip that receives but cannot send. The MCU restarts with a watchdog reset
  (about 5 s, which also clears the EtherCard pointers) after 5 min without a reply (2 min once the chip has flagged a receive
  overflow),
  3 min with the link down after it was up, a NIC fault after traffic, or the chip's
  key registers (ECON1.RXEN, MACON1, MACON3, ERXFCON, the MAC address) changing on two
  checks 10 s apart. Consecutive restarts stretch the windows up to 4 times, and 30
  min of healthy uptime forgets them. The clock survives a restart. `gwip` must
  answer ARP, otherwise a bench board with no gateway restarts at 5, 10 and then every 20 min.
- Evidence for the next failure. Before a self-restart the chip's registers are read
  (12 bytes: ECON1 ESTAT EIR MACON1 MACON3 ERXFCON, a mask of wrong MAC bytes,
  EPKTCNT, then the receive buffer pointers ERXRDPT and ERXWRPT, low byte first),
  printed on Serial, kept in RAM across the restart and served as `why` and `dbg` in
  `/list.json`. The read restores ECON1.BSEL, so EtherCard's cached register bank
  stays valid. At every boot Serial shows `Chip audit OK` when the registers match
  what EtherCard configured, or `Chip audit BAD` with the bytes. The chip's own reset
  clears its sticky error flags (EIR.RXERIF, EIR.TXERIF, ESTAT.BUFER), so a flag seen
  later means an overflow or error since the last bring-up. Serial also prints the
  link changes with the uptime, the snapshot when the first gateway probe goes
  unanswered, a one-time "RX overflow flag" line, and a snapshot whenever you send `d`
  over USB.
- Receive buffer pointer. ENC28J60 errata item 14 says an even ERXRDPT can corrupt
  the receive buffer, including the next packet pointer of a frame. EtherCard's
  `initialize()` leaves ERXRDPT at 0, and its own steady state writes only odd values
  (next packet pointer minus 1, or ERXND). After every successful `ether.begin()` the
  sketch therefore writes ERXRDPT = ERXND (0x0BFF), the value EtherCard itself writes
  when the next packet is at 0, so the first frames after a bring-up no longer arrive
  with an even pointer. Whether this was behind the 09-25 and 09-26 failures is
  unproven.
- LCD status cells, both blank when all is well: row 0 column 8 shows recent
  self-restarts (`+` above 9) and clears itself after 30 min of healthy uptime, row 1
  column 8 is `O` for NIC offline, `L` for no link and `S` for link up but no
  gateway ARP reply for 90 s. The lifetime restart count is `rst` in `/list.json`.
- The DS18B20 can be plugged in at any time. DallasTemperature only counts devices in
  `begin()`, so a probe attached after boot used to stay invisible; while none is
  known the sketch checks the bus every 3 s and re-runs `begin()` when one appears.
- SleepyDog watchdog (8 s), safe on the 32u4 bootloader.
- Temperature and LCD are fully decoupled from the network, so the monitor always
  works.

## Field failure, 2026-09-25

Cause not yet proven: LCD, clock and link LEDs
fine, but the board answered ping (with a static ARP entry) and never answered ARP
or completed a TCP handshake, from wired and Wi-Fi clients alike. Every frame the
firmware sends with an explicit short length (the 42 byte ARP reply, the SYN-ACK)
was lost, while echo replies, which copy the incoming length, were fine. The
suspect is the chip no longer padding short frames (MACON3), so the router drops
them; a power cycle fixed the same symptom the day before. The gateway ARP round
trip now detects it and the register snapshot should show which register changed.

## Second occurrence, 2026-09-26, and the first recovery in the field

After a reflash the board booted with no Ethernet cable, and the cable was plugged in
about a minute later. For the next five minutes the board sent an ARP request for the
gateway every 3 s or so (visible on the LAN) but answered nothing, so there was no ping,
no web page and no NTP. The LCD showed `S` after 90 s. At about 6 minutes of uptime it
restarted itself (`why` 2, no gateway ARP reply with the link up) and was back with NTP
and the web page about 90 s later, with `rst` 1 and no human involved.

The saved chip snapshot was `[5, 65, 9, 1, 50, 177, 0, 0]`. Every register the audit
checks was exactly as EtherCard configured it (ECON1 0x05 with RXEN set, MACON1 0x01,
MACON3 0x32, ERXFCON 0xB1, MAC bytes correct), so the register audit could not see this
fault and the gateway ARP ladder did. Two sticky flags stand out: ESTAT.BUFER (0x40)
and EIR.RXERIF (0x01) were set, meaning the chip's receive buffer overflowed at some
point since power-up (EtherCard never clears them), while EIR.PKTIF was clear (no packet
pending at that instant). That fits a receive path that got out of step, but it does not
prove it: the flags could also come from a burst of traffic during boot. It also weakens
the padding theory from 09-25, because MACON3 was correct this time.

## Reading the snapshot

The 12 bytes are ECON1, ESTAT, EIR, MACON1, MACON3, ERXFCON, a mask of wrong MAC
bytes, EPKTCNT, ERXRDPT (low, high) and ERXWRPT (low, high). A healthy idle chip looks
like `05 01 08 01 32 B1 00 00 xx yy xx+1 yy` with EPKTCNT 0 and ERXWRPT one ahead of
ERXRDPT (EtherCard writes ERXRDPT as the next packet pointer minus 1). A gap of about
one frame right after traffic is only EtherCard releasing a frame lazily. The suspicious
signature is EIR.RXERIF and ESTAT.BUFER set with EPKTCNT 0, and ERXWRPT just below
ERXRDPT or garbage. The pointer bytes are only reliable when the link is quiet: ERXWRPT
updates when a frame completes and the two bytes are read a fraction of a millisecond
apart.

## Chip state corruption, 2026-09-26 (hardware suspected)

On the afternoon of 09-26 the chip's registers were repeatedly scrambled while the MCU
kept running. Snapshots showed ESTAT.CLKRDY clear, MACON1, MACON3 and all six MAC bytes
wrong, nonsense buffer pointers, or every register reading 0. It happened on USB power
from a Mac (whose connector was loose) and on a wall adapter, 50 to 130 s after each
restart, six self-restarts between 12:53 and 13:11. Then it stopped for more than 45
minutes with no change made. The firmware caught it every time (`enc28j60Healthy()`
notices RXEN or CLKRDY lost) and restarted, but because the restart windows double, each
recovery took one to four minutes.

Suspected, not confirmed: the module is powered from the Leonardo's 3.3 V pin, whose
documented limit is about 50 mA, while an ENC28J60 module draws roughly 120 to 200 mA
once linked. A regulator that small can overheat or sag, especially inside a closed box.
An intermittent SPI or power connection is the other candidate. Next steps: give the
module its own 3.3 V regulator, check the SPI wires, and audit ERXST, ERXND and ERXRDPT
too, so a scrambled chip is caught faster.

## EtherCard cannot be re-initialised in place

EtherCard 1.1.0 cannot be re-initialised in place after it has received a frame
(`packetReceive()` keeps private static pointers that `ether.begin()` does not
reset). Do not call `ether.begin()` again from your own code once traffic has
flowed; the firmware restarts the MCU instead. Confirmed in the source and a
simulation, not yet on hardware.
