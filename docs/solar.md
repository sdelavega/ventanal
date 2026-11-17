# Solar weather, first pass

The optional solar screen is available for active, unexpired US amateur licences
whose FCC registration ZIP5 matches the configured location ZIP5. Enter a call
sign in Settings → Ham radio. It is checked locally; no call sign is included
in any outbound request. This is an availability gate based on public data,
not proof of identity. Other countries' licences are outside this first pass.

The official weekly ULS archive is
<https://data.fcc.gov/download/pub/uls/complete/l_amat.zip> (verified 2026-11-16).
`solar/licence.py` streams HD, licensee EN and AM pipe records through bounded
SQLite staging tables, joining by unique system identifier. In EN, state is
field 18 and ZIP is field 19 (one-based). The compact index retains only call,
status, expiry, operator class and ZIP5, plus refresh time. Index replacement is
atomic; the previous database survives failures and the raw archive is deleted.
The weekly systemd FCC timer also runs five minutes after boot. Settings can
start a refresh in a background process. A file lock prevents overlapping imports. Services use private writable temporary
directories so SQLite can spill disk-backed sorts while ProtectSystem remains strict.
Normal ticks also refresh an index older than seven days when a call is configured,
before acquiring the tick lock. Imports cannot block other weather/art ticks.
Lock contention reports "Database refresh already running" in Settings. A corrupt
index asks the owner to refresh rather than claiming it has never been downloaded.

NOAA products, NASA's 171 Å SDO image, URLs and TTLs are centralized in
`solar/data.py`. The daily solar text supplies F10.7 and SESC sunspots because
the F10.7 JSON endpoint was stale when checked. Planetary Kp's `a_running`
supplies running planetary A; the X-ray class uses GOES 0.1–0.8 nm flux.
The three-day text provides per-day Kp maxima; NOAA scales provide flare,
radiation and geomagnetic probabilities/levels. Warnings honor validity/extension
times. Watches use their dated UTC storm predictions. Only the newest message
per product is considered; cancellations suppress that product. Instant alert
notifications are shown for six hours, continued alerts for 36 hours, since these
have no explicit end time. These windows are display policy, not inferred
physical event durations. Full NOAA history is never treated as active alerts.

The 171 Å image gives clear coronal contrast in the six panel colours. It is
fetched once per UTC day; a centred 4.5% margin crop removes the burned-in timestamp. Sources
can be delayed. Failed data fetches retain the previous published frame, whose
readout timestamp remains its previous render time. The live-data preview is a
simulation, not a physical colour-accuracy claim.

`solar/conditions.py` owns the band groups and heuristic thresholds. These are
broad, non-location-specific **estimates**, not measured propagation predictions:

- MUF MHz = clamp(10 + 0.1 × SFI, 10, 45), rounded.
- Each band has SFI thresholds for Fair/Good day conditions and a baseline night
  rating. K ≥ 4 or A ≥ 20 reduces a rating one step; K ≥ 6 or A ≥ 50 reduces two.
- Daytime M/X absorption reduces another one/two steps. Noise is a K-only proxy:
  S1–S2 below K3, S3–S4 below K5, S5–S7 below K7, otherwise S8–S9.

The layout group table is `solar/render.py:LAYOUT`; fonts, exact SOLID ink and
pills, PATTERN flat fills, DIFFUSE solar image, quantisation and LCARS-style
readout reuse the weather pipeline. The frame is 1600 × 1200. Test fixture
rendering has an exact simulated golden in `server/tests/golden/solar.png`.

The existing tick publication lock serializes solar with other screens. A new
Kp interval, daily SFI/SSN, forecast values or daily image may publish only after
three hours since the last solar publication. Alert set changes bypass that
limit. X-ray fluctuations and timestamps alone do not cause new frame downloads.
Gate failure removes the solar manifest entry; serving also rechecks the gate
before signing. Existing manifest HMAC and SHA-256 protection cover solar.
The board downloads atomically, selects solar only with a cached frame, and
removes it when a successfully verified manifest omits it. An offline board can
retain its last cache until it next receives a manifest.

