<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Stephen de la Vega -->
# Ventanal firmware

The firmware caches art, weather, optional solar and Extras screens, overlays the
readout, applies battery and local-time scheduling, and sleeps between updates.
A fresh installation starts on weather; a short WAKE press cycles cached screens
without starting Wi-Fi.

## Toolchain

Install Arduino CLI and Soldered's board index (from the board repository README):

```sh
brew install arduino-cli
arduino-cli config add board_manager.additional_urls https://github.com/SolderedElectronics/Inkplate-Board-Definitions-for-Arduino-IDE/raw/refs/heads/main/package_Inkplate_Boards_index.json
# Dedicated sketchbook avoids scanning unrelated legacy hardware/libraries.
arduino-cli config set directories.user "$HOME/Library/Arduino15/ventanal-sketchbook"
arduino-cli core update-index
arduino-cli core install soldered-inkplate-boards:esp32@4.0.0
arduino-cli lib install 'InkplateLibrary@11.2.1' 'ArduinoJson@7.4.3'
server/.venv/bin/pip install pyserial esptool
arduino-cli board listall | grep -i 13SPECTRA
```

For a new installation, copy `ventanal/config.example.h` to `ventanal/config.h`
and edit it privately. Compile against your private configuration; never print it
or log credentials. The example uses
`http://192.168.0.10:8765`; the secure example is `https://192.168.0.10:8766`.

Base FQBN: `soldered-inkplate-boards:esp32:Inkplate13SPECTRA`.
Use the explicit hardware options:

```sh
FQBN='soldered-inkplate-boards:esp32:Inkplate13SPECTRA:FlashSize=32M,FlashMode=opi,PSRAM=opi,PartitionScheme=custom,DebugLevel=none'
```

