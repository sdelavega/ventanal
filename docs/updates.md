<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Stephen de la Vega -->
# Wireless firmware updates

Ventanal checks for firmware releases daily. On the host's **Status** page, use
**Check for updates** to check immediately. A paired board shows its running
version and last update result. Select **Install** next to an available version
to approve it for that board. **Install automatically** is a per-board option
and starts off. **Upload signed release** accepts the application image, JSON
metadata and detached signature for a private or offline release.

The board verifies the owner's Ed25519 signature, the board ID, version
eligibility, image size and SHA-256 digest before starting the new image. The
target must be newer than the running version and the running version must meet
the release's minimum. The Inkplate only installs with at least 50% battery
and outside quiet hours. It can pick up an approved update on a later connected
wake when those conditions are met.

On the LCD, open **Settings → Firmware updates** and select **Install** beside
the host-approved version, or **Later** to return to Settings. A host approval
makes an offer available on that LCD. The local Install action starts it
without another prompt. **Check for updates** looks for a release on GitHub
from the LCD and allows a separate install. A completed GitHub check and its
offer are saved for a week between checks.

After three failed attempts for the same host approval, the board reports
`failed:attempts`. Select **Install** again on the host to grant a new approval,
or approve a newer version. The LCD has a separate three-attempt budget for each
GitHub version. The Inkplate's direct GitHub check is available with its
[diagnostic build flag](../firmware/README.md#toolchain).

## Rollback

The board writes to its spare app slot and leaves the running slot intact.
An interrupted transfer leaves the existing image bootable. After switching,
the new image must connect to Wi-Fi, verify the host manifest and draw a frame
before it is marked healthy. If it resets before confirmation,
the bootloader returns to the previous image. The next check-in reports the
result on **Status**. A successful install or rollback clears the host's
approval for that version. After rollback, inspect the result and use a fixed,
newer build before approving another update.

## One-time wired setup

Wireless updates carry one app image. Each board needs a wired flash to install
an OTA-capable bootloader, the two-slot partition table and the initial
configured app. Preserve NVS by keeping its existing offset and size and
avoiding a full-chip erase. The relocated LittleFS cache is reinitialized and
frames are downloaded again. After the wired flash, check that the device
retains its pairing and completes a normal refresh.

On the Inkplate, the one-time wired flash with a private `config.h` provisions
Wi-Fi, the host URL, timezone, display and wake settings, and any TLS certificate
bundle and pairing key into NVS. Subsequent release images read these settings
from NVS. An unprovisioned release image shows the waiting screen and sleeps.
Keep the private build for wired recovery.

A wired private build preserves the current settings after provisioning. To
apply changed private settings deliberately, compile it with
`-DVENTANAL_FORCE_PROVISION=1` in `compiler.cpp.extra_flags`. Each nonzero
revision applies once, so use a new revision for each later reprovisioning.
The replay counter stays in its existing NVS namespace.

On the LCD, the first wired build with a private installation header provisions
Wi-Fi, the host origin, timezone, display profile and optional pairing and
TLS values. Generic builds read that installation from NVS. An unprovisioned
generic LCD displays a waiting screen. To revise a provisioned installation,
add `-DVENTANAL_FORCE_PROVISION=1` to the PlatformIO private build flags, using
a new revision for each change. Brightness, sleep settings and the pairing
counter keep their existing storage.

| Board | Wired migration |
| --- | --- |
| Inkplate 13SPECTRA (`inkplate13spectra`) | Follow the [Inkplate build and upload instructions](../firmware/README.md#toolchain) using the board's private settings. Use the project upload helper, which puts the app at `0x20000` and OTA metadata at `0xf000`. NVS stays at `0x9000`, 24 KiB. |
| Waveshare 4.3B (`lcd43b`) | Build the configured `normal` target and follow the [LCD wired upload instructions](../firmware/lcd43b/README.md#private-configuration-and-building). The initial application is at `0x10000` and OTA metadata is at `0xe000`. NVS stays at `0x9000`, 20 KiB. |

## Owner signing key

Generate the release key on an offline machine using the installed Ventanal
CLI. Use a path outside the checkout on encrypted storage. The private PEM
itself is unencrypted, so keep its offline storage and backup protected.

```sh
ventanal release keygen /path/to/offline/release.pem /path/to/release.pub
```

Export the 32 raw Ed25519 public-key bytes from `release.pub`. Put the same bytes
in the `VENTANAL_RELEASE_PUBLIC_KEY` array in
`firmware/common/release_key.h` and the hex string in
`server/src/ventanal/release.py`. Remove the test-key comments above the
active keys after replacing them. Keep the compile-time guard in place.
The checked-in development key is a public test fixture and the release build
rejects it. Never put the private PEM in the repository, CI or a GitHub release.

Deploy a host using the owner's public key and seed the board with the matching
configured image during the wired setup. For a later key change, repeat this
process with a new keypair and a wired reflash of each board. Existing firmware
trusts its compiled public key. Keep access to the old key until the migration is
complete.

## Build, sign and publish

1. Choose a new `MAJOR.MINOR.PATCH` version, with no leading zeros. A tag
   named `vMAJOR.MINOR.PATCH` in the public repository triggers the firmware
   workflow. It builds the Inkplate and also builds the LCD when its
   `firmware/lcd43b/platformio.ini` is present. The workflow attaches each
   board's `.bin` and canonical `.json` to a draft. The tag sets the version
   compiled into each image and recorded in its metadata.
2. Review the assets and release notes before signing. Each pair is named
   `ventanal-<board>-<version>.bin` and
   `ventanal-<board>-<version>.json`. The build uses public configuration.
   Inkplate release images rely on the settings provisioned by the wired
   setup. Keep builds with installation-specific settings private.
3. On the offline signing machine, sign the exact JSON bytes and verify the
   signature and image against the expected release public key. Repeat for each
   board. For example, for an installable Inkplate image:

   ```sh
   asset=ventanal-inkplate13spectra-1.2.3
   ventanal release sign "$asset.json" --key /path/to/offline/release.pem
   ventanal release verify "$asset.json" --key /path/to/release.pub --image "$asset.bin"
   ```

4. Attach each resulting `.json.sig` file to the draft next to the original
   image and JSON. Publish manually when the supported boards have complete,
   verified and installable sets. Editing the JSON after signing invalidates
   its signature.

For a local copy of the public build, install the [pinned Inkplate core and
libraries](../firmware/README.md#toolchain) and the server's Python environment.
The workflow uses Arduino CLI 1.6.0. When the LCD project is present, also
install `pioarduino==6.2.0` in a Python environment whose `pio` is on the
command path. Its project pins the pioarduino platform release to
`55.03.312-1`. From the repository root, use a new output directory:

```sh
server/.venv/bin/python tools/release/build.py 1.2.3 --output build/ventanal-1.2.3
```

Use the same clean checkout and toolchain as CI. The script stages tracked
firmware sources, uses the public Inkplate example configuration and emits
unsigned assets. Add `--dry-run` to inspect the commands and output names.
Use `--board lcd43b` for only the LCD and `--pio` to select a local PlatformIO executable.
The default minimum running version is `0.0.0`. Local builds can set
`--min-from-version` before generating and signing the metadata.