Firmware appends solar cache/readout metadata after the previous State prefix.
An explicit 1084-byte legacy branch copies the entire previous owner State,
including queue, cursor, override and spares; it does not quarantine this upgrade.
The older pre-spare migration branch remains supported. RTC/PON replay now also
selects solar's exact frame/readout. Double presses keep their existing spare
behavior. Single presses cycle art → weather → solar → art with solar cached,
otherwise art ↔ weather.

Current K is planetary Kp rounded to the nearest integer (halves upward): 0–3
quiet/green, 4 unsettled/yellow, 5–9 storm/red. Forecast Kp uses NOAA thirds
notation (4.33 → 4+, 4.67 → 5−). The chart spans 0–9 over its full height,
with 0/3/6/9 ticks, a four-pixel minimum zero bar and UTC interval hours.
Forecast headings use UTC weekday/day and a quiet/unsettled/active/storm descriptor.
Band pills use bold 22 px SOLID white text for Good/Poor and black for Fair.
Calibrated Oklab lightness contrast must be at least 0.28 for Good and 0.35
for Fair/Poor. The shared frame keeps weather geometry and its default violet accent. Solar
selects the original LCARS-inspired orange (#ff7700) through PATTERN, with
sky/red/gold/sky/violet sidebar segments; the battery corner stays shared.
Both sun captions are centred on the sun. Solar uses a large flux
readout, four metric cells, separate summary and band rows, full-width Kp chart
with a G1 (Kp 5) reference, three forecast columns and a final alert line. Alerts use a
bright bold label and light summaries of up to two active products (type, scale,
UTC dates); fitting keeps 18 px type, omits lower-priority secondary items first,
then shortens the primary by whole words with an ellipsis to fit the width budget.
NOAA scales are labelled as current. The verified configured call sign appears
only in the private runtime header; golden/live evidence uses CALL SIGN.
Every NOAA/NASA/FCC request sets Ventanal's User-Agent independently of its client.

Round-four content uses 120 px SFI, 56 px metric values, 20 px labels, 28 px
band names, 38 px pills, 32 px forecast days and 24/20 px forecast lines.
Captions remain at least 18 px. The sun is 340 px; band rows have 28 px gaps.
The 90 px Kp plot retains its full 0–9 scale and reference line. Measured text
bounds enforce 28 px between major rows, 20 px forecast/alert clearance and
24 px alert/bottom-bar clearance. Sidebar segments use (y, height) pairs
(212, 126), (344, 146), (496, 264), (766, 114), and (886, 102), with
six-pixel gaps and label baselines at y + height − 12. The 3-DAY segment ends
six pixels before the bottom elbow at y=994. Content rows retain their positions.


## Publication runbook

The owner-authorized look rollout on 2026-11-17 manually republished solar
under `tick.lock`, after checking the existing security and licence gates.
`solar-state.json` was atomically hand-advanced to the actual publication time,
with the normal meaningful-data key, alert set and context hash. This restarted
the three-hour interval; it did not alter owner settings, auth or publication
policy. Round-one visual fixes use the same explicit rollout procedure.

After a rollout, run the ordinary solar tick and verify it completes cleanly.
An immediate `False` is expected if neither alerts nor meaningful data changed.
Check the next eligible natural tick after three hours and a meaningful change:
the manifest solar timestamp and `solar-state.json` publication time should
advance together. Do not delete state or backdate its timestamp to force a tick.


## Renderer rollout runbook

On 2026-11-17 the owner-requested colour rollout deliberately republished solar
inside the three-hour guard. Under `tick.lock`, with transport security and the
FCC licence gate verified, the rollout script rendered current data, published
it, then atomically hand-advanced `solar-state.json` (`key`, `alerts`, `published`,
`context`) to that publication. Fix round 1 repeated this explicit republication.
The timestamp starts the next normal three-hour interval; no clock spoofing,
auth change, owner-setting change, or permanent rate-policy bypass was used.
Do not delete the state to force a normal tick. After the interval, verify the
next natural solar tick publishes when meaningful data changes (an unchanged
data key correctly returns false). Inspect publication timestamps and frame
hashes without dumping the private state/context or configuration.

Both weather and solar sidebar label inks now maximize black/white Oklab ΔL
against the mean RGB of a complete simulated 8×8 Bayer tile using the configured
panel calibration. This accounts for actual patterned fills, including sky.
