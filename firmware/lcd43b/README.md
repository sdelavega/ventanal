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

## Behaviour

- Connects Wi-Fi, syncs NTP, fetches `/devices/<id>/manifest.json` every
  10 minutes (and on a long-press refresh), downloads changed frames with
  SHA-256 verification into LittleFS, and blits them to the panel.
- Touch (GT911): tap right half = next screen, tap left half = previous
  screen, double tap = next spare artwork (when spares are published),
  press >= 1.5 s = refresh now. All edges debounced 30 ms.
- Backlight off during the manifest's quiet hours, on otherwise; any touch
  lights it for 60 s during quiet hours.
- Offline: keeps showing the cached screen with a small OFFLINE mark.
- Cache state (screen ids) persists in NVS; frames cache in LittleFS.

## Private configuration

Never commit private values. Copy `config.example.h` to `config.h`, or build
with `-DVENTANAL_LCD_CONFIG="<absolute path>"` pointing at a header kept
outside the repository (e.g. under `.superpowers/`):

```sh
arduino-cli compile --fqbn 'esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,FlashMode=qio,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=default' \
  --build-property 'compiler.cpp.extra_flags=-DVENTANAL_LCD_CONFIG="/path/to/private_config.h"' \
  --build-path /tmp/lcd43b firmware/lcd43b
```

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

Covered: gesture classifier halves/double/long-press/debounce, bundled
SHA-256 against FIPS 180-4 vectors, VNR1 header/hash validation, device
manifest parsing/quiet hours, and overlay pixel clipping.

## Phase 2 (not yet)

Multi-device security (per-device keys/counters for HTTPS + signed manifests
against the owner's secured host), pairing UI, and running the profile on
the render Pi (the frame's unpaid art department).