<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Stephen de la Vega -->
# Ventanal firmware

The firmware caches art, weather, optional solar and Extras screens, overlays the
readout, applies battery and local-time scheduling, and sleeps between updates.
A fresh installation starts on weather; a short WAKE press cycles cached screens
without starting Wi-Fi.

See [wireless firmware updates](../docs/updates.md) for update approval,
rollback and release signing.

Secure Boot and flash encryption are never enabled; see [security hardening](../docs/security-hardening.md) for why and for the disabled scaffolding.

## Toolchain

From the repository root, create a CPython 3.13 virtual environment for
PlatformIO and the upload helper:

```sh
python3.13 -m venv firmware/.venv
firmware/.venv/bin/python -m pip install --require-hashes --only-binary=:all: -r tools/release/requirements-platformio.txt
firmware/.venv/bin/python -m pip install "esptool>=5,<6"
```

The project pins pioarduino `55.03.312-1` (Arduino-ESP32 3.3.12, ESP-IDF 5.5.5),
InkplateLibrary 11.2.1 and ArduinoJson 7.4.3. The first build downloads the
toolchain and the checksum-verified Soldered Inkplate_S3 pin header. Set
`UV_CONSTRAINT` for PlatformIO commands so the platform's inner Python
environment uses the same CLI version as the outer venv. The custom
board uses 32 MB octal flash at 80 MHz, octal PSRAM, and UART0 serial with USB CDC
disabled on boot. Its board manifest explicitly supplies
`ARDUINO_INKPLATE13SPECTRA`, which appears once per compiler command in the
pinned hybrid build.

