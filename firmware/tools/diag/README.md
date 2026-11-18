<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Stephen de la Vega -->
# Panel stall evidence tools

`VENTANAL_STALLTEST` defaults to 0. A nonzero integer enables a 40-attempt
cached-frame diagnostic and also identifies the run in NVS (`stall-run`). Use a
new integer for a fresh run; reflashing the same value resumes the attempted
index. `stall-i` is stored before blit/display, so a watchdog-aborted attempt
is recorded once and the next boot advances. `stall-sleep` records the boundary
before samples 5, 10, ..., 40: ten seconds of actual timer deep sleep. Other
samples wait five seconds. Odd samples draw art, even samples weather, both with
the normal readout. A normal application wake precedes the first sample.

The diagnostic alone uses a 60000 ms watchdog, bypasses the three-strike draw
limit, and renews the watchdog user immediately before each numbered attempt.
There are no feeds during blit, readout or display. Production retains its
unfed 180000 ms whole-wake deadline and three-strike limit. On reset, the next
sample's `after=pause` means a five-second pause on that recovery boot; the ROM
reset and `reset=watchdog` lines distinguish it from an uninterrupted pause.

Run `python3 firmware/tools/diag/prepare.py /tmp/ventanal-stall` once with a
fresh scratch destination. It copies libraries from the installed Ventanal
sketchbook and applies `busy.patch` only in the scratch copy. Compile with
`arduino-cli --config-file /tmp/ventanal-stall/arduino.yaml compile` and the
normal board arguments from task-robust-report.md, plus
`--build-property compiler.cpp.extra_flags=-DVENTANAL_STALLTEST=1`.
Use a dedicated build path. Never apply this patch to the installed library.

The patch retains the original driver signature, waits, delay(1), SPI commands,
power sequence and GPIO configuration. It adds stage tags and Serial logging,
BUSYN reads and one battery ADC read per display. Logs perturb timing; voltage
is an ADC battery measurement before power-up, not a measurement of the panel
rail under load. All eight wait sites are tagged, including partial-display
sites not exercised by Ventanal's full display path. `busy-stuck` is printed once
after five seconds and continues waiting. A normal DRF wait exceeds that
threshold; only an unfinished attempt plus watchdog evidence identifies a stall.

Capture through `stalltest done=40`, then upload a build made with the default
Arduino configuration (installed unpatched library) and no diagnostic defines.
Capture a clean reset boot through `stack min-free bytes=`. Do not leave the
board running the diagnostic. `analyze.py LOG` prints the refresh table and
min/median/max wait statistics; `--json` includes entry voltage/time and levels.
Only completed refreshes contribute to the normal wait distributions.

## PWR_EN hypothesis experiment

Add `-DVENTANAL_PWRENTEST=1` alongside a fresh nonzero STALLTEST run ID
(e.g. `-DVENTANAL_STALLTEST=1116`). This diagnostic-only switch holds GPIO21
low across deep sleep, logs `pwr_en_on_wake` before releasing its per-pad hold
before panel initialization, and sleeps before every even sample (20 of 40).
GPIO21 uses input/output mode so its digital input can be sampled as well as
actively driven. The read is a software logic level, not an electrical trace.
No other pad hold is enabled. The installed ESP32-S3 SDK requires both
`gpio_hold_en` and `gpio_deep_sleep_hold_en` for a digital deep-sleep hold.
The original cadence remains unchanged with PWRENTEST off. Capture to
`pwren-run.log`; reject the hypothesis on any stall. Always restore an unpatched
normal build without either diagnostic flag, and capture `pwren-final.log`.

## Boot-time hypothesis experiment

Use `-DVENTANAL_STALLTEST=111602 -DVENTANAL_BOOTTIMETEST=1` with the patched
scratch library and PWRENTEST off. This default-off mode replaces the original
cadence with ten seconds of actual timer deep sleep before **every** numbered
sample, including sample 1 and the first sample after watchdog recovery. It
runs 32 alternating art/weather attempts: eight each at boot gates 4000, 5500,
7000 and 8500 ms, in that order. Immediately before display it waits with
`delay(1)` until `millis()` reaches the gate and logs
`boot_gate ms=<L> entry_ms=<millis>`. The attempt index is saved before blit;
a watchdog reset advances it, and another ten-second sleep precedes the next
attempt. The existing 60-second diagnostic watchdog remains active.

