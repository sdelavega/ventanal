<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Stephen de la Vega -->
# Ventanal lcd43b firmware (Waveshare ESP32-S3-Touch-LCD-4.3B)

Phase-1 port of Ventanal to the Waveshare
[ESP32-S3-Touch-LCD-4.3B](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3B):
an ESP32-S3 (N16R8: 16 MB flash, 8 MB PSRAM) driving an 800x480 parallel
RGB565 LCD (ST7262-class) with GT911 capacitive touch, both behind a CH422G
I/O expander for reset and backlight. USB powered: no battery, no deep sleep.

## Hardware references

- Wiki (pin map, timings): <https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3B>
- Schematic: <https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3B/ESP32-S3-Touch-LCD-4.3B-Sch.pdf>
- CH422G datasheet: <https://files.waveshare.com/wiki/common/CH422DS1_EN.pdf>
- Board pin map as used by this port (from the wiki/schematic):
  - LCD RGB565 16-bit data: GPIO14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40
  - LCD control: HSYNC GPIO46, VSYNC GPIO3, DE GPIO5, PCLK GPIO7
  - Touch: I2C SDA GPIO8 / SCL GPIO9 (400 kHz), IRQ GPIO4, reset via CH422G EXIO1
  - CH422G (I2C addr 0x24 register selects): EXIO2 = backlight, EXIO3 = LCD reset,
    EXIO4 = TF card select
  - USB-Serial/JTAG native CDC

## Display library choice

`esp_lcd` RGB panel driver (`esp_lcd_new_rgb_panel`) configured directly, with
`ESP32_Display_Panel` v1.0.4's board file for this exact panel
(`BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_4_3_B.h`) as the reference for pins and
timings. Chosen over Arduino_GFX/ESP32_Display_Panel's full stack because the
port only needs a full-frame blit of raw RGB565 (the frames are rendered
server-side), so the direct driver is smaller, avoids LVGL/screen-drift
workarounds, and lets us keep the PSRAM frame buffer plus the recommended
`WIDTH*10` bounce buffers.

Full-frame blits pass exactly `(0, 0, 800, 480)`: ESP-IDF's
`esp_lcd_panel_draw_bitmap` end coordinates are **exclusive** and the source
is read packed at `x_end - x_start` pixels per row, so `(0, 0, 799, 479)`
would copy 799×479 pixels at a 799-pixel stride, shearing every row one
pixel further (the diagonal wrap seen in the first hardware run).
`test_lcd43b_panel` pins the arguments with a fake `esp_lcd` and checks a
vertical-line test pattern survives the blit.

## Serial logging

Logs go to the native USB-Serial/JTAG port (`/dev/cu.usbmodem101`, the same
port esptool uses), so the sketch must be built with hardware CDC on boot:

```
USBMode=hwcdc,CDCOnBoot=cdc
```

Both appear in the boot log as `usb cdc hw=1 onboot=1` so a capture proves
the running image is configured correctly. Every line is emitted by one
bounded `write()` (never a per-character trickle), a 4 KB TX ring retains
recent boot lines until a host opens the port (oldest dropped if it fills),
and `flush()` runs only while a host actually has the port open — a missing
or stalled host can never block the loop. Gesture handling always logs
`gesture=<kind> x=… y=…` and every screen change logs `screen=<name>`.
To capture a boot, open the port and reset the board
(`capture_lcd43b.py --reset`); baud rate is ignored by USB CDC.

## Diagnostics

Building with `-DVENTANAL_LCD_DIAG_GESTURES=1` (added to
`compiler.cpp.extra_flags` alongside `VENTANAL_LCD_CONFIG`) replays a host-tested
synthetic touch sequence (`diag.h`) through the production classifier a
couple of seconds after the first refresh, logging every gesture
(`next`, `double`, `previous`, `refresh`) and screen change without a finger
on the panel — the sequence and its ordering are pinned by `test_lcd43b_diag`.
Use it only for captures and hardware bring-up; production images leave the
flag off.

Building with `-DVENTANAL_LCD_DIAG_TOUCH=1` instead logs the raw GT911
view rate-limited (`touch raw=<report> status=0x.. contact=.. x=.. y=..`:
every report change plus at most one line per second while a finger is
held) alongside the usual `gesture=` lines, so a five-minute capture shows
exactly what the controller reports for real touches. The two diagnostic
flags cannot be combined (the sketch rejects that build).

## Behaviour

- Connects Wi-Fi, syncs NTP, fetches `/devices/<id>/manifest.json` every
  10 minutes (and on a long-press refresh), downloads changed frames with
  SHA-256 verification into LittleFS, and blits them to the panel.
