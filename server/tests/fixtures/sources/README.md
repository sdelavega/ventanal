# Source fixtures (retrieved 2026-11-16)

Small unmodified official response subsets, except where tests explicitly mutate a field.
All source code and tests use Ventanal's identifying User-Agent.

- `rijks-object.json`: https://data.rijksmuseum.nl/200108396?_profile=la-framed
- `rijks-visual.json`: https://data.rijksmuseum.nl/202108396?_profile=la-framed
- `rijks-digital.json`: https://data.rijksmuseum.nl/500711199912110510799100?_profile=la-framed
  (a separate published example digital object, used to exercise link traversal).
  Image rights: **visual** `subject_to[].classified_as[].id`, PD mark / CC0 only.
  Metadata `subject_of` CC0 does not grant image rights. Terms/documentation:
  https://data.rijksmuseum.nl/docs/search/ and https://data.rijksmuseum.nl/tutorials/iiif/.
- `smk.json`: https://api.smk.dk/api/v1/art/search/?keys=winter&rows=3&filters=%5Bpublic_domain:true%5D,%5Bhas_image:true%5D
  First two items. `public_domain: true`, `rights` PD mark / CC0, `has_image`.
  Official OpenAPI: https://api.smk.dk/api/v1/docs/swagger-ui-init.js
  Terms: https://www.smk.dk/article/smk-api/
- `nga-objects.csv`, `nga-images.csv`: seasonal subsets of
  https://raw.githubusercontent.com/NationalGalleryOfArt/opendata/main/data/objects.csv
  and https://raw.githubusercontent.com/NationalGalleryOfArt/opendata/main/data/published_images.csv.
  Join `objectid` to `depictstmsobjectid`; images require `openaccess=1`.
  Definition: https://github.com/NationalGalleryOfArt/opendata/blob/main/documentation/Data%20Dictionary.txt
  Dataset licence CC0; image rights are separate and checked via `openaccess`.
- `commons.json`: https://commons.wikimedia.org/w/api.php with `action=query`,
  `generator=search`, `gsrnamespace=6`, `gsrsearch=winter filetype:bitmap painting`,
  `gsrlimit=6`, `prop=imageinfo`, `iiprop=url|size|extmetadata`, `iiurlwidth=2400`, `maxlag=5`.
  Includes CC BY-SA rejection and CC0/PD-Art examples. Only file-page `extmetadata`
  PD-Art/PD-old/CC0 plus painting categories admitted.
  https://commons.wikimedia.org/wiki/Commons:Reuse_of_PD-Art_photographs
  https://www.mediawiki.org/wiki/API:Imageinfo

Paris Musées: no recorded live fixture is claimed. The official explorer and
GraphQL endpoint returned HTTP 503, and no owner token is available. Its proposed
schema fields need verification before enabling the adapter; see task report.
