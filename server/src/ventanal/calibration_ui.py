# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Private, session-bound chart photo workflow and palette preview."""
from datetime import datetime
import fcntl
import hashlib
from io import BytesIO
import json
import logging
import os
import shutil
import time
from zoneinfo import ZoneInfo
import httpx
import numpy as np
from PIL import Image
from ventanal import calibrate
from ventanal import calibration_photo as imaging
from ventanal.auth import private_write
from ventanal.config import load_config
from ventanal.manifest import read_manifest, publish
from ventanal.palette import PANEL_INDEX
from ventanal.settings import ConfigWriteError, save_settings

log = logging.getLogger(__name__)

MAX_UPLOAD = 150 * 1024 * 1024
UPLOAD_HINT = 'Upload original DNG, CR2, CR3, NEF, ARW, RAF, ORF, RW2, HEIC/HEIF, JPEG, PNG or TIFF up to 150 MB. RAW gives the highest accuracy.'



class CalibrationFiles:
    def __init__(self, cache_dir, token):
        self.root = cache_dir/'calibration'
        self.directory = self.root/hashlib.sha256(token.encode()).hexdigest()

    def cleanup(self, now=None):
        now = time.time() if now is None else now
        if not self.root.exists(): return
        for path in self.root.iterdir():
            if path.is_dir() and not path.is_symlink() and now-self.upload_time(path) >= 86400:
                shutil.rmtree(path, ignore_errors=True)

    @staticmethod
    def upload_time(directory):
        try: return (directory/'photo.png').stat().st_mtime
        except FileNotFoundError: return directory.stat().st_mtime

    def discard(self):
        shutil.rmtree(self.directory, ignore_errors=True)

    def upload(self, data, filename):
        if len(data)>MAX_UPLOAD: raise ValueError(UPLOAD_HINT)
        pixels, metadata = imaging.decode(data,filename)
        photo = Image.fromarray(imaging.encode(pixels))
        self.cleanup()
        self.discard()
        self.directory.mkdir(parents=True, mode=0o700)
        self.root.chmod(0o700)
        # Stream directly into private storage; avoid a second full RAW buffer.
        descriptor=os.open(self.directory/'linear.npy',os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
        with os.fdopen(descriptor,'wb') as linear:
            np.save(linear,pixels,allow_pickle=False)
        private_write(self.directory/'source.json',json.dumps(metadata).encode())
        data=BytesIO(); photo.save(data,'PNG'); private_write(self.directory/'photo.png',data.getvalue())
        photo.thumbnail((1200,1200),Image.Resampling.LANCZOS)
        data=BytesIO(); photo.save(data,'PNG'); private_write(self.directory/'preview.png',data.getvalue())

    def valid(self):
        return self.directory.exists() and time.time()-self.upload_time(self.directory) < 86400

    def measure(self, values, cfg):
        if not self.valid(): raise ValueError('Photo expired. Upload the chart photo again.')
        if not (self.directory / 'linear.npy').is_file():
            raise ValueError('Please upload the photo again. This session predates linear photo storage.')
        with Image.open(self.directory/'preview.png') as small, Image.open(self.directory/'photo.png') as photo:
            pixels=np.load(self.directory/'linear.npy',allow_pickle=False,mmap_mode='r')
            metadata=json.loads((self.directory/'source.json').read_text())
            points=[tuple(map(float,pair.split(','))) for pair in values.get('corners','').split()]
            # The actual stored preview defines the coordinate system, never a forged size.
            corners=calibrate.map_corners(points,small.size,photo.size)
            samples=np.asarray(calibrate.measure(pixels,corners))
            raw=imaging.encode(samples).tolist()
            balanced=None
            if values.get('paper'):
                rect=tuple(map(float,values['paper'].split(',')))
                if len(rect)!=4 or not all(np.isfinite(v) for v in rect): raise ValueError('Invalid white-paper rectangle.')
                x,y,w,h=rect
                if min(x,y)<0 or min(w,h)<=0 or x+w>small.width or y+h>small.height: raise ValueError('Paper rectangle must be inside the preview.')
                # Rectangle uses pixel edges, corners use pixel centres.
                sx,sy=photo.width/small.width,photo.height/small.height
                left,top,right,bottom=map(round,(x*sx,y*sy,(x+w)*sx,(y+h)*sy))
                balanced=imaging.encode(imaging.balance(samples,imaging.paper_mean(pixels,(left,top,right-left,bottom-top)))).tolist()
        render_comparison(cfg,balanced or raw,self.directory)
        private_write(self.directory/'measurement.json',json.dumps({'raw':raw,'balanced':balanced,'raw_photo':metadata['raw'], 'paper_used':balanced is not None,'profile':metadata['profile'],'note':metadata['note']}).encode())


def render_comparison(cfg, colours, directory):
    """Re-render the current artwork from its source through both palettes."""
    from ventanal.art.pool import ArtPool
    from ventanal.art.sources import fetch_image, USER_AGENT
    from ventanal.art.render import render_art
    from ventanal.quantize import quantize, simulate
    entry = current_painting(cfg)
    credit = entry.get('credit') or {}
    if 'source' not in credit: return
    candidate = ArtPool(cfg.cache_dir/'art.sqlite3').get(credit['source'],credit['object_id'])
    if candidate is None: raise ValueError('Current painting is unavailable for preview. Try again after the next art slot.')
    with httpx.Client(timeout=60,follow_redirects=True,headers={'User-Agent':USER_AGENT}) as client:
        image=fetch_image(candidate.image_url,client,cfg.cache_dir)
    canvas,_=render_art(image,candidate)
    for name,palette in [('before',cfg.panel_rgb),('after',colours)]:
        picture=simulate(quantize(canvas,palette),palette)
        picture.thumbnail((800,600))
        data=BytesIO(); picture.save(data,'PNG'); private_write(directory/(name+'.png'),data.getvalue())


def current_painting(cfg):
    entry=read_manifest(cfg.output_dir).get('screens',{}).get('art',{})
    if (entry.get('credit') or {}).get('title')=='Calibration chart' and 'source' not in (entry.get('credit') or {}):
        try: return json.loads((cfg.cache_dir/'calibration-art.json').read_text())
        except (OSError,ValueError): pass
    return entry


def show_chart(cfg):
    from ventanal.art.render import ART_READOUT
    from ventanal.frame import encode_frame
    from ventanal.quantize import simulate
    with (cfg.cache_dir/'tick.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        entry=current_painting(cfg)
        if 'source' not in (entry.get('credit') or {}): raise ValueError('Publish a painting before showing the chart.')
        private_write(cfg.cache_dir/'calibration-art.json',json.dumps(entry).encode())
        indices=calibrate.chart()
        publish(cfg.output_dir,'art',encode_frame(indices),ART_READOUT,
                {'title':'Calibration chart','artist':'Ventanal','date':'','museum':''},simulate(indices,cfg.panel_rgb),datetime.now(ZoneInfo(cfg.timezone)))


class MeasurementError(ValueError):
    pass


def read_measurement(files):
    try:
        values=json.loads((files.directory/'measurement.json').read_text())
        for name in ('raw', 'balanced'):
            palette=values[name]
            if name=='balanced' and palette is None: continue
            if len(palette)!=6: raise ValueError('invalid palette')
            for rgb in palette:
                if len(rgb)!=3 or any(type(v) is not int or not 0<=v<=255 for v in rgb):
                    raise ValueError('invalid colour')
        return values
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise MeasurementError('Unable to read calibration measurement. Measure the photo again.') from exc


def apply(ui, files):
    if not files.valid(): raise ValueError('Photo expired. Upload the chart photo again.')
    if not (files.directory/'measurement.json').exists():
        raise ValueError('Measure and preview the photo before applying.')
    measurement=read_measurement(files)
    colours=measurement['balanced'] or measurement['raw']
    def mutate(doc):
        for key,rgb in zip(PANEL_INDEX,colours): doc['panel'][key]='#%02x%02x%02x'%tuple(rgb)
    save_settings(ui.config_path,{},mutate=mutate)
    files.discard()
    ui.start_render()


def body(ui, token, error='', skip_measurement=False):
    from ventanal.ui import e, form
    from ventanal.status import next_wake
    cfg=load_config(ui.config_path)
    files=CalibrationFiles(cfg.cache_dir,token)
    board=ui.store.latest()
    wake=next_wake(cfg,datetime.now(ZoneInfo(cfg.timezone)),bool(board and board.get('bat',100)<=10))
    result=f'<div class="form-card"><h2>1 · Show the colour chart</h2><p>The chart appears at the board’s next scheduled update: {e(wake.strftime("%a %d %b %H:%M %Z"))}. The board must be on the art screen; single press WAKE if needed.</p><div class="actions">'+form('/calibration/chart',token,'','Show the colour chart')+form('/calibration/restore',token,'','Restore the painting')+'</div></div>'
    if ui.render_result: result+='<p class="notice">'+e(ui.render_result)+'</p>'
    if error: result+='<p class="error" role="alert">'+e(error)+'</p>'
    result+=f'<div class="form-card"><h2>2 · Upload a chart photo</h2><p id="upload-hint">{e(UPLOAD_HINT)} EXIF orientation is normalised on upload; all selections refer to the rotated photo.</p><form id="photo-upload" method="post" action="/calibration/upload" enctype="multipart/form-data" data-csrf="{e(token)}"><label>Chart photo<input type="file" name="photo" accept=".dng,.cr2,.cr3,.nef,.arw,.raf,.orf,.rw2,.heic,.heif,.jpg,.jpeg,.png,.tif,.tiff" required></label><button class="btn" disabled>Upload photo</button><p id="upload-status" role="status"></p><noscript>Enable JavaScript to upload a photo.</noscript></form><small>Photos stay in private state storage and are deleted on Apply, Cancel, or after 24 hours.</small></div><script src="/static/calibration.js?v=2" defer></script>'
    result += '<details class="form-card"><summary>How to photograph the chart accurately</summary><ul><li>iPhone: Settings → Camera → Formats → Apple ProRAW; then shoot with RAW on.</li><li>Pixel: Camera → Settings → Advanced → RAW/JPEG control.</li><li>Samsung Galaxy: Pro mode with RAW enabled, or the Expert RAW app.</li><li>Other Android: Pro/Manual mode RAW/DNG.</li><li>Dedicated cameras: shoot RAW.</li></ul><p>Use indirect daylight, no glare, fill the frame, and include a sheet of plain white paper.</p><p>Send the original file via AirDrop/Files/Google Drive “original quality”; avoid messaging apps that recompress.</p></details>'
    if not files.valid(): return result
    result+=f'<div class="form-card"><h2>3 · Pick corners and optional white paper</h2><p>Click the four outer corners in order TL, TR, BR, BL. The corner squares may be partly hidden by the bezel — click where the chart’s corner would be.</p><p>For optional white balance, select White paper, then drag over a sheet of plain white paper. Samples and the paper mean are measured in linear light. Per-channel scaling maps the paper to sRGB 243.</p><div class="actions"><button class="btn" type="button" id="corner-mode">Chart corners</button><button class="btn" type="button" id="paper-mode">White paper</button><button class="btn" type="button" id="undo">Undo</button><button class="btn" type="button" id="reset">Reset selections</button></div><p id="selection-status" aria-live="polite">Select corner 1: TL</p><canvas id="chart-picker" aria-label="Chart photo: choose four corners; arrow keys move the cursor, Enter selects" tabindex="0" data-photo="/calibration/photo"></canvas><form id="measure-form" method="post" action="/calibration/measure"><input type="hidden" name="csrf" value="{e(token)}"><input type="hidden" name="corners" id="corners"><input type="hidden" name="paper" id="paper"><button class="btn" id="measure" disabled>Preview measured colours</button></form>'+form('/calibration/cancel',token,'','Cancel')+'</div>'
    measurement=files.directory/'measurement.json'
    if measurement.exists() and not skip_measurement:
        values=read_measurement(files)
        tier=imaging.RAW_TIER if values.get('raw_photo') else imaging.PROCESSED_TIER
        reference='White paper reference used.' if values.get('paper_used') else 'White paper reference not used.'
        result+=f'<p class="notice">{e(tier)}<br>{e(reference)} {e(values.get("profile",""))}. {e(values.get("note",""))}</p>'
        result+='<div class="form-card"><h2>4 · Preview palette</h2><p>Review the measured swatches and simulated art before applying.</p><div class="table-scroll"><table><thead><tr><th>Ink</th><th>Current palette</th><th>Raw</th>'+('<th>Balanced</th>' if values['balanced'] else '')+'</tr></thead><tbody>'
        for i,key in enumerate(PANEL_INDEX):
            result+='<tr><th>'+e(key.title())+'</th>'
            for label,palette in [('Current palette',cfg.panel_rgb),('Raw',values['raw'])]+([('Balanced',values['balanced'])] if values['balanced'] else []):
                colour='#%02x%02x%02x'%tuple(palette[i])
                # SVG is a private endpoint, avoiding inline CSS under the CSP.
                result+=f'<td data-label="{label}"><img class="swatch" src="/calibration/swatch/{colour[1:]}" alt="">{colour}</td>'
            result+='</tr>'
        result+='</tbody></table></div><div class="previews">'
        for name,label in [('before','Current palette · simulated'),('after','New palette · simulated')]:
            if (files.directory/(name+'.png')).exists(): result+=f'<div class="pv"><div class="lbl">{label}</div><img src="/calibration/{name}" alt="{label}"></div>'
        if not (files.directory/'after.png').exists(): result+='<p>No current painting published to preview.</p>'
        result+='</div><p>Apply saves the panel palette with a backup and republishes art, all reserved spares and weather in the background.</p><div class="actions">'+form('/calibration/apply',token,'','Apply')+form('/calibration/cancel',token,'','Cancel')+'</div></div>'
    return result


def _handle(ui, h, path, post, values, token, send, redirect):
    cfg=load_config(ui.config_path)
    files=CalibrationFiles(cfg.cache_dir,token)
    if not post and path.startswith('/calibration/swatch/'):
        import re
        value=path.removeprefix('/calibration/swatch/')
        if not re.fullmatch('[0-9a-f]{6}',value): ui.respond(h,404); return
        ui.respond(h,200,f'<svg xmlns="http://www.w3.org/2000/svg" width="56" height="32"><rect width="56" height="32" fill="#{value}"/></svg>','image/svg+xml'); return
    pictures={'/calibration/photo':'preview.png','/calibration/before':'before.png','/calibration/after':'after.png'}
    if path in pictures:
        if post or not files.valid(): ui.respond(h,404); return
        try: data=(files.directory/pictures[path]).read_bytes()
        except OSError: ui.respond(h,404); return
        ui.respond(h,200,data,'image/png'); return
    if path=='/calibration' and not post: send(200,'Calibration',body(ui,token)); return
    actions=('/calibration/upload','/calibration/chart','/calibration/restore','/calibration/measure','/calibration/apply','/calibration/cancel')
    if path not in actions: ui.respond(h,404); return
    if not post: ui.respond(h,405); return
    if path.endswith('/upload'):
        files.upload(values['photo'],values['filename'])
    elif path.endswith('/chart'):
        show_chart(cfg)
        redirect('/calibration'); return
    elif path.endswith('/restore'):
        ui.start_render()
        redirect('/calibration'); return
    elif path.endswith('/measure'):
        files.measure(values,cfg)
    elif path.endswith('/apply'):
        apply(ui,files)
        redirect('/calibration'); return
    elif path.endswith('/cancel'):
        files.discard()
        redirect('/calibration'); return
    send(200,'Calibration',body(ui,token))


def handle(ui, h, path, post, values, token, send, redirect):
    try:
        _handle(ui,h,path,post,values,token,send,redirect)
    except ConfigWriteError as exc:
        send(400,'Calibration',body(ui,token,str(exc),skip_measurement=True))
    except Exception as exc:
        if isinstance(exc, (ValueError, KeyError, TypeError)) and not isinstance(exc, MeasurementError):
            message = str(exc) if isinstance(exc,ValueError) and str(exc).startswith(('Upload ','Please upload the photo again','Photo expired','Select four','corners ','paper ','Paper rectangle','Invalid white','Current painting','Publish a painting','Measure and preview')) else 'Unable to process the photo. Check the corners and optional paper selection.'
        else:
            log.exception('Calibration failed during %s', path)
            ui.store.event('calibration error', 'Calibration failed; see service log for diagnostics.')
            message = str(exc) if isinstance(exc, MeasurementError) else 'Unable to complete calibration. Check the photo or try again after the next tick.'
        send(400,'Calibration',body(ui,token,message,skip_measurement=True))