**Upload with `firmware/tools/upload.py`, using Python with esptool installed.**
This core hardcodes app `0x10000` and OTA metadata `0xe000` in its standard
Arduino uploader, merged image and flash_args. Our mandatory table instead has
app `0x20000` and otadata `0xf000`. The helper validates the generated binary
partition table and image sizes, then writes bootloader at 0, table at 0x8000,
OTA metadata at 0xf000 and app at 0x20000. It never erases the filesystem.
Do not use the stock uploader, merged image or generated flash_args for this
layout. Optional compile properties
`--build-property recipe.hooks.objcopy.postobjcopy.3.pattern=/usr/bin/true`
and `--build-property recipe.hooks.objcopy.postobjcopy.4.pattern=/usr/bin/true`
skip those unused artifacts (used during this task's verification).

32 MB flash, OPI flash at 80 MHz, OPI PSRAM (16 MB fitted), UART0 serial,
CDC on boot disabled. `PartitionScheme=custom` selects the sketch's CSV;
the build's application-size limit is overridden to 4194304 below to match its
4 MiB factory slot.

The local `partitions.csv` overrides the menu partition table. NVS: 0x9000/24K;
otadata: 0xf000/8K; factory app: 0x20000/4M; LittleFS: 0x420000/0x1bd0000
(27.8125 MiB); coredump: 0x1ff0000/64K. End: 0x2000000 (32 MiB).
The gap before app0 permits 64K app alignment. No OTA app slot is provided.
LittleFS explicitly mounts the `littlefs` label, using the required `spiffs` subtype.

## Modes and hardware checks

Use build properties to select a mode without editing private configuration.
All diagnostic flags default to zero. Build each mode into its own temporary
build directory; upload the exact build just compiled. Do not commit build output.

```sh
# Normal firmware:
arduino-cli compile --fqbn "$FQBN" --build-property upload.maximum_size=4194304 --build-path /tmp/ventanal-cache firmware/ventanal
server/.venv/bin/python firmware/tools/upload.py /tmp/ventanal-cache
# Full wake cycle with 60-second diagnostic sleeps:
arduino-cli compile --fqbn "$FQBN" --build-property upload.maximum_size=4194304 --build-path /tmp/ventanal-wake --build-property 'compiler.cpp.extra_flags=-DVENTANAL_WAKETEST=1' firmware/ventanal
# WAKETEST=2 also simulates the application button path on timer wake 2.
# Serial explicitly identifies the simulation; GPIO18 still needs an owner check.
```

Serial is 115200 baud. Capture with a Python environment containing pyserial:

```sh
server/.venv/bin/pip install pyserial esptool
server/.venv/bin/python firmware/tools/capture_serial.py --reset --seconds 100
```

Opening the port may reset the board. Keep one capture open across the 60-second
sleep to see `wake=timer`; the controller/owner will test `wake=button` separately.
Serial reports battery, Wi-Fi/time/manifest/download/blit/display phases and total
wake duration, plus draw time, offline status and the selected readout rectangle.

Rotation is consumed directly from private config and never changed here.
The driver's native panel is 1200 columns × 1600 rows. For rotation 1,
`(x,y) -> (panel_col=y, panel_row=1599-x)`: TOP at logical y=0 is the **native
column-0 edge**, the native portrait left edge. For rotation 3, TOP is the native
column-1199 (portrait right) edge.  The owner must confirm the physical hanging orientation by eye.
Even rotations are rejected because the VNF1 frames require 1600×1200.

## Cache provisioning and recovery

For first bring-up, prepare the host and private board settings, then compile
and upload the normal firmware with the custom partition table and upload helper
above. Power the board with a charged battery or other stable supply. On the
first boot, capture serial at 115200 while it connects to the host and downloads
the initial weather and art frames. A blank LittleFS partition should log
`cache provisioning=mount-failed-no-state` once, then show weather. Reboot and
verify the filesystem mounts without provisioning. A short WAKE press should
cycle the cached frames with the host offline.

The firmware tries a regular mount first. If it fails, formatting is allowed
only when NVS confirms that both saved `state13` and quarantined `state-bad` are
absent. Formatting erases the whole LittleFS partition. If the first format
fails, the firmware logs `error=cache-provision` and can retry on the next wake.
When saved state exists or the NVS check fails, the firmware preserves the
partition and logs `error=cache-mount`. It shows “Waiting for host” if the
selected frame cannot be blitted. On a mounted filesystem, the explicit debug
flag `-DVENTANAL_ERASE_CACHE=1` removes the committed art, weather and solar frames
and the art/weather `.tmp` files. Always return to a build without debug flags
afterwards.

Every cold/timer wake syncs the tiny manifest and downloads only changed or missing
frames. IDs and their readout rectangles advance in NVS after each successful
frame commit. Button wakes use cache and RTC without connecting. The selected
screen refreshes at most once, after all downloads, only when `needsRedraw` permits
(or when a missing/bad selected cache needs the waiting screen). There is no
boot-cache refresh before networking. Last updated is the local time of the draw;
OFFLINE means this wake's manifest request failed. An unchanged timer wake keeps
the panel, including its previous timestamp and status, untouched.

Manifest version, IDs, size,
relative frame URLs and readout bounds are checked. Both art and weather download
through a 64 KiB internal-heap buffer using `readBytes` and large LittleFS writes
(512-byte stack validation buffer) to `<path>.tmp`; only an exact 960016-byte VNF1
frame with valid header and all colour indices 0..5 can replace a committed file.
Close and validate before LittleFS atomic rename; never unlink the old destination.
A failed download, buffer allocation or rename leaves the previous frame intact.
Wi-Fi power saving is disabled during each frame request and its exact previous
mode is restored on every exit, including failure and diagnostic abort. All HTTP
requests share the User-Agent `Ventanal/1.0.0`.
Internal RAM avoids the flash driver's small PSRAM bounce copies, and stays off
the 8 KiB task stack. Each frame log includes elapsed `ms` from allocation/request through validation
and commit, plus read/write/check phase timings; the combined sketch download
timer excludes the separately reported manifest request. Art and weather
commit independently, so losing one download does not lose the other cache.

To simulate interruption after a good cache exists:

```sh
arduino-cli compile --fqbn "$FQBN" --build-property upload.maximum_size=4194304 --build-path /tmp/ventanal-abort --build-property 'compiler.cpp.extra_flags=-DVENTANAL_ABORT_AFTER_BYTES=100000' firmware/ventanal
```

Upload that build with a changed/missing frame to exercise its download branch.
It aborts at exactly 100000 bytes, leaving the partial `.tmp`. A failed transfer
preserves the committed cache. After checking recovery, upload the normal build
with all flags disabled.

State is in the `ventanal` NVS namespace (`state13` blob): active screen, cached
IDs/rectangles and `Drawn`. Battery smoothing history persists across deep sleep
in RTC memory. At critical voltage the board commits the `charge-drawn` marker
to NVS before attempting the “Charge me” refresh and arms only GPIO18. A
brownout during refresh can leave a partial screen. The persisted marker
suppresses a second draw while the battery is still critical, and an abnormal
reboot with the marker goes straight to button-only sleep. Wake the board after
charging. A successful replacement refresh clears the marker for the next
low-battery episode. Normal sleep follows quiet hours and the low-battery interval.

RTC is initialized by `display.begin()`. A set PCF85063A seeds system time even
offline. Online `configTzTime` waits up to 15 seconds for NTP completion and writes
the RTC. RTC stores UTC; the library's local-time epoch helpers are called with
TZ temporarily set to UTC, then the configured POSIX TZ is restored.

## Host checks and cleanup

```sh
make -C firmware/test test
make -C firmware/test clean
rm -rf /tmp/ventanal-cache /tmp/ventanal-pattern /tmp/ventanal-wake /tmp/ventanal-abort
```

The manifest host test also uses the installed ArduinoJson 7 headers; override
`ARDUINOJSON=/path/to/ArduinoJson/src` in make if installed elsewhere.

Host cache builds reject stack frames over 2048 bytes. The firmware reports
minimum free loop-task stack before sleeping; buffer sizes leave room for nested
filesystem/network calls on the default 8 KiB stack.

Host tests exercise production cache.cpp with in-memory filesystem, HTTP and
display doubles: exact frame decode, interruption, invalid header/colour/length,
HTTP failure, rename/write/allocation failure, large block transfers, shared
User-Agent, Wi-Fi power-save restoration, pre-blit validation, and the real 100000-byte debug
abort branch. They do not establish hardware timer, panel, RTC or flash behaviour.
Keep installed CLI, board core and libraries; remove temporary Arduino builds.

## Wake watchdog

The IDF 5 task watchdog subscribes an unfed `ventanal-wake` user after reset
accounting and before initializing the panel. It uses a 180000 ms timeout with
panic/reset enabled and stays armed through deep sleep entry. The existing NVS
`wdt-count` records strikes for abnormal resets, including panics, brownouts and
all watchdog reasons. The third strike skips panel access, seeds time from the
standalone RTC over I2C, and sleeps on the normal schedule. If the charge
marker is set, an abnormal reboot uses button-only sleep instead. A returned
refresh or unchanged-frame wake clears the count. The PON timer callback samples
BUSYN and signals a dedicated 8 KiB FreeRTOS task on a stall. That task handles
the two bounded software restarts, then powers down the panel and sleeps if
both retries fail.
The stall diagnostics continue their samples after recording reset strikes.
`VENTANAL_HANGTEST=1` (default 0) loops with delay(1) immediately before every
panel refresh, including the critical-battery screen. The diagnostic alone shortens the watchdog timeout to 60 s; production uses 180 s. Always restore a build without diagnostic defines.
Frame downloads have a 30 s total network budget, using nonblocking reads of available bytes; HTTP connect timeout is 5 s and read timeout 15 s.

## Password-protected transport

With no password and no firmware security values, the HTTP protocol is unchanged.
First password setup starts HTTPS on `[server] https_listen` (default
`0.0.0.0:8766`). An existing password from an earlier installation requires
explicit **Settings → Security → Enable security**; deployment never rotates
or creates that installation's identity automatically.

Visit the HTTPS URL and compare its certificate SHA-256 fingerprint with
Settings → Security before trusting it. Use **Reveal board values** to copy
`VENTANAL_HOST_CERT_PEM` (the complete PEM, as a C++ raw string) and
`VENTANAL_DEVICE_KEY` (64 lowercase hex characters) into private `config.h`.
Change `VENTANAL_HOST_URL` to the displayed HTTPS URL and build/upload normally.
Keep these values out of logs and version control. Regeneration requires updating
the certificate on the board; re-pairing requires updating its device key.

The self-signed ECDSA P-256 certificate is the firmware's sole trust anchor via
`WiFiClientSecure::setCACert`: mbedTLS checks its signature, hostname and validity.
There is no insecure TLS mode or HTTP downgrade when any security value is set.
The task stack is 16 KiB to leave room for certificate rejection paths too.
HTTPS uses HTTP/1.1 and one verified connection across the wake, including
spare refills; every request still has a fresh counter/proof. Partial
responses close that connection. HTTP mode keeps its existing connection behavior.

Each GET signs `method\npath\ncounter` with HMAC-SHA256. The path includes the
exact query string, and the unsigned counter uses decimal digits. The two LF
separators are part of the MAC input. Upgrade the HTTPS server together with
the board firmware so both use the same proof format.

The board commits `sec-counter` in NVS before transmitting;
server state persists the accepted counter before processing reports. Missing,
corrupt or unwritable existing counter storage fails closed. After NVS loss,
use **Re-pair board** and update its key. NVS State/frame layouts are unchanged.

Manifest HMAC covers UTF-8 JSON with recursively sorted keys, compact separators,
array order preserved and integer numbers; `sig` itself is omitted. The server
adds full SHA-256 hashes for main/weather/spare frames. A hash mismatch removes
only the temporary download and keeps the committed cache. The 30-second frame
cap and 180-second wake watchdog remain in force. Serial logs show request
handshake/header and total transfer timings without keys or proof contents.

For an isolated diagnostic build only, pass
`--build-property compiler.cpp.extra_flags=-DVENTANAL_SECURITY_CONFIG=/absolute/path/diagnostic.h`.
That private header can `#undef VENTANAL_HOST_URL` and supply disposable TLS/key
values without editing `config.h`. Omit this property for the production build.

## Wake button gestures

A short press cycles cached screens; a double press cycles spare art. Hold WAKE
for about two seconds to refresh the current screen online without advancing it.
Long press requires GPIO18 LOW continuously for at least 1500 ms from wake
classification start. Any HIGH sample ends long eligibility, even a bounce.
Single/double detection uses a 600 ms window and 30 ms release/second-press
debounce. Holding the second press remains a double press; it cannot become a
long press. Classification does not wait for release after recognizing a long or
double press. Battery protection and bounded panel recovery still apply.

All frame slots (art, weather, solar, two Extras and three spares) share a
105-second cutoff from watchdog arming for starting downloads. A final 30-second
transfer can finish by 135 seconds, leaving 45 seconds for blit/readout, a normal
20-second refresh, PON detection (3 seconds, then a fresh boot) and shutdown.
The watchdog stays unfed throughout networking and display. A separate NVS
`dl-next` cursor records the first deferred slot; it runs first next wake, even
when earlier art changes every wake. Transfers and failed attempts advance the
cursor so a bad slot cannot starve others. Spares now share the pre-display pass.
A stuck display still reaches the 180-second watchdog; that is intentional.
