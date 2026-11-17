# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Fixed gamut evaluation: run from server with .venv/bin/python scripts/gamut_metrics.py.
Fixtures are downscaled CC0 Cleveland images and the existing Hunters source.
"""
import argparse
import json
import inspect
from pathlib import Path
import subprocess
import time
import types

import httpx
import numpy as np
from PIL import Image, ImageFilter

from task6_quantize_metrics import viewing_error, CANDIDATE
from ventanal.art.compose import compose
from ventanal.art.render import render_art
from ventanal.palette import to_oklab
import ventanal.quantize as q

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / 'server/tests/fixtures/gamut'
BEFORE = 'ca28bcdb01663cde613f6526d33e61cd6857e966'
PANEL = np.array([[42,39,46], [193,197,179], [214,197,71],
                  [101,41,34], [60,91,145], [86,115,89]])
SOURCES = {
    'nativity': ('Miniature Excised from a Psalter: The Nativity',
                 'https://openaccess-cdn.clevelandart.org/1924.427/1924.427_print.jpg'),
    'kerchief': ('The Red Kerchief',
                 'https://openaccess-cdn.clevelandart.org/1958.39/1958.39_web.jpg'),
    'beach': ('The Beach at Deauville',
              'https://openaccess-cdn.clevelandart.org/1946.71/1946.71_web.jpg'),
    'portrait': ('Portrait of a Woman',
                 'https://openaccess-cdn.clevelandart.org/1944.90/1944.90_web.jpg'),
    'hunters': ('Hunters in the Snow', 'docs/mockups/hunters.jpg'),
}


def baseline(commit=BEFORE):
    source = subprocess.check_output(['git', 'show', f'{commit}:server/src/ventanal/quantize.py'], cwd=ROOT)
    module = types.ModuleType('gamut_before')
    exec(compile(source, '<gamut-before>', 'exec'), module.__dict__)
    return module


def hue_error(source, output):
    rgb = np.asarray(source, dtype=int)
    r, g, b = rgb.transpose(2, 0, 1)
    mask = (r > 150) & (r-g > 40) & (g-b > 40)
    labs = [to_oklab(np.asarray(im.filter(ImageFilter.GaussianBlur(2))))
            for im in (source, output)]
    angles = [np.arctan2(lab[..., 2], lab[..., 1]) for lab in labs]
    delta = np.abs(np.angle(np.exp(1j*(angles[1]-angles[0]))))
    return mask, float(np.degrees(delta[mask]).mean()) if mask.any() else None


def linear_experiment():
    """Compare linear error accounting, retaining production Oklab selection.

    Change only the accounting-space decode in a copy of the kernel. This is
    evaluation code; production diffusion and non-DIFFUSE paths stay intact.
    """
    kernel = getattr(q._diffuse, 'py_func', q._diffuse)
    source = inspect.getsource(kernel)
    decode = 'linear[ch] = v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4'
    assert decode in source
    namespace = {'np': np}
    exec(compile(source.replace(decode, 'linear[ch] = v'), '<linear-experiment>', 'exec'), namespace)
    diffuse = namespace['_diffuse']
    if q.njit is not None:
        diffuse = q.njit(diffuse)
    for name in SOURCES:
        image = compose(Image.open(FIXTURES/f'{name}.png'), (400,300))
        lab = to_oklab(np.asarray(image))
        palette = to_oklab(PANEL)
        work = q._to_linear(q._diffuse_targets(lab, palette)) * 255
        indices = diffuse(work, np.zeros((300,400),dtype=np.uint8),
                          q._to_linear(palette)*255, palette,
                          np.zeros((300,400),dtype=np.uint8))
        output = q.simulate(indices,PANEL)
        print(name, 'linear error delta_e', viewing_error(image,output),
              'orange hue degrees', hue_error(image,output)[1])


def evaluate(download=False):
    before = baseline()
    previous = baseline("390a16d")
    # Historical experiments stay pinned outside the production path.
    cusp = baseline('ad33145')
    cusp._diffuse_targets = cusp._gamut_map
    kernels = [('before', before), ('previous', previous), ('cusp', cusp), ('after', q)]
    dest = ROOT / '.superpowers/sdd/2026-11-15-ventanal'
    results = {}
    for name, (title, url) in SOURCES.items():
        path = FIXTURES / f'{name}.png'
        if download:
            if url.startswith('https:'):
                from io import BytesIO
                response = httpx.get(url, timeout=90, follow_redirects=True)
                response.raise_for_status()
                image = Image.open(BytesIO(response.content)).convert('RGB')
            else:
                image = Image.open(ROOT / url).convert('RGB')
            image.thumbnail((512, 512), Image.Resampling.LANCZOS)
            image.save(path)
        source = compose(Image.open(path), (400,300))
        canvas = q.Canvas(source.size, method=q.Method.DIFFUSE)
        canvas.paste(source, (0,0), q.Method.DIFFUSE)
        outputs = []
        result = {'title':title, 'url':url}
        for label, quantizer in kernels:
            indices = quantizer.quantize(canvas, PANEL)
            output = q.simulate(indices, PANEL)
            mask, hue = hue_error(source, output)
            result[label] = {'delta_e':viewing_error(source,output), 'orange_hue_degrees':hue,
                             'orange_pixels':int(mask.sum()),
                             'orange_mix':(np.bincount(indices[mask],minlength=6)/max(1,mask.sum())).tolist()}
            r, g, b = np.asarray(source, dtype=int).transpose(2, 0, 1)
            gold = (r > 150) & (g > 110) & (b < 90) & (r-g < 60)
            result[label]['gold_pixels'] = int(gold.sum())
            result[label]['gold_mix'] = (np.bincount(indices[gold], minlength=6)/max(1,gold.sum())).tolist()
            if label in ('before', 'after'):
                outputs.append(output)
        preview = Image.new('RGB',(1200,300))
        for x, image in zip((0,400,800),[source,*outputs]): preview.paste(image,(x,0))
        preview.resize((800,200),Image.Resampling.LANCZOS).save(dest/f'gamut-{name}-v2.png')
        results[name] = result
    patch = q.Canvas((192,192),(211,120,64),q.Method.DIFFUSE)
    results['orange_patch'] = {}
    for label, quantizer in kernels:
        indices = quantizer.quantize(patch,PANEL)
        results['orange_patch'][label] = (np.bincount(indices.ravel(),minlength=6)/indices.size).tolist()
    patch = q.Canvas((192,192),(205,160,60),q.Method.DIFFUSE)
    results['gold_patch'] = {}
    for label, quantizer in kernels:
        indices = quantizer.quantize(patch,PANEL)
        results['gold_patch'][label] = (np.bincount(indices.ravel(),minlength=6)/indices.size).tolist()
    print(json.dumps(results,indent=2))
    (dest/'gamut-metrics-v2.json').write_text(json.dumps(results,indent=2)+'\n')


def timing(before_path, rounds):
    # Allows an isolated Pi run without Git or reading production settings.
    module = types.ModuleType('gamut_before')
    exec(compile(Path(before_path).read_bytes(), '<gamut-before>', 'exec'), module.__dict__)
    image = Image.open(ROOT/'docs/mockups/hunters.jpg').convert('RGB')
    for label, quantizer in [('before',module),('after',q)]:
        times=[]
        for _ in range(rounds):
            start=time.perf_counter()
            canvas,_ = render_art(image, CANDIDATE)
            quantizer.quantize(canvas,PANEL)
            times.append(time.perf_counter()-start)
        print(label, times, 'median',float(np.median(times)),flush=True)


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--download',action='store_true')
    parser.add_argument('--linear-experiment',action='store_true')
    parser.add_argument('--timing-before',type=Path)
    parser.add_argument('--rounds',type=int,default=3)
    args=parser.parse_args()
    if args.linear_experiment: linear_experiment()
    elif args.timing_before: timing(args.timing_before,args.rounds)
    else: evaluate(args.download)
