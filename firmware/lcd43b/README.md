<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Stephen de la Vega -->
# Ventanal LCD 4.3B port

Experimental port on the separate `port-lcd43b` branch for the Waveshare
[ESP32-S3-Touch-LCD-4.3B](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3B).
The board has an ESP32-S3 (16 MB flash, 8 MB PSRAM), an 800 × 480 RGB565 LCD,
GT911 capacitive touch, and a CH422G expander controlling reset and backlight.

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

While the display is on, the board checks the manifest every 10 minutes. It
verifies the SHA-256 of each changed frame's zlib payload and decodes it into
PSRAM for immediate display. It saves the original payload and its cache ID
while the backlight is off. If the screen stays lit for six hours, it blanks
briefly to save. Saves wait for Settings to close.

On restart or power loss, the board loads the most recently saved frame in
each slot. Recently downloaded pictures may revert to an earlier frame until
the next refresh. The board can also load raw frames saved by older firmware.
The available screens follow the server's publication, including solar
availability and spares. A failed host refresh adds an OFFLINE mark.

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
Keep the header outside the repository. [config.example.h](config.example.h)
documents the installation fields. An unprovisioned generic build displays
**Not set up**.

The LCD project uses PlatformIO Core 6.2.0 and pins
[pioarduino 55.03.312-1](https://github.com/pioarduino/platform-espressif32/releases/tag/55.03.312-1)
(Arduino-ESP32 3.3.12, ESP-IDF 5.5.5). On the first build, pioarduino's hybrid
Arduino build compiles the ESP-IDF libraries with PSRAM XIP and 64-byte data
cache lines.

Install the compatible pioarduino Core in the existing PlatformIO venv before
the first build. This platform's version check rejects Core `6.2.0-post.1`.

```sh
~/.platformio/penv/bin/python -m pip install --no-deps 'pioarduino==6.2.0'
```

For a configured build, create the ignored file `firmware/lcd43b/platformio.local.ini`:

```ini
[lcd43b]
private_flags =
    '-DVENTANAL_LCD_CONFIG="/absolute/path/to/private_config.h"'
```

For a secure override, add
`'-DVENTANAL_LCD_SECURITY_CONFIG="/absolute/path/to/pairing_override.h"'`
under `private_flags`. The same options apply to all environments.
See the firmware guide for [LCD pairing](../README.md#waveshare-lcd-pairing).
The first wired private build stores these installation values in NVS. Later
generic builds reuse them. For a certificate or settings change, add
`-DVENTANAL_FORCE_PROVISION=1` to the private flags and use a distinct nonzero
revision for each subsequent change.

From the repository root:

```sh
~/.platformio/penv/bin/pio run -d firmware/lcd43b -e normal -e diag
~/.platformio/penv/bin/pio run -d firmware/lcd43b -e normal -t upload --upload-port /dev/cu.usbmodem101
~/.platformio/penv/bin/pio device monitor -d firmware/lcd43b --port /dev/cu.usbmodem101
```

Select the board's actual serial port. The board uses native USB Serial/JTAG.
After light sleep, replug USB to restore the port. If the uploader cannot enter
download mode, hold BOOT while plugging in.

`normal` requires the owner release public key. Use `-e bench` for a
local build with the public fixture key. It prints **TEST RELEASE KEY** at boot
and reports that status to the host. The diagnostic environments also permit
the fixture key.

`diag` starts a three-minute automatic screen
cycle at boot, logs bounce and VSYNC statistics, and exercises shorter refresh
and cache-save intervals. Use `-e diag -t upload` for that test and then restore
`normal`. To add another existing build flag, put it under `private_flags` in
the local file.

Each build writes `firmware.bin` and `partitions.bin` in
`firmware/lcd43b/.pio/build/<environment>/`. The [partition table](partitions.csv)
defines two **4 MiB OTA slots**, at `0x10000` and `0x410000`, and LittleFS at
`0x810000`. For the initial wired migration, the uploader writes the new
bootloader, partition table, OTA metadata and first app as individual images.
Use the upload command above to preserve NVS at `0x9000`. The combined
`firmware.factory.bin` spans that region. The board rebuilds its frame cache
on first boot.

The project selects QIO flash, 16 MB flash capacity and 80 MHz octal PSRAM. The boot log reports PSRAM size, XIP, data cache line size and VSYNC
cadence. Keep configured binaries and build output private.

### Firmware updates

Use **Settings → Firmware updates** to install an approved host offer or to
check GitHub directly. See the [update guide](../../docs/updates.md) for the
owner signing and approval workflow.

For local direct-update testing, a `diag` build can set
`VENTANAL_LCD_OTA_BASE_URL` to a quoted HTTP test origin with no trailing
slash. The mirror serves `/repos/sdelavega/ventanal/releases/latest` with
GitHub's release listing fields and serves the assets under
`/sdelavega/ventanal/releases/download/vX.Y.Z/`. The installer still requires
the normal signed release metadata. Normal builds use GitHub over HTTPS with
the ESP-IDF certificate bundle.

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

### LCD bench builds and serial diagnostics

Normal firmware requires a paired HTTPS installation using the provisioned
host leaf certificate. The local `bench` and `diag` environments use the
public fixture release key and can also run an unpaired HTTP installation.

Set `lcd43b.version` in a local PlatformIO override before building each
candidate. The ESP app descriptor identifies bench and diagnostic binaries
with `-bench` and `-diag` suffixes. Use a newer product version for each OTA
offer. Before handing a board back after a diagnostic run, replace the
diagnostic apps in both OTA slots with regular builds.

Build with `pio run -d firmware/lcd43b -e diag-dim` and flash the result to
a provisioned bench board with Art enabled. At startup the diagnostic
checks all 65,536 RGB565 values at 25%, 50%, and 75% against the scalar
reference (196,608 comparisons). After `diag dim=start RGB565 exhaustive=pass`,
it runs 20 dimmed Settings/Art cycles and forces refreshes. The
`diag dim=done` line reports peak refill time and scan health. Brightness
changes in this diagnostic are RAM-only.

Capture native USB serial on macOS with
`python3 firmware/tools/capture_serial.py --port /dev/cu.usbmodem…`.
This path opens the device without changing DTR or RTS. A conventional
serial monitor can reset the ESP32-S3 at open. Close captures before
starting a USB flash.

The coredump table takes 64 KiB from the end of LittleFS. An OTA update
retains the installed table. After a USB partition-table update, the
firmware rebuilds the frame cache for the smaller filesystem on the next
boot. The partition offsets for NVS and both OTA slots are unchanged.
