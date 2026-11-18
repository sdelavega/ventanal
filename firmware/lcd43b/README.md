<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Stephen de la Vega -->
# Ventanal LCD 4.3B port

Experimental port on the separate `port-lcd43b` branch for the Waveshare
[ESP32-S3-Touch-LCD-4.3B](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3B).
The board has an ESP32-S3 (16 MB flash, 8 MB PSRAM), an 800 × 480 RGB565 LCD,
GT911 capacitive touch, and a CH422G expander controlling reset and backlight.

The owner has checked the full-screen weather and solar pages, top-bar clock,
art corner clock, Settings and persistence, gestures, and smooth text on hardware
and reported “All good”. This guide describes that completed port.

## Screens and touch controls

Art and spare artworks fill the display. Weather and optional ham solar use
native 800 × 480 LCARS pages. A live time/date readout appears in the top bar on
weather, solar and Settings, and in the top-right corner on art and spares.

| Gesture | Action |
| --- | --- |
| Tap the right half | Next enabled screen. |
| Tap the left half | Previous enabled screen. |
| Double tap | Cycle published spare artworks, when Art is enabled. |
| Hold for about 1.5 seconds | Refresh online and redraw the current screen. Holding the second press of a double tap also refreshes. |
| Tap the clock | Open Settings. This takes precedence over left/right navigation. |

A manual refresh immediately shows **REFRESHING...**. If nothing changed,
**UP TO DATE** appears for about two seconds; a failed host fetch shows
**OFFLINE**, and a failed required frame download shows **DOWNLOAD FAILED**.
When content changes successfully, the new content replaces the refresh label.
Touch remains responsive during network activity.

While the display is on, the board checks the manifest every 10 minutes and
downloads changed frames with SHA-256 verification. Cached screens and the
selection survive restarts and work offline; a failed refresh adds an OFFLINE
mark. The available screen list follows the server's publication, including
solar availability and spares.

## Settings

Tap controls to change them; accepted changes are saved immediately in NVS and
survive restart. Tap **DONE** to return, or wait 30 seconds without touching.

- **Brightness:** 25%, 50%, 75% or 100% (default). This dims the picture by
  scaling pixel colours. The physical backlight is on/off only, so this is not
  backlight PWM or a proportional power-saving control.
- **Screen off after:** 1, 5 or 15 minutes of inactivity, or **Never** (default).
  This applies on USB and battery alike, including time spent in Settings.
  The screen turns off and the board enters light sleep. Touch wakes it and
  restores the cached screen at the saved brightness; that wake touch is consumed.
  While dark, the board wakes every 30 minutes to refresh, then sleeps again
  without lighting the display.
- **Show:** toggle Art, Weather and, when published, Solar. Spares follow Art.
  At least one available screen must stay enabled. Hidden screens are not
  downloaded; turning them back on allows downloads on the next refresh.

The server's quiet hours also turn the backlight off. Touch overrides quiet
hours for 60 seconds. The quiet window is saved for offline restarts; until the
board has trustworthy time, the backlight stays on.

## Clock and power facts

The clock synchronizes by NTP and sets the onboard **PCF85063 RTC**. A valid RTC
can supply time at boot before the network is available; an unset or invalid
clock stays hidden until synchronized. The RTC needs a **CR927 coin cell** to
keep time across loss of main power. The RTC backup cell is separate from the
board's main battery supply. Configure the board's POSIX timezone privately;
the manifest supplies the 12/24-hour format, saved in NVS for offline restarts
(default 24-hour until first received). The visible clock updates each minute.

This board has **no battery-voltage sense and no USB-source detection**. It
cannot show battery percentage, charging status or voltage-based warnings, and
there is no battery calibration control. The timeout therefore applies to both
power sources.

Light sleep drops the native USB Serial/JTAG link, which may remain unavailable
after touch wake until USB is unplugged and replugged. This only matters for
flashing and serial logs; touch wake and normal display operation still work.
Use **Never** when keeping a serial logging session available is important.

## Server configuration

The LCD profile is opt-in. Add this to the render host's private configuration:

```toml
[devices]
lcd43b = true
# id = "lcd43b-1"  # must match the board's VENTANAL_DEVICE_ID
```

