# Security hardening (Secure Boot and flash encryption)

**Ventanal never turns any of this on.** Everything described here is permanent once a board boots with it. People install Ventanal on their own hobby boards, and the project will never risk locking anyone out of their own device, by accident or on purpose. The build files contain only commented-out scaffolding and placeholder key paths. The firmware refuses to compile if any of these options becomes active without two explicit opt-ins.

Read this page only if you are building your own device and understand that a mistake can make a board permanently unable to take new firmware.

## What each feature does

| Feature | Protects against | Cost |
|---|---|---|
| **Secure Boot v2** (RSA-3072 on the ESP32-S3) | Someone installing their own firmware over USB, or a modified image over the air | Every image must be signed with your key. Lose the key and the board can never be updated again, wired or wireless. |
| **Flash encryption** | Someone reading flash contents (Wi-Fi password, pairing key) with esptool or a clip on the flash chip | A key generated inside the chip, unreadable by anyone. "Development" mode allows a few more USB reflashes; "release" mode allows over-the-air updates only. |
| **NVS encryption** | The settings store (Wi-Fi details, pairing keys) is not covered by flash encryption on its own | Another eFuse key. |
| **Secure ROM download mode / disabling download mode** | Reading flash over USB | Blocks recovery over USB. |

Ventanal's own release signature (Ed25519, checked by the firmware before an update installs) is separate. It stops a compromised host or network from pushing firmware, but it cannot stop someone who replaces the whole firmware over USB. Secure Boot closes that gap.

## Cheaper first steps

- Put your frames on a separate IoT/guest Wi-Fi network, so a leaked password only exposes that network.
- Keep the release signing key offline (see [updates.md](updates.md)).

## If you still want to try: practise first

1. Use a **spare** board you can afford to lose.
2. Start with **virtual eFuses**: uncomment `[env:hardened-virtual]` in the board's `platformio.ini`. With `CONFIG_EFUSE_VIRTUAL=y` nothing is burned. The eFuse values live in flash, and the guard allows this without opt-ins.
3. Generate your own Secure Boot key (`espsecure.py generate_signing_key --version 2 --scheme rsa3072 your-key.pem`), keep it offline, make two backups, and replace `PLACEHOLDER-secure-boot-signing-key.pem`.
4. Check that wireless updates still work end to end, including rollback, with the virtual setup.
5. Only then consider `[env:hardened]` on real eFuses. That environment must also define `VENTANAL_I_ACCEPT_PERMANENT_EFUSE_CHANGES=1` and `VENTANAL_SECURE_BOOT_KEY_READY=1`, or `firmware/common/hardening.h` stops the build.

## Guard rails in this repository

- `firmware/common/hardening.h` (included by every firmware build) fails compilation when any permanent security option is active on real eFuses without both opt-ins.
- `tools/release/build.py` refuses to produce public release assets if a board's `platformio.ini` has any of these options active. Public releases are always unhardened, so anyone can reflash their board.
- `make -C firmware/test test` checks the guard's behaviour for every option.

## Status

- **E-paper (Inkplate 13SPECTRA) and LCD (Waveshare 4.3B):** scaffolding only, untested. Whether the pioarduino hybrid build rebuilds the bootloader with these options has not been verified.
- Never use these options on a board someone else owns.
