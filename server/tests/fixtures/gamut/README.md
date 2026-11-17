# Fixed gamut evaluation sources

512px maximum-dimension RGB PNG derivatives downloaded 2026-11-16. The evaluation
uses identical 400×300 compose() inputs for every kernel, with the calibrated
palette specified in task-gamut-brief.md. Source | before | after previews are
800×200. Regenerate with `scripts/gamut_metrics.py --download` from server.

Cleveland records were selected through the museum's open-access API with
`cc0=1&has_image=1`; all four are public-domain/CC0 open-access images.

| Fixture | Work | Source image |
| --- | --- | --- |
| nativity.png | Cleveland 106344, Miniature Excised from a Psalter: The Nativity | https://openaccess-cdn.clevelandart.org/1924.427/1924.427_print.jpg |
| kerchief.png | Cleveland 135382, The Red Kerchief | https://openaccess-cdn.clevelandart.org/1958.39/1958.39_web.jpg |
| beach.png | Cleveland 125091, The Beach at Deauville | https://openaccess-cdn.clevelandart.org/1946.71/1946.71_web.jpg |
| portrait.png | Cleveland 124089, Portrait of a Woman | https://openaccess-cdn.clevelandart.org/1944.90/1944.90_web.jpg |
| hunters.png | Hunters in the Snow | Existing repository docs/mockups/hunters.jpg |

The pinned before-kernel is quantize.py at ca28bcdb01663cde613f6526d33e61cd6857e966.
`gamut_metrics.py` loads only that public source path from Git; it never loads
production configuration. Index mixtures are ordered black, white, yellow,
red, blue, green. Hue error uses circular angle distance in degrees; the source
orange mask is computed before blur, then both inputs are blurred at sigma=2px.