- **A refresh runs on a short-lived FreeRTOS worker task**, so touch
  polling (every 10 ms) and the backlight keep being serviced while
  Wi-Fi waits, NTP waits and up to six 60 s frame transfers run. The
  worker is supervised by the **task watchdog** (30 s window, fed on
  every progress step): a wedged network call resets the board instead of
  freezing refreshes. The worker only performs network and staging I/O;
  all state changes are applied by `loop()` (`sync.h`, single writer).
- **Frame downloads are bounded**: every read is capped by the frame
  bytes still missing (the PSRAM buffer can never overflow) and a
  response that keeps sending past the advertised length is rejected
  (`excess=1`) rather than accepted as a prefix (`test_lcd43b_sync`,
  under AddressSanitizer).
- Touch (GT911): tap right half = next screen, tap left half = previous
  screen, double tap = next spare artwork (when spares are published),
  press >= 1.5 s = refresh now — from **either** press: a second press
  held past the window refreshes rather than doubling. All edges are
  debounced 30 ms.
- GT911 reports arrive every ~10-15 ms, slower than the 10 ms poll:
  "no new report" keeps the previous contact state (only a valid
  zero-point report is a release), and the point data is read **before**
  the status register is acknowledged so the controller can never replace
  a report mid-read (`touch.h`, `test_lcd43b_touch`).
- Backlight off during the manifest's quiet hours, on otherwise; any
  touch lights it for 60 s during quiet hours. The validated quiet window
  **persists in NVS**, so an offline reboot during quiet hours still
  honours it; before the clock is trustworthy (first NTP sync) the
  backlight stays on — defined, safe behaviour for a USB-powered board.
- Offline: keeps showing the cached screen with a small OFFLINE mark
  (drawn/erased whenever the offline state changes, including after a
  failed scheduled refresh).
- Cache state (screen ids, selection, quiet window, publish shape)
  persists in NVS; frames cache in LittleFS. A shrunk screen list (solar
  dropping out overnight) normalises the persisted selection to art and
  the spare cursor to spare0.

## Private configuration

Never commit private values. The `VENTANAL_LCD_CONFIG` build flag is the
**only** private-configuration path — the sketch has no local `config.h`
fallback. Keep the private header outside the repository (e.g. under
`.superpowers/`, which is git-ignored):

```sh
arduino-cli compile --fqbn 'esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,FlashMode=qio,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=default' \
  --build-property 'compiler.cpp.extra_flags=-DVENTANAL_LCD_CONFIG="/path/to/private_config.h"' \
  --build-path /tmp/lcd43b firmware/lcd43b
```

`config.example.h` documents the keys; `VENTANAL_DEVICE_ID` and
`VENTANAL_TZ` are optional and fall back to defaults when the private
header omits them.

Upload to `/dev/cu.usbmodem101` with esptool (via `arduino-cli upload`) and
capture serial at 115200 baud. The sketch partition table
(`partitions.csv`: 4 MiB factory app, 11.9 MiB LittleFS) applies via
`PartitionScheme=default` (the sketch-local CSV overrides).

## Server side

Enable the profile on the render host with:

```toml
[devices]
lcd43b = true
# id = "lcd43b-1"   # must match the board's VENTANAL_DEVICE_ID
```

Each `ventanal tick` then also publishes the 800x480 RGB565 frames and the
per-device manifest under `<output_dir>/devices/<id>/`, served read-only at
`/devices/<id>/manifest.json` and `/devices/<id>/frames/*.vnr`. E-paper
outputs are unchanged.

## Host tests

```sh
make -C firmware/test test   # includes test_lcd43b_* units
```

Covered: gesture classifier halves/double/long-press (either press)/
debounce, the GT911 register protocol and 10-15 ms report cadence through
the production driver (`test_lcd43b_touch`), the bounded downloader with
excess rejection and stall budgets under the task-watchdog contract plus
AddressSanitizer (`test_lcd43b_sync`), persisted quiet hours/publish
shape/cache ids/selection including an offline boot during quiet hours
(`test_lcd43b_persist`), bundled SHA-256 against FIPS 180-4 vectors, VNR1
header/hash validation, device manifest parsing/quiet hours/pre-sync
behaviour, overlay pixel clipping, the panel blit arguments against a
fake `esp_lcd`, and the diagnostic gesture sequence.

## Phase 2 (not yet)

Multi-device security (per-device keys/counters for HTTPS + signed manifests
against the owner's secured host), pairing UI, and running the profile on
the render Pi (the frame's unpaid art department).