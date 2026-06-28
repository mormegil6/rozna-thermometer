# Original 2015 sketches (historical reference)

These are the untouched sketches from the 2015 build, kept to show how the project
evolved. They are not maintained and will not necessarily compile against current
libraries. For the working firmware see [`../firmware/TempServerJQuery/`](../firmware/TempServerJQuery/).

In date order (the commit dates match these):

| Date | File | What it was |
|---|---|---|
| 2015-01-25 | `thermometr_01.ino` | The earliest sketch: an LCD-only thermometer (C / F), no networking. |
| 2015-01-26 | `Exosite_tempServ.ino` | A variant that pushed readings to the Exosite IoT cloud (since defunct). |
| 2015-01-27 | `JQueryOrig.ino` | The jQuery-Mobile web UI (header "Is it hot?"), no LCD; DHCP or static IP selectable. |
| 2015-01-27 | `temp_server.ino` | A plain HTTP page ("My Arduino Website / Hello!"). |
| 2015-02-05 | `TempServerJQuery.ino` | The most complete variant: jQuery-Mobile UI + 16x2 LCD + clock + `/list.json`. The 2026 firmware is based on this one. |
| 2015-05-04 | `TempServerJQuery_noethernet.ino` | Same as above with Ethernet commented out, for LCD and clock bench testing. |

See the main [README](../README.md), section "What changed since the 2015 code", for the details.
