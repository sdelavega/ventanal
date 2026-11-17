# Optional extras

Ventanal's server and firmware core stay GPL-3.0-or-later. Nothing previously
available becomes paid. Approach C combines separately distributed commercial
add-ons and hosted subscriptions, using offline entitlements. Paddle is the
checkout provider; the core makes no Paddle API calls. Issuing, webhooks,
revocation for hosted services and commercial services belong in future private
projects. The entitlement check in GPL code is not a copy-protection boundary.

Settings → Extras lists Installed, Available and Licences. Installing, owning a
licence and enabling are separate actions: all three are needed for activation.
Perpetual licences work offline indefinitely. Subscriptions work until expiry
and through their stated grace period. Disabled/unentitled render hooks never
run. With no extras, the full core works offline without contacting extra
services. Buying opens a bundled Paddle checkout URL in a separate browser tab.
The production catalogue is empty. The example catalogue and fake entry-point
package under server/tests/fixtures are test-only and excluded from distribution.

## Token version 1

A token is `base64url(payload_bytes).base64url(signature_bytes)`, unpadded,
canonical base64url. The signature is Ed25519 over the exact UTF-8 JSON bytes,
not over their base64 representation. Issuers should use compact sorted JSON;
verifiers authenticate the original bytes. Duplicate and unknown JSON fields,
wrong types, repeated features and oversized tokens are rejected. Maximum token
size is 16 KiB. Required payload fields:

| Field | Meaning |
| --- | --- |
| v | integer 1 |
| kid | embedded public key ID; 1–96 ASCII letters/digits/underscore/hyphen |
| licence_id | issuer's non-personal ID, same grammar as kid |
| licensee | lowercase hex SHA-256 of lowercased email encoded as UTF-8; never email |
| features | 1–64 distinct feature IDs, lowercase letters/digits/dot/underscore/hyphen, starting with a letter, at most 96 chars |
| kind | perpetual or subscription |
| issued | integer UTC Unix seconds |
| expires | integer UTC Unix seconds, after issued; required only for subscription |
| grace_days | integer 0–365 |

Perpetual tokens must omit expires. Issue times tolerate up to 300 seconds of
future clock skew. Expiry and grace end are inclusive, with no extra skew added.
Reasons distinguish malformed payload, unknown signer, invalid signature, future
issuance, valid, grace and expired. Only verified metadata is shown in the UI.
Token bytes never appear in returned Settings/Extras HTML or logs.

The trust store is `ventanal/extras/keys/<kid>.pub`, Ed25519 PEM
SubjectPublicKeyInfo. Multiple files support rotation; retain old public keys
while their licences are supported. **No production key ships yet: all real
submissions currently return unknown signer.** Test keys must stay under tests,
never in this directory. Tests create new throwaway signers in memory. A committed public key and token
form a deterministic, explicitly public test vector generated in the test itself;
neither is trusted by the package.

`server/scripts/extras_keytool.py generate --private /outside/repo/key.pem
--public /outside/repo/test.pub` creates a developer keypair, with private file
0600. Existing outputs and private paths inside the resolved repository are
refused, including symlink aliases. `sign --private /outside/repo/key.pem
--payload payload.json --output /outside/repo/token.txt` signs test payloads to
an exclusive 0600 output file; it does not print tokens or key material. These
are developer tools, not the future issuing service.

Tokens and extra settings live in atomic 0600 `extras.json` beside the existing
auth/security state in the configured cache/state directory. Writes use a shared
cross-process lock. No token is stored in TOML. Back this private state up as you
do the transport identity. Removing a licence immediately removes its entitlement
from subsequently served manifests; cached board frames disappear on its next
successful online manifest refresh. Offline board caches cannot be remotely revoked.

## Building an extra

Publish an independently installed Python distribution with an entry point:

```toml
[project.entry-points."ventanal.extras"]
my-extra = "my_package:manifest"
```

Export a `ventanal.extras.registry.Manifest` object with id, name, description,
kind (`add-on` or `hosted`), required feature id, privacy note, tuple of `Field`
settings, tuple of screen IDs and a callable renderer. Imports/registration must
be side-effect free: no requests, threads or telemetry at import time. Installed
Python code is trusted local code, not sandboxed. Only install trusted packages.
Field kinds are text (max 1024 chars) and bool; use defaults. Secret credential
fields are not supported by this initial framework; do not put secrets in these
controls. Extra settings have their own private state and do not rewrite TOML.

The renderer receives `(cfg, aware_now, settings_dict)` and returns a mapping
from declared screen IDs to `(encoded_VNF_bytes, Readout, PIL_preview)` tuples.
Use the existing renderer, quantizer and frame encoder. Rendering executes on
normal ticks under tick.lock, with fixed failure messages and previous valid
frames retained on failure. Settings apply immediately; publication follows the
next ordinary tick. Each extra may declare at most two screens, globally limited
to two active extra screens, in deterministic entry-point name order. IDs must
start `extra-`, use lowercase ASCII letters/digits/underscore/hyphen, and fit
48 characters. They may not collide. Empty-screen service manifests are allowed.

The served manifest has a signed `screen_order` array: art, weather, solar when
available, then up to two extra screen IDs. Firmware follows that order on single
WAKE presses, skips unavailable caches, and retains the selection on timers/long
refreshes. Each extra cache uses an atomic replacement, with a full-frame plus
64 KiB free-space check. Old core/solar State records migrate with owner history,
queue and spare selection intact. Existing transport signs the order, entries
and frame hashes. No firmware change is needed for each new extra.

## Privacy and catalogue rules

A hosted extra must declare a human-readable privacy note explaining precisely
which data leaves the LAN, to whom, why, retention and deletion. Send only what
its feature needs, only while enabled and entitled. Do not send raw email,
credentials, artwork history or telemetry by default. Do not log tokens or
upstream exception messages. A future hosted revocation client must authenticate
signed revocation data and run only while its hosted extra is in use.

Catalogue entries have id, name, description, kind, privacy (required for hosted)
and checkout_url. Only HTTPS buy.paddle.com/checkout.paddle.com URLs are accepted.
Example entries marked `example: true` are hidden. Catalogue updates ship with
releases; there is no background catalogue fetch. See [CONTRIBUTING](../CONTRIBUTING.md)
for contribution and commercial relicensing terms.