Install the [owner release public key](../docs/updates.md#owner-signing-key)
before building firmware for a deployed board. For a new installation, copy
`ventanal/config.example.h` to the ignored `ventanal/config.h` and edit it privately.
Create the ignored `firmware/ventanal/platformio.local.ini` with:

```ini
[inkplate]
private_flags = -DVENTANAL_INSTALLATION_CONFIG=config.h
```

PlatformIO prints the installation header path it selected. If `config.h`
exists, `normal` and `diag` require an explicit installation header selection.
With no local header present or selected, builds use `config.example.h` and
need installation settings already provisioned in NVS. The release builder
always selects `config.example.h`. See
[wireless firmware updates](../docs/updates.md#one-time-wired-setup) for provisioning.

From the repository root, build and upload with the board's serial port:

```sh
export UV_CONSTRAINT="$PWD/tools/release/platformio-constraints.txt"
firmware/.venv/bin/pio run -d firmware/ventanal -e normal && \
  firmware/.venv/bin/python firmware/tools/upload.py firmware/ventanal/.pio/build/normal --port /dev/cu.YOUR_BOARD
```

Use the project upload helper for wired installs. It checks the generated
partition table and image sizes before writing the bootloader at `0x0`, the
partition table at `0x8000`, the OTA selector at `0xf000` and the app at
`0x20000`. It preserves NVS and LittleFS. Add `--dry-run` to validate the
images and see the command without opening the serial port.

NVS: 0x9000/24K;
otadata at 0xf000/8K, `ota_0` at 0x20000/4M, `ota_1` at 0x420000/4M,
LittleFS at 0x820000/0x17d0000, and coredump at 0x1ff0000/64K.
Moving from the factory layout requires a wired upload to seed `ota_0`.
The device initializes the relocated LittleFS and downloads its frames again.
Pairing and counters stay in NVS.

`VENTANAL_OTA_GITHUB=1` enables diagnostic checks of the latest GitHub release
at most once a week. It uses the ESP-IDF certificate bundle and applies the
same signed-image checks and battery and quiet-hours gates as host offers.
The first boot of an updated image requires Wi-Fi, a valid host manifest, and
a completed frame refresh. A transient connection failure gets one light-sleep
retry while the image is still pending.

## Modes and hardware checks

Use `normal` for an owner-key build and `bench` for an isolated build with the
checked-in test key. `diag` requires the owner key and enables GitHub OTA
checks. Each environment writes to its own directory under `.pio/build`.

For a full wake diagnostic with 60-second sleeps, add this option in the
`[inkplate]` section of your ignored local INI and build `diag`:

```ini
diag_flags = -DVENTANAL_WAKETEST=1
```

`VENTANAL_WAKETEST=2` also simulates the app's button path on timer wake 2.
Serial identifies the simulation. The physical GPIO18 button needs a separate
check. Upload the build from the environment you compiled.

Serial is 115200 baud. Capture with the Python environment created above:

```sh
firmware/.venv/bin/python firmware/tools/capture_serial.py --reset --seconds 100
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
only when the `ventanal` NVS namespace is absent or NVS confirms that both saved
`state13` and quarantined `state-bad` are absent. Formatting erases the whole
LittleFS partition. If the first format fails, the firmware logs
`error=cache-provision` and can retry on the next wake. The firmware creates
`state13` only while the filesystem is mounted.
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

To simulate interruption after a good cache exists, set
`diag_flags = -DVENTANAL_ABORT_AFTER_BYTES=100000` in the local INI and build
the `diag` environment.

Upload that build with a changed/missing frame to exercise its download branch.
It aborts at exactly 100000 bytes, leaving the partial `.tmp`. A failed transfer
preserves the committed cache. After checking recovery, upload the normal build
with all flags disabled.

State is in the `ventanal` NVS namespace (`state13` blob): active screen, cached
IDs/rectangles and `Drawn`. Battery smoothing history persists across deep sleep
in RTC memory. At critical voltage the firmware records the refresh attempt
in NVS before drawing “Charge me,” then enters deep sleep with every wake
source disabled. A brownout can leave a partial image. The marker suppresses
a repeat while the battery is critical and sends an abnormal reboot directly
back to sleep. A stalled “Charge me” refresh also goes straight to sleep.

**After charging, turn the ON/OFF switch off and back on, or press RESET.**
The WAKE button and timer are disabled during critical sleep. On power-up or
reset the firmware runs its normal checks. A successful replacement refresh
clears the marker.

On the 13SPECTRA v1.1, the physical ON/OFF latch supplies the main regulator,
whose enable is wired to its input. Firmware controls the separate panel
regulator through GPIO21. Before critical sleep it shuts down Wi-Fi, holds
GPIO21 low and sets the panel signal pins to input to prevent backfeeding. The
library turns off the battery-sense divider after sampling, and the unused
microSD supply defaults off. The RTC, I/O expander and optional blue power LED
remain on their normal supplies. The power LED has its own switch. Sleep
current and board behavior still need post-soak verification.
[Soldered v1.1 schematic, sheets 2–6](https://community.soldered.com/uploads/short-url/7CTPEO0Ta4QczGAJhpwzArGSHxY.pdf)
[Inkplate 13SPECTRA driver](https://github.com/SolderedElectronics/Inkplate-Arduino-library/blob/master/src/boards/Inkplate13SPECTRA/Inkplate13SPECTRADriver.cpp)

RTC is initialized by `display.begin()`. A set PCF85063A seeds system time even
offline. Online `configTzTime` waits up to 15 seconds for NTP completion and writes
the RTC. RTC stores UTC; the library's local-time epoch helpers are called with
TZ temporarily set to UTC, then the configured POSIX TZ is restored.

## Host checks and cleanup

```sh
export UV_CONSTRAINT="$PWD/tools/release/platformio-constraints.txt"
firmware/.venv/bin/pio pkg install -d firmware/ventanal -e bench
make -C firmware/test test
make -C firmware/test clean
```

Host tests use the ArduinoJson headers from a project's PlatformIO environment.
If the headers are installed elsewhere, pass
`ARDUINOJSON=/path/to/ArduinoJson/src` to make.

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
marker is set, an abnormal reboot disables all wake sources. A returned
refresh or unchanged-frame wake clears the count. The PON timer callback samples
BUSYN and signals a dedicated 8 KiB FreeRTOS task on a stall. That task handles
two bounded software restarts for ordinary refreshes, then powers down the
panel and sleeps if both retries fail. A critical refresh stall goes directly
to the critical sleep path.
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
If the board already has provisioned settings, use a new force-provision
revision as described in the [wired setup guide](../docs/updates.md#one-time-wired-setup).
Keep these values out of logs and version control. Regeneration requires updating
the certificate on the board. The device key stays valid until you explicitly
choose **Re-pair board** and copy the new key. See the
[regeneration and pairing steps](../README.md#security-and-pairing).

The self-signed ECDSA P-256 certificate is the firmware's sole trust anchor via
`WiFiClientSecure::setCACert`. For a locally trusted self-signed leaf, mbedTLS
requires an exact certificate match and checks the hostname or IP. The TLS
handshake verifies proof of possession of the leaf's private key. Certificate
date checks depend on the core's mbedTLS build; the documented Inkplate core
ships with those checks disabled.
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

For an isolated diagnostic build, add
`-DVENTANAL_SECURITY_CONFIG=/absolute/path/diagnostic.h` to `diag_flags` in
the local INI.
That private header can `#undef VENTANAL_HOST_URL` and supply disposable TLS/key
values without editing `config.h`. Use a new force-provision revision if the diagnostic build needs to replace
stored settings. Omit the diagnostic header for a release build.

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