Each `ventanal tick` also publishes native **800 × 480** RGB565 pages and the
per-device manifest under `<output_dir>/devices/<id>/`. The board fetches
`/devices/<id>/manifest.json` and `/devices/<id>/frames/*.vnr`. Weather and solar
are rendered for the LCD dimensions; e-paper outputs are unchanged. Solar is
still gated by the server's existing amateur licence check and is published only
when that check and the configured solar schedule allow it.

## Private configuration and building

Never commit network credentials, host addresses, location or licence details.
The `VENTANAL_LCD_CONFIG` build flag is the only private-configuration path;
there is no local `config.h` fallback. Keep the header outside the repository.
[config.example.h](config.example.h) documents the keys; `VENTANAL_DEVICE_ID` and
`VENTANAL_TZ` have defaults when omitted.

From the repository root, with the ESP32 Arduino core and required libraries
installed:

```sh
arduino-cli compile --fqbn 'esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,FlashMode=qio,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=default' \
  --build-property 'compiler.cpp.extra_flags=-DVENTANAL_LCD_CONFIG="/path/to/private_config.h"' \
  --build-path /tmp/lcd43b firmware/lcd43b
```

The sketch-local [partitions.csv](partitions.csv) overrides the default layout.
The build above emits `lcd43b.ino.partitions.bin` with a **4 MiB factory app**
(`factory`, offset `0x10000`, size `0x400000`) and **11.9 MiB LittleFS**
(`littlefs`, offset `0x410000`, size `0xBF0000`); that is the partition table
used when flashing this build. Decoding the generated binary on 2026-11-17,
using the public example configuration, confirmed these entries. The app binary
was 1,197,456 bytes: about **28.6% of the real app slot**, leaving **2.86 MiB**
of headroom. Exact usage varies with configuration and code changes.

arduino-cli still reports roughly **91% of 1,310,720 bytes (1.25 MiB)** because
its size-report yardstick comes from `default.csv` for `PartitionScheme=default`.
That report does not describe the app capacity of the emitted partition table.

Logs use native USB Serial/JTAG, with `USBMode=hwcdc,CDCOnBoot=cdc` required.
Select the board's actual serial port; USB CDC ignores the baud rate. Replug USB
after light sleep before flashing or collecting logs. Production builds leave
`VENTANAL_LCD_DIAG_*` flags disabled. HTTPS pairing and signed-manifest support
remain future work for this experimental port.

## Anti-aliased fonts and rendering

LCD text uses 4-bit glyph coverage blended into RGB565 pixels, then picture
brightness is applied when blitting. The bundled SIL OFL Antonio and Inter TTFs
supply the eight used font faces/sizes (49,615 bitmap bytes): Antonio Bold at
13/19/28 px, Antonio Regular at 13 px, Inter Bold at 20 px, and Inter Medium at
11/17/21 px. LCD server curves use 4× masks reduced with LANCZOS; e-paper
rendering is unchanged.

Regenerate the checked-in font tables from the repository root:

```sh
server/.venv/bin/python tools/aa_fonts.py
server/.venv/bin/python tools/aa_fonts.py --check
```

The generator writes `firmware/lcd43b/readout_fonts.h`.
`firmware/lcd43b/tools/readout_fonts.py` delegates to it. To regenerate the
Settings chrome spans and independent full-frame RGB565 regression fixture:

```sh
server/.venv/bin/python firmware/lcd43b/tools/lcars_geometry.py
```

The panel uses the direct `esp_lcd` RGB driver with PSRAM frame and bounce
buffers. Full-frame blits use exclusive end coordinates `(0, 0, 800, 480)`.
Hardware pin and circuit references are in the
[Waveshare schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3B/ESP32-S3-Touch-LCD-4.3B-Sch.pdf);
[board.h](board.h) records the port's capability contract.

## Host tests

Run from the repository root:

```sh
make -C firmware/test test
(cd server && .venv/bin/python -m pytest)
```

Firmware tests cover touch/gesture handling, frame verification and bounded
downloads, persistence, clock, Settings, power/sleep policy, panel blits and
pixel-level overlays. They also check font and Settings geometry generation for
stale output. Server tests cover rendering and publication, including the LCD
profile and licence gate. These complement the owner's hardware acceptance.
