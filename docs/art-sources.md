# Art source plugins

Ventanal's museum IDs remain in `[art] museums`. Updating the package preserves
that list. The registry supplies Settings labels, descriptions, legal notices,
and credential inputs. Commons and Paris Musées are disabled by default;
Rijksmuseum no longer uses an API key. Existing `rijksmuseum_key` values are
accepted for migration, hidden from Settings, and never used for requests.

Adapters live in `server/src/ventanal/art/sources/`, one module per collection.
Add the adapter to `PLUGINS` in the package registry. Each implements `Source`:
`id`, display `name`, `description`, `default_enabled`, declared `credentials`,
`rate_limit` (requests/second), `search(season, client, limit)`,
`fetch_image(candidate, client)`, `attribution(candidate)`, and `rights_filter`.
`BaseSource` supplies season-term iteration, caching, image retrieval and credits.
Sources can implement a custom search for REST, GraphQL, IIIF, SPARQL or bulk data.
The original museum classes and `search(term, limit, client)` calls remain
compatible. Candidate identity remains `source:object_id`, including in history,
spares and ignored lists. SMK uses its numeric collection record IDs, because
accession numbers sometimes contain slashes.

Only explicitly public-domain/CC0 works are admitted. Rijksmuseum checks the
visual item's rights rather than the licence on its metadata. NGA joins the
official CSV exports and requires each image's `openaccess=1`. SMK requires
`public_domain`, an open `rights` URI and an image. Commons checks imageinfo's
file-page licence metadata and painting/PD-Art/PD-old/CC0 categories; it never
infers copyright solely from a work's age. It remains opt-in, with its legal
notice in Settings.

The shared HTTP helper identifies Ventanal, paces by host, retries transient
failures up to three attempts with bounded backoff/Retry-After, and isolates
source failures with fixed log messages. Credential-bearing requests do not
follow redirects. Legacy image-cache calls retain their original single-request
failure behavior. IIIF requests fit within 2400 pixels (hard cap 3400); source
metadata provides size/orientation hints to the existing pool selector.

NGA creates a compact open-image index under `cache_dir/sources/nga-index-v2.json`,
refreshed weekly during a pool refresh. It does not redownload CSVs per tick or
per search term. Failed downloads never replace a valid index. The current and
next season use the existing daily pool refresh and no-repeat logic. New source
selection takes effect on the next pool refresh; the deployment explicitly made
that refresh due under the tick lock.

Credential fields use password inputs, remain empty in returned HTML even after
validation errors, and accept blank to keep / an explicit clear checkbox to
remove. Credentials are stored as strings under `[art.credentials.<source>]`.
For example:

```toml
[art.credentials.paris]
token = "" # Set in Settings; do not put a real token in version control.
```

Paris Musées is pending live schema verification: its official API/explorer
returned HTTP 503 during implementation, and no owner token was available. The
adapter's provisional `fieldOeuvreLibre` and media fields must be checked against
the live explorer before enabling it. Its filter fails closed for missing rights
or HD images. No successful live Paris search or recorded Paris response is
claimed. The other four new sources passed live searches and image retrieval.

Request a free token at <https://apicollections.parismusees.paris.fr/>: create an
account, follow the confirmation email, open **Mon compte → Modifier → Auth
Tokens → Add new auth token**, create the application token, and save. Enter it
in Settings after the API is available and the schema has been verified. The
public API allows 1000 requests/day; keep it disabled until then.

Official references:

- Rijksmuseum [search](https://data.rijksmuseum.nl/docs/search/),
  [Linked Art](https://data.rijksmuseum.nl/docs/http/),
  [IIIF](https://data.rijksmuseum.nl/tutorials/iiif/).
- NGA [dataset](https://github.com/NationalGalleryOfArt/opendata),
  [image permission field](https://github.com/NationalGalleryOfArt/opendata/blob/main/documentation/Data%20Dictionary.txt).
- SMK [OpenAPI](https://api.smk.dk/api/v1/docs/),
  [terms](https://www.smk.dk/article/smk-api/).
- Paris Musées [API portal](https://apicollections.parismusees.paris.fr/),
  [data structure](https://www.parismuseescollections.paris.fr/fr/structure-des-donnees),
  [terms](https://apicollections.parismusees.paris.fr/cgu).
- Commons [imageinfo](https://www.mediawiki.org/wiki/API:Imageinfo),
  [PD-Art](https://commons.wikimedia.org/wiki/Commons:Reuse_of_PD-Art_photographs),
  [User-Agent policy](https://foundation.wikimedia.org/wiki/Policy:Wikimedia_Foundation_User-Agent_Policy).

Personal library integrations belong to v1.1; no cloud account connections are
included here.

## Colour selection (default)

`[art] include_monotone = false` selects colour works for the six-colour panel.
Settings exposes **Include monotone works (black-and-white or single-colour prints, drawings and photographs)**;
checking it permits monotone artwork. Existing installations default to false
without rewriting their configuration. Rights admission remains mandatory under
both settings.

Every candidate carries `kind`: `painting`, `drawing`, `print`, `photograph`,
`manuscript/illumination`, or `other`. Classification uses source metadata, never
the title or artist:

| Source | Metadata used |
| --- | --- |
| Met | `classification`, `objectName`, `medium` |
| AIC | `classification_title`, `artwork_type_title`, `medium_display` |
| Cleveland | `type`, `technique` |
| Rijksmuseum | Linked Art `classified_as` and `made_of` (including English `notation` values) |
| NGA | objects CSV `classification`, `medium`, retained in the versioned weekly index |
| SMK | `object_names`, `techniques` (including Danish technique names) |
| Commons | file `extmetadata.ObjectType`, `Medium`, `Categories`; this classifies the depicted painting, rather than its digital reproduction |
| Paris Musées | `other` pending verification of its type metadata and schema; adapter remains disabled without a token |

The classifier recognises English and common Dutch, Danish and French type and
technique terms. Illumination/miniature terms precede print, photograph, drawing
and painting terms; unknown metadata becomes `other`. Legacy database rows also
start as `other`, and get a source-derived kind when metadata is refreshed.

Metadata-confirmed paintings and manuscript/illumination candidates are admitted
without the image heuristic, preserving muted paintings with a narrow palette.
Prints, drawings, photographs and unknown kinds must pass the decoded-image check.
Before dithering, credits or panel simulation, sample at most 256×256 pixels,
compose transparency onto white and convert sRGB to Oklab. Require at least 8%
of pixels above chroma 0.02, and at least 3% of chroma weight outside the most
populated circular 60° hue band (72 bins, including hue wraparound). Single-ink
sepia, bistre, blue and other tinted images fail even with substantial chroma.
This is a heuristic, calibrated on 25 visually labelled museum images (8 colour,
17 monotone): 8/8 colour admitted and 17/17 monotone rejected. This is in-sample
calibration evidence, not a general accuracy guarantee. Synthetic single-hue
red, blue, sepia, cyanotype-like and bistre gradients are also regression checked.

Selection checks main art and spares, including reserved spares on a normal
tick. Rejected candidates remain in SQLite with their history and season
memberships. Colour admission scores are cached per image URL and invalidated when
that URL, kind or the algorithm version changes; image failures are retried on later selections, never counted
as monotone. Forced re-renders reject a monotone reserved identity under the
default rather than silently changing that identity. An already published main
frame changes at the next normal art selection. A reserved spare whose replacement
cannot be fetched retains its previous published frame until refill succeeds.

Source failures retain traceback frames through sanitized `exc_info`: the
original exception message, chained exception bodies, URLs and request headers
are not logged. Image fetches require an explicit cache directory; they never
write a CWD-relative cache.

## Screen fitness

`[art] screen_fit_only = false` is opt-in. Settings calls it “Only show art
that suits this screen”. The Art page reads cached scores for main art, spares
and skipped works; pending scores are labelled explicitly. HTTP requests never
render or download images for scoring.

The score uses the configured palette, with no panel-specific RGB constants.
Compose at 400×300 using the art layout (including scaled background blur), run
the production DIFFUSE quantizer, and simulate the actual inks. Both source and
simulation receive a 2 px Gaussian viewing blur before mean Oklab distance.
The score is `100 * (1 - loss)`, where loss is:

- 65% fidelity: mean Oklab distance divided by 0.20, clipped to one.
- 25% gamut: chroma-weighted outside-hull share multiplied by distance outside
  the production mapper's linear-light hull, normalized by 0.25 and clipped.
- 10% lost tone: mean lightness excursion below panel black or above panel
  white, divided by 0.20 and clipped. This includes both affected share and depth.

The fixed threshold is **88**. Calibration used 30 stratified live-pool artworks
from seven available sources and agent visual labels (good/mixed/poor).
The 65/25/10 weights ranked those labels slightly better than three nearby
weight combinations; this is in-sample heuristic calibration, not an independent
accuracy estimate. With the calibrated panel, 129/289 colour-eligible production
works passed (44.6%); including the disabled Commons pool, 138/322 passed (42.9%).
Paris has no verified live pool and was not represented. The owner should judge
the ignored scratch contact sheet before enabling the setting.

SQLite stores score and a fingerprint of algorithm version plus all configured
ink values. Changed image URLs invalidate the cache; changed palettes become
pending immediately. Each tick gives a separate scoring worker at most eight
works and twenty seconds, including downloads and optional JIT compilation. A
worker timeout preserves completed scores. Consecutive failures persist per
artwork with exponential retry cooldowns (five minutes, doubling to a 24-hour
cap). Cooldown is committed before work starts, so a killed worker also backs
off. Success or a changed image URL resets it. Cooling-down rows consume no
attempts; failures emit source-level warnings with sanitized error classes and
no upstream exception text or stack source lines. The optional `accel`
extra accelerates the same production kernel; scoring workers retain its compile
cache under the service cache directory. The foreground tick waits only for this
bounded batch, while the independent HTTP service reads cached results.

The filter preserves ignored works, source enablement, monotone admission and
explicit reservations. It first applies seasonal, size and no-repeat preferences.
Among otherwise eligible seasonal candidates,
passing scores retain the existing weighted selection. If none pass, selection
logs a fallback and chooses the highest current score available. If scoring has
not completed for any eligible candidate, the existing eligible selection acts
as a separately logged unscored provisional selection. If strict spare selection
has no seasonal candidates, it logs that condition and uses the same scored
selection over remaining pool entries, relaxing seasonal/size/no-repeat
preferences while retaining explicit exclusions. Main art and spares, including forced re-renders,
use this policy. Existing fallback spares are retained when a proposed replacement
would score worse. Enabling the filter does not change the owner's source list,
palette, authentication or transport security.