Capture through `stalltest done=32` to `boottime-run.log`. Support requires a
stall at 4000 and a zero-stall suffix of higher tested levels. Only then use a
fresh STALLTEST run ID and add `-DVENTANAL_BOOT_CONFIRM_MS=<L*+1500>`: the same
mode runs 20 deep-sleep samples at that gate, ending `stalltest done=20`.
Confirm only with zero stalls. Otherwise reject and restore normal firmware.
The boot-time flags add no production gate; a production change is conditional
on hardware confirmation. Always restore the unpatched normal build and capture
`boottime-final.log`. See task-boottime-report.md for the measured decision.

## Production PON retry verification

Use a fresh STALLTEST ID plus `-DVENTANAL_PONRETRYTEST=1`, with BOOTTIMETEST,
PWRENTEST and HANGTEST off. This runs 20 alternating art/weather samples, each
preceded by a real ten-second timer sleep, including sample 1. The ordinary
production 50 ms BUSYN monitor wraps every display call. BUSYN never observed HIGH
in the inclusive 500..3000 ms window after display starts restarts the ESP, retaining the exact pending draw in RTC NOINIT.
Software-reset recovery replays that draw without networking. A retry is part
of the same numbered sample; only its successful display logs completion.
The next numbered sample again follows ten seconds of actual deep sleep.
The diagnostic retains its existing 60 s watchdog; production retains 180 s.
Diagnostic display-start and reset markers include board millis; subtract them
to verify reset completion precedes 500 ms. HIGH before 500 ms is ignored.

Capture through `stalltest done=20` to `ponretry-run.log`. Host monotonic
millisecond timestamps on received lines allow elapsed time across software
resets: subtract the first `panel display-start attempt=0` timestamp for a
sample from its `stalltest complete i=N` timestamp. Count
`panel pon-stall restart attempt=N` within the sample; require at most two,
exactly 20 completions, and no watchdog reset or retry-limit marker. These are
serial receive times, including reboot and redraw, with USB/logging latency.

Build and restore normal firmware with the installed unpatched library and
no diagnostic defines; capture a cold boot through `stack min-free bytes=` to
`ponretry-final.log`. Production retry code remains enabled in the normal build.
See task-ponretry-report.md for measured results and build commands.

## Owner State retention regression

`-DVENTANAL_STATELOSTTEST=1` runs the ordinary application with ten-second
sleeps. After the initial online refresh, three timer wakes simulate offline
double presses. The first deliberately suppresses BUSYN detection and waits
for the production PON monitor to restart the ESP; retry boots use the real
panel driver. This is a forced software PON condition, not an electrical stall
measurement. Later presses may also hit real PON stalls after the short sleep.
The next timer wake reports the queued pairs and checks the normal spare cache.
Capture through `diagnostic=statelost-done` (after manifest acknowledgement,
spare handling and State save). This mode repeats timer wakes until reflashed;
restore a normal build without extra defines promptly.

`state phase=boot-load` logs the NVS record length, actual getBytes return,
State size, queue count, cursor, override ID and three spare IDs. `retry-load`
logs the retained RTC snapshot; `save` and `sleep` expose the state on the
application and PON retry paths. The reset-strike limit sleeps before loading
state. IDs are bounded to sixteen characters; no
credentials, URLs or configuration settings are logged. Existing incompatible records or reads that fail twice are copied byte-for-byte
into `state-bad` (replacing any earlier quarantine), with
`error=state-load quarantined bytes=<n>`. This wake then provisions from defaults
online and normal saves replace `state13`. A single read failure retries without
quarantining. If raw NVS recovery or the quarantine write/commit fails, the
original key is retained and `error=state-load quarantine-failed` precedes a
60-second retry sleep. A missing record still provisions normally.
