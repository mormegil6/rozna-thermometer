# Flashing when the automatic reset fails

The normal steps are in the [README](../README.md#build-and-upload).

**If upload fails (`butterfly_recv ... failed`).** The Leonardo's 1200-baud
auto-reset can stop dropping the board into the Caterina bootloader (common after
many upload cycles, or with a watchdog sketch), and arduino-cli then flashes against
the running sketch and fails. Recovery: double-tap RESET (the "L" LED pulses, which
means the bootloader is up for about 8 s) and flash with avrdude directly, bypassing
the 1200-baud touch:

```sh
AV=~/Library/Arduino15/packages/arduino/tools/avrdude/8.0.0-arduino1
# the hex path is shown by:  arduino-cli upload -v ...   (under ~/Library/Caches/arduino/sketches/<hash>/)
"$AV/bin/avrdude" "-C$AV/etc/avrdude.conf" -patmega32u4 -cavr109 -P<PORT> -b57600 -D \
  -Uflash:w:<sketch>.ino.hex:i
```
