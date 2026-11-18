# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Small server-rendered LAN interface, shared by the frame HTTP server."""
from datetime import datetime, timezone
from html import escape
from http.cookies import SimpleCookie, CookieError
import hmac
import json
import logging
import sqlite3
from pathlib import Path
import subprocess
import sys
from threading import Event, RLock, Thread
import time
import re
from urllib.parse import parse_qs
from zoneinfo import ZoneInfo
import httpx
import tomlkit
from ventanal.auth import Auth, PASSWORD_MAX_LENGTH, PASSWORD_MIN_LENGTH, validate_password
from ventanal.config import load_config, ConfigError
from ventanal.manifest import read_manifest
from ventanal.security import PreviousCertificateHeld
from ventanal.settings import ConfigWriteError, FIELDS, SOURCE_REGISTRY, MUSEUMS, MUSEUM_LABELS, FIELD_HELP, OPTION_LABELS, READ_ONLY, ADVANCED, season_count, save_settings, read_document
from ventanal.status import BoardStore, next_wake, showing, preview
from ventanal.ota import FirmwareStore, OPEN_DEVICE, poll_github, verified_metadata
from ventanal.release import BOARDS, ReleaseError


def e(value): return escape(str(value), quote=True)


def form(action, csrf, body, button):
    return f'<form method="post" action="{e(action)}"><input type="hidden" name="csrf" value="{e(csrf)}">{body}<button class="btn" type="submit">{e(button)}</button></form>'


def password_fields(change=False):
    current = '<label>Current password<input type="password" name="current" autocomplete="current-password" required></label>' if change else ''
    limits = f'minlength="{PASSWORD_MIN_LENGTH}" maxlength="{PASSWORD_MAX_LENGTH}" required'
    return current + f'<label>Password<input type="password" name="password" autocomplete="new-password" {limits}></label><label>Confirm password<input type="password" name="confirm" autocomplete="new-password" {limits}></label>'


FIRMWARE_ACTIONS = frozenset(('/updates/check', '/updates/install', '/updates/automatic', '/updates/upload'))


SETUP_TOKEN_FIELD = '<label>Setup token<input name="setup_token" type="password" autocomplete="off" required></label>'


class UI:
    fcc_refreshing = False
    fcc_result = ""
    def __init__(self, config_path, cache_dir, rerender=None):
        self.config_path = config_path
        self.auth = Auth(cache_dir)
        from ventanal.security import Security
        self.security = Security(cache_dir)
        self.fcc_refreshing = False
        self.fcc_result = ''
        self.store = BoardStore(cache_dir)
        self.firmware = FirmwareStore(cache_dir)
        self.firmware_checking = False
        # Backfill roles for frames published before this feature was installed.
        cfg = load_config(config_path)
        manifest = read_manifest(cfg.output_dir)
        for role, entry in manifest.get('screens', {}).items():
            self.store.role(entry['id'], role)
        for i, entry in enumerate(manifest.get('spares', [])):
            self.store.role(entry['id'], f'spare{i}')
        self.rerender = rerender or self._rerender
        self.lock = RLock()
        # Photo workflows serialize separately; fetch/quantize must not block pages.
        self.calibration_lock = RLock()
        self.render_result = ''
        self.rendering = False
        self.render_pending = False
        self.render_effects = set()
        self.active_effects = {'art', 'weather', 'spares'}
        self.render_delay = 3
        self.render_deadline = 0
        self.cleanup_stop = Event()
        def cleanup():
            from ventanal.calibration_ui import CalibrationFiles
            files = CalibrationFiles(cache_dir, '')
            while not self.cleanup_stop.is_set():
                with self.calibration_lock:
                    files.cleanup()
                if self.cleanup_stop.wait(60):
                    break
                self.start_firmware_check(force=False)
        Thread(target=cleanup, daemon=True).start()

    def firmware_device(self):
        if self.auth.mode == 'password':
            return self.security.pairing_id
        if self.auth.mode == 'none':
            return OPEN_DEVICE
        return None

    def start_firmware_check(self, *, force):
        with self.lock:
            device = self.firmware_device()
            if device is None or self.firmware_checking:
                return
            self.firmware_checking = True

        def run():
            try:
                if force or self.firmware.poll_due(datetime.now(timezone.utc)):
                    with httpx.Client(timeout=15, trust_env=False) as client:
                        poll_github(self.firmware, client, device)
            except (OSError, sqlite3.Error):
                logging.getLogger(__name__).warning('Firmware release check could not access the cache')
            finally:
                with self.lock:
                    self.firmware_checking = False

        Thread(target=run, daemon=True).start()

    def firmware_action(self, path, values):
        device = self.firmware_device()
        if device is None:
            raise ReleaseError('Finish board transport setup before managing firmware')
        if path == '/updates/check':
            self.start_firmware_check(force=True)
        elif path == '/updates/upload':
            release = verified_metadata(values['metadata'], values['signature'])
            self.firmware.cache(release, values['image'], device, manual=True)
        else:
            board = values.get('board')
            if board not in BOARDS:
                raise ReleaseError('Unknown board')
            if path == '/updates/install':
                self.firmware.approve(board, device, values.get('version'))
            else:
                self.firmware.set_automatic(board, device, values.get('automatic') == 'yes')

    def _rerender(self):
        result = subprocess.run([sys.executable, '-m', 'ventanal.settings_worker', str(self.config_path), *[('--refresh-pool' if effect=='pool' else '--force-'+effect) for effect in sorted(self.active_effects)]],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=600)
        if result.returncode: raise RuntimeError('Render incomplete; see recent activity.')
        try: values = json.loads(result.stdout)
        except ValueError: raise RuntimeError('Render returned an invalid result.') from None
        if any(not values.get(key, False) for key in self.active_effects & {'art','weather','spares'}):
            raise RuntimeError('Render incomplete; see recent activity.')

    def start_render(self, effects=None, *, delay=0):
        with self.lock:
            self.render_effects.update(effects if effects is not None else {'art', 'weather', 'spares'})
            if not self.render_effects: return
            self.render_deadline = time.monotonic() + delay
            self.render_pending = True
            if self.rendering: return
            self.rendering = True
            self.render_result = 'Updating screens…'
        def run():
            while True:
                with self.lock:
                    wait = self.render_deadline - time.monotonic()
                if wait > 0:
                    self.cleanup_stop.wait(min(wait, .1))
                    if self.cleanup_stop.is_set(): return
                    continue
                with self.lock:
                    self.active_effects = self.render_effects
                    self.render_effects = set()
                    self.render_pending = False
                try:
                    self.rerender()
                    result = 'Screens updated ' + datetime.now().strftime('%H:%M')
                except Exception:
                    result = 'Updating screens incomplete; the next tick will retry.'
                    self.store.event('tick error', 'Settings re-render incomplete')
                with self.lock:
                    if self.render_pending: continue
                    self.render_result = result
                    self.rendering = False
                    return
        Thread(target=run, daemon=True).start()

    def settings_effects(self, keys):
        effects = set()
        for key in keys:
            if key.startswith(('ham.','location.','panel.')): effects.add('solar')
            if key in ('schedule.quiet_start','schedule.quiet_end'): effects.add('schedule')
            if key.startswith('location.') or key.startswith('weather.') or key.startswith('display.'):
                effects.add('weather')
            if key.startswith('panel.'):
                effects.update(('art','weather','spares'))
            if key.startswith('art.') and key not in ('art.no_repeat_days','art.ignored'):
                effects.add('pool')
        return effects

    def respond(self, h, status, body=b'', kind='text/html; charset=utf-8', cookie=None, location=None, cache=False):
        if isinstance(body, str): body=body.encode()
        h.send_response(status)
        if h.command == 'POST':
            h.send_header('Connection', 'close')
            h.close_connection = True
        h.send_header('Content-Type', kind)
        h.send_header('Content-Length', str(len(body)))
        h.send_header('Cache-Control', 'public, max-age=86400' if cache else 'no-store')
        h.send_header('X-Content-Type-Options', 'nosniff')
        h.send_header('Content-Security-Policy', "default-src 'self'; style-src 'self'; img-src 'self'; font-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'")
        if cookie: h.send_header('Set-Cookie', f'ventanal={cookie}; HttpOnly; SameSite=Strict; Path=/; Max-Age=2592000' + ('; Secure' if self.security.ready and self.auth.mode == 'password' else ''))
        if location: h.send_header('Location', location)
        h.end_headers()
        try: h.wfile.write(body)
        except ConnectionError: pass

    def page(self, path, session, title, body):
        nav = ''.join(f'<a class="{cls}{" cur" if path==url else ""}" href="{url}">{name}</a>' for cls,url,name in [('status','/','STATUS'),('art','/art','ART'),('settings','/settings','SETTINGS'),('cal','/calibration','CALIBRATION'),('extras','/extras','EXTRAS')])
        banner = '<div class="warn"><span>NO PASSWORD SET · ANYONE ON THIS NETWORK CAN CHANGE SETTINGS</span><a class="btn" href="/setup">SET A PASSWORD</a></div>' if self.auth.mode == 'none' else ''
        account = ''
        if self.auth.mode == 'password' and session['granted']:
            account = '<div class="account"><a href="/password">Change password</a>'+form('/logout',session['csrf'],'','Log out')+'</div>'
        return f'<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Ventanal · {e(title)}</title><link rel="stylesheet" href="/static/ui.css?v=extras1"></head><body><div class="frame"><div class="elbow-top"><div class="cap"></div><div class="bar">VENTANAL · OPERATIONS</div><div class="end"></div></div><nav class="side">{nav}<div class="fill"></div></nav><main>{banner}<h1>{e(title)}</h1>{body}{account}</main></div></body></html>'

    def settings_body(self, token, values=None, error=''):
        doc = tomlkit.parse(read_document(self.config_path)).unwrap()
        values = values or {}
        error_key = error.split(':',1)[0]
        sections = {}
        for key, kind in FIELDS.items():
            if key == 'art.rijksmuseum_key': continue
            section, name = key.split('.',1)
            value = values.get(key, doc.get(section,{}).get(name,'0.0.0.0:8766' if key=='server.https_listen' else ''))
            if key in READ_ONLY: value = doc.get(section, {}).get(name, '')
            if kind == 'secret': value = ''
            if isinstance(value,list) and kind != 'museums': value = ', '.join(map(str,value))
            label, hint = FIELD_HELP[key]
            if key in READ_ONLY:
                link = '/art' if key == 'art.ignored' else '/calibration'
                control = (f'<span class="readonly-value">Ignored artworks: {len(doc.get("art", {}).get("ignored", []))}</span>' if key == 'art.ignored' else f'<span class="readonly-value"><span class="colour-swatch swatch-{name}" aria-hidden="true"></span>{e(value)}</span>') + f'<small><a href="{link}">Open {"Art" if link == "/art" else "Calibration"}</a></small>'
            elif isinstance(kind,tuple):
                control = f'<select name="{key}">'+''.join(f'<option value="{e(v)}"{" selected" if v==value else ""}>{e(OPTION_LABELS.get(key, {}).get(v, v))}</option>' for v in kind)+'</select>'
            elif key == 'schedule.art_slots':
                control = '<div data-time-chips>'+''.join(f'<span class="time-chip"><input type="time" aria-label="Art change time" value="{e(v.strip())}"><button type="button" data-remove-time aria-label="Remove time">×</button></span>' for v in value.split(','))+'</div><button type="button" data-add-time class="btn">+ Add time</button>'+f'<input type="hidden" name="{key}" value="{e(value)}" disabled><noscript><input name="{key}" value="{e(value)}" aria-label="Art change times"></noscript>'
            elif kind == 'bool':
                checked = ' checked' if value is True or value == 'yes' else ''
                marker = f'{key}.present'
                control = (
                    f'<input type="hidden" name="{marker}" value="yes">'
                    f'<label class="checkbox-row"><input type="checkbox" name="{key}" value="yes"{checked}><span>{e(label)}</span></label>'
                )
            elif kind == 'museums':
                value = value if isinstance(value,list) else [value]
                control = '<input type="hidden" name="museums.present" value="yes">'+''.join(f'<label class="checkbox-row"><input type="checkbox" name="{key}" value="{v}"{" checked" if v in value else ""}><span>{e(MUSEUM_LABELS[v])}<small>{e(SOURCE_REGISTRY[v].description)}</small><small>{e(SOURCE_REGISTRY[v].legal_note)}</small></span></label>' for v in MUSEUMS)
            else:
                control = f'<input name="{key}" type="{"password" if kind=="secret" else "time" if key.startswith("schedule.quiet_") else "number" if kind in ("int","float") else "text"}" value="{e(value)}"'+(' autocomplete="new-password" placeholder="Leave blank to keep stored key"' if kind=='secret' else ' step="any"' if kind=='float' else '')+'>'
            inline = f'<span class="error">{e(error)}</span>' if error_key==key else ''
            if kind == 'secret': control += f'<label><input type="checkbox" name="{key}.clear" value="yes">Clear stored key</label>'
            if section == 'server': control += '<small>Service restart required for listen and directory changes; paths must be writable by the service.</small>'
            group = 'advanced' if key in ADVANCED else 'panel colours' if section == 'panel' else 'ham radio' if section == 'ham' else section
            fragment = (control if kind == 'bool' else f'<div>{e(label)}{control}</div>' if kind == 'museums' or key in READ_ONLY else f'<label>{e(label)}{control}</label>') + f'<small>{e(hint)}</small>'
            if key == 'location.latitude':
                coordinates = fragment
                continue
            if key == 'location.longitude':
                fragment = coordinates + fragment
                key = 'location.coordinates'
            sections.setdefault(group,[]).append(f'<div data-field="{key}">{fragment}<span class="field-state" role="status" aria-live="polite">{inline}</span></div>')
        advanced = sections.pop('advanced', [])
        from ventanal.solar.licence import verify
        cfg = load_config(self.config_path)
        gate = verify(cfg.cache_dir/'fcc.sqlite3', cfg.ham_call_sign, cfg.zip, datetime.now(timezone.utc))
        sections.setdefault('ham radio', []).append('<div><p id="ham-reason" role="status">'+e(gate.reason)+'</p><p>Last database refresh: <span id="ham-refreshed">'+e(gate.refreshed or 'never')+'</span></p><p id="ham-progress">'+e('Refreshing licence database…' if self.fcc_refreshing else self.fcc_result)+'</p><button class="btn" type="submit" formaction="/ham/refresh">Refresh licence database now</button></div>')
        body = ''.join(f'<fieldset><legend>{e(section.capitalize())}</legend><div class="fields">{"".join(fields)}</div></fieldset>' for section,fields in sections.items())
        seasons = doc['art']['seasons']
        try: count = season_count(values.get('seasons.count', len(seasons)))
        except ValueError: count = len(seasons)
        rows = ''
        for i in range(count):
            entry = seasons[i] if i<len(seasons) else {}
            cells = ''
            for name in ('name','start','end','terms'):
                key = f'art.seasons[{i}].{name}'
                value = values.get(key,entry.get(name,''))
                if isinstance(value,list): value=', '.join(value)
                if name in ('start','end'):
                    parts = str(value).split('-')
                    month, day = parts if len(parts)==2 else ('01','01')
                    control = f'<input type="hidden" name="{key}" value="{e(value)}"><span class="month-day">'
                    for part, selected, maximum in [('month',month,12),('day',day,31)]:
                        control += f'<select data-date-part="{part}" name="{key}.{part}" aria-label="Season {i+1} {name} {part}">'+''.join(f'<option value="{v:02}"{" selected" if str(v).zfill(2)==selected else ""}>{v}</option>' for v in range(1,maximum+1))+'</select>'
                    control += '</span>'
                else: control = f'<input aria-label="Season {i+1} {name}" name="{key}" value="{e(value)}">'
                cells += f'<td><label><span class="season-label">{e(name.title())}</span>{control}</label></td>'
            rows += f'<tr data-field="art.seasons[{i}]" data-season>'+cells+'<td><button type="button" class="btn" data-remove-season>Remove season</button><span class="field-state" role="status"></span></td></tr>'
        body += f'<fieldset data-field="seasons"><legend>Seasons</legend><noscript><label>Number of rows (1–50)<input name="seasons.count" value="{count}" type="number" min="1" max="50"></label><small>Use Update rows before filling new rows. Choose month and day. Terms: comma separated.</small><button class="btn" name="action" value="rows" type="submit">Update rows</button></noscript><div class="table-scroll"><table><thead><tr><th>Name</th><th>Start</th><th>End</th><th>Terms</th></tr></thead><tbody>{rows}</tbody></table></div><button type="button" class="btn" data-add-season>+ Add season</button><span class="field-state" role="status"></span></fieldset>'
        body += '<details><summary>Advanced</summary><fieldset><legend>Installation and overrides</legend><div class="fields">'+''.join(advanced)+'</div></fieldset></details>'
        return (f'<p class="error">{e(error)}</p>' if error else '') + '<p><a href="/security">Security</a> · <a href="/extras">Extras</a></p><p id="screen-state" role="status"></p><form id="settings-form" method="post" action="/settings">'+f'<input type="hidden" name="csrf" value="{e(token)}">'+body+'<noscript><button class="btn" type="submit">Save settings</button></noscript></form><script src="/static/settings.js?v=solar1" defer></script>'

    def status_body(self, token):
        from importlib.metadata import version
        cfg = load_config(self.config_path)
        m = read_manifest(cfg.output_dir)
        board = self.store.latest()
        now = datetime.now(ZoneInfo(cfg.timezone))
        at = datetime.fromisoformat(board['time']).astimezone(ZoneInfo(cfg.timezone)) if board else None
        age = f'{max(0,int((now-at).total_seconds()//60))} MIN AGO' if at else 'No board has checked in'
        bat = f"{board['bat']}%" if board and 'bat' in board else 'Unknown'
        wake = next_wake(cfg,now, board is not None and board.get('bat',100)<=10)
        data = [('LAST CHECK-IN',at.strftime('%H:%M') if at else 'never',age),('BATTERY (ESTIMATE)',bat, 'Firmware '+str(board.get('fw','Unknown')) if board else 'Firmware unknown'),('NEXT UPDATE',wake.strftime('%H:%M'),f'QUIET {cfg.quiet_start}–{cfg.quiet_end}'),('SHOWING',board.get('screen','Unknown').upper() if board else 'Unknown',showing(board,m,self.store.frame_role(board.get('frame')) if board else None))]
        battery_detail = '<p>'+ (f"Raw voltage: {board['bat_mv']/1000:.3f} V" if board and 'bat_mv' in board else 'Raw voltage: unknown') + '</p>'
        battery_detail += '<p>'+ (f'Calibrated: full = {cfg.battery_full_mv/1000:.3f} V' if cfg.battery_full_mv is not None else 'Default battery curve') + '</p>'
        if self.auth.mode == 'password':
            battery_detail += form('/battery',token,'<input type="hidden" name="action" value="full">','Battery is full — set as 100%')
            battery_detail += form('/battery',token,'<input type="hidden" name="action" value="reset">','Reset to default')
        body = '<p>Server '+e(version('ventanal'))+'</p><script src="/static/battery.js" defer></script><div class="grid">'+''.join(f'<div class="tile"><div class="k">{e(k)}</div><div class="v">{e(v)}</div><div class="s">{e(s)}</div>'+ ('<progress max="100" value="'+str(board['bat'])+'" aria-label="Battery"></progress>' if k.startswith('BATTERY') and board and 'bat' in board else '')+(battery_detail if k.startswith('BATTERY') else '')+'</div>' for k,v,s in data)+'</div><div class="previews">'
        if cfg.config_backup is not None:
            body = f'<p class="error" role="status">Live config could not be parsed. Using backup {e(cfg.config_backup.name)}.</p>' + body
        from ventanal.solar.licence import verify
        solar_gate = verify(cfg.cache_dir/'fcc.sqlite3',cfg.ham_call_sign,cfg.zip,datetime.now(timezone.utc))
        for screen in ('art','weather','solar'):
            entry = m.get('screens',{}).get(screen)
            credit = (entry.get('credit') or {}) if entry else {}
            title = screen.upper() + (' · '+credit.get('title','') if screen=='art' else '')
            if screen == 'solar' and not solar_gate.ok: entry = None
            label = solar_gate.reason if screen=='solar' else credit.get('artist','') if screen=='art' else ('RENDERED '+datetime.fromisoformat(entry['rendered']).astimezone(ZoneInfo(cfg.timezone)).strftime('%H:%M') if entry else '')
            picture = f'<img src="/ui-preview/{screen}/{entry["id"]}.png" alt="{e(screen)} panel preview">' if entry else '<div class="empty">No frame published</div>'
            body += f'<div class="pv"><div class="lbl{" w" if screen=="weather" else ""}"><span>{e(title)}</span><span>{e(label)}</span></div>{picture}</div>'
        body += '</div>'
        if (device := self.firmware_device()) is not None:
            from ventanal.ota_ui import body as firmware_body
            body += firmware_body(self.firmware, device, token, self.firmware_checking)
        body += '<div class="log">'
        for row in self.store.recent():
            at = datetime.fromisoformat(row['time']).astimezone(ZoneInfo(cfg.timezone))
            first = datetime.fromisoformat(row['first_time']).astimezone(ZoneInfo(cfg.timezone))
            stamp = at.strftime('%d %b %H:%M:%S')
            if row['count'] > 1: stamp = first.strftime('%d %b %H:%M:%S') + ' – ' + stamp
            detail = row['detail']
            if row['kind'] == 'check-in':
                values = json.loads(detail)
                detail = ' · '.join(str(values[k]) + ('%' if k == 'bat' else '')
                                    for k in ('screen', 'bat', 'fw') if k in values) or 'Legacy board request'
            repeat = f' ×{row["count"]}' if row['count'] > 1 else ''
            body += f'<div><b>{e(stamp)}</b> {e(row["kind"].upper())} · {e(detail)}{e(repeat)}</div>'
        body += '</div>'
        return body

    def handle(self, h, path, post=False):
        if path.startswith('/static/') and not post:
            name = path.removeprefix('/static/')
            assets = {'extras.js':(Path(__file__).parent/'static/extras.js','text/javascript'), 'ui.css':(Path(__file__).parent/'static/ui.css','text/css'), 'settings.js':(Path(__file__).parent/'static/settings.js','text/javascript'), 'battery.js':(Path(__file__).parent/'static/battery.js','text/javascript'), 'calibration.js':(Path(__file__).parent/'static/calibration.js','text/javascript'), **{f'Antonio-{w}.ttf':(Path(__file__).parent/f'fonts/antonio/Antonio-{w}.ttf','font/ttf') for w in ('Regular','Bold')}}
            if name not in assets: self.respond(h,404); return
            file, kind = assets[name]
            content = file.read_bytes()
            if name == 'ui.css':
                cfg = load_config(self.config_path)
                content += ''.join('.swatch-'+name+'{background:#'+''.join(f'{v:02x}' for v in rgb)+'}' for name,rgb in zip(('black','white','yellow','red','blue','green'),cfg.panel_rgb)).encode()
            self.respond(h,200,content,kind,cache=name != 'ui.css'); return
        paths = ('/extras','/extras/licences','/extras/upload','/extras/remove','/extras/configure','/','/settings','/settings/field','/settings/state','/ham/refresh','/battery','/art','/calibration','/setup','/setup/skip','/setup/confirm-skip','/login','/logout','/password','/security')
        if path not in paths and path not in FIRMWARE_ACTIONS and not path.startswith(('/ui-preview/', '/art/', '/calibration/')):
            self.respond(h,404); return
        try:
            cookies = SimpleCookie(h.headers.get('Cookie',''))
            session = self.auth.session(cookies['ventanal'].value) if 'ventanal' in cookies else None
        except CookieError: session=None
        cookie = None
        if session is None:
            session,cookie = self.auth.issue(False)
        token = session['csrf']
        def send(status,title,body): self.respond(h,status,self.page(path,session,title,body),cookie=cookie)
        def redirect(url): self.respond(h,303,cookie=cookie,location=url)
        if self.auth.mode=='password' and not session['granted'] and path!='/login': redirect('/login'); return
        if path=='/security' and self.auth.mode!='password':
            self.respond(h,403); return
        if self.auth.mode=='unset' and path not in ('/setup','/setup/skip','/setup/confirm-skip','/login'):
            if post: self.respond(h,403); return
            path='/setup'
        if post:
            if cookie is not None:
                self.respond(h,403); return
            if path == '/calibration/upload':
                submitted = h.headers.get_all('X-CSRF-Token', [])
                if len(submitted) != 1 or not hmac.compare_digest(submitted[0].encode(), token.encode()):
                    self.respond(h,403); return
            try:
                length = int(h.headers.get('Content-Length','0'))
                if path == '/extras/upload':
                    from ventanal.extras.ui import upload
                    if length > 24576:
                        send(413,'Extras','Licence upload exceeds 16 KiB.'); return
                    if not h.headers.get('Content-Type','').startswith('multipart/form-data;'): raise ValueError()
                    h.continue_body()
                    values = upload(h,length)
                elif path == '/updates/upload':
                    from ventanal.ota_ui import MAX_UPLOAD, upload
                    if length > MAX_UPLOAD:
                        self.respond(h,413); return
                    values = upload(h, length)
                elif path == '/calibration/upload':
                    from ventanal.calibration_ui import MAX_UPLOAD, UPLOAD_HINT
                    if length > MAX_UPLOAD + 65536:
                        send(413,'Calibration',e(UPLOAD_HINT)); return
                    if length <= 0 or not h.headers.get('Content-Type','').startswith('multipart/form-data;'): raise ValueError()
                    from email.parser import BytesParser
                    from email.policy import default
                    h.continue_body()
                    payload = h.rfile.read(length)
                    message = BytesParser(policy=default).parsebytes(('Content-Type: '+h.headers['Content-Type']+'\r\nMIME-Version: 1.0\r\n\r\n').encode()+payload)
                    parts = list(message.iter_parts())
                    if len(parts) != 1 or message.defects: raise ValueError()
                    part = parts[0]
                    if (part.is_multipart() or part.defects or part.get_content_disposition() != 'form-data'
                            or part.get_param('name', header='content-disposition') != 'photo'):
                        raise ValueError()
                    content = part.get_payload(decode=True)
                    if not isinstance(content, bytes): raise ValueError()
                    if len(content)>MAX_UPLOAD: send(413,'Calibration',e(UPLOAD_HINT)); return
                    values = {'photo': content, 'filename': part.get_filename() or ''}
                else:
                    if not 0 < length <= 131072 or h.headers.get('Content-Type','').split(';')[0] != 'application/x-www-form-urlencoded': raise ValueError()
                    h.continue_body()
                    raw = parse_qs(h.rfile.read(length).decode(),keep_blank_values=True, max_num_fields=1000)
                    if any(len(v)!=1 for k,v in raw.items() if k!='art.museums'): raise ValueError()
                    values = {k:(v if k=='art.museums' else v[0]) for k,v in raw.items()}
            except (ValueError, UnicodeError): self.respond(h,400); return
            if path != '/calibration/upload' and not hmac.compare_digest(values.get('csrf','').encode(),token.encode()):
                self.respond(h,403); return
        if self.auth.mode=='password' and not session['granted'] and path!='/login': redirect('/login'); return
        if post and path in FIRMWARE_ACTIONS:
            try:
                self.firmware_action(path, values)
            except ReleaseError as exc:
                send(400, 'FRAME STATUS', f'<p class="error" role="status">{e(exc)}</p>' + self.status_body(token))
                return
            except sqlite3.Error:
                send(503, 'FRAME STATUS', '<p class="error">The firmware cache is unavailable.</p>')
                return
            redirect('/')
            return
        if path.startswith('/ui-preview/'):
            match = re.fullmatch(r'/ui-preview/(art|weather|solar|spare[0-2])/([0-9a-f]{16})\.png',path)
            if post or not match: self.respond(h,404); return
            cfg=load_config(self.config_path)
            entry=read_manifest(cfg.output_dir).get('screens',{}).get(match[1],{})
            if match[1] == 'solar':
                from ventanal.solar.licence import verify
                if not verify(cfg.cache_dir/'fcc.sqlite3',cfg.ham_call_sign,cfg.zip,datetime.now(timezone.utc)).ok:
                    self.respond(h,404); return
            if match[1].startswith('spare'):
                entry=next((row[1] for row in self.store.spares() if row[0]==int(match[1][-1])),{})
            if entry.get('id')!=match[2]: self.respond(h,404); return
            try: data=preview(cfg,entry).read_bytes()
            except (OSError,ValueError): self.respond(h,404); return
            self.respond(h,200,data,'image/png'); return
        if path=='/logout':
            if not post: self.respond(h,405); return
            self.auth.invalidate(); session,cookie=self.auth.issue(False); redirect('/login'); return
        if path in ('/setup/skip','/setup/confirm-skip'):
            if not post: self.respond(h,405); return
            with self.auth.lock:
                if session['epoch'] != self.auth.state['epoch']:
                    self.respond(h,403); return
                if self.auth.mode != 'unset':
                    self.respond(h,400); return
                if path.endswith('confirm-skip'):
                    if not session.get('setup_skip') or values.get('confirm')!='yes':
                        send(400,'Confirmation required','Confirm the warning before continuing.'); return
                    try: self.auth.none()
                    except OSError:
                        send(500,'Setup','Unable to save setup. Check service permissions and try again.'); return
                    session,cookie=self.auth.issue(False)
                    redirect('/'); return
                try:
                    if not self.auth.consume_setup_token(values.get('setup_token','')):
                        self.respond(h,403); return
                except OSError:
                    send(500,'Setup','Unable to validate setup. Check service permissions and try again.'); return
                cookie = self.auth.sign({**session, 'setup_skip': True})
            send(200,'Network access warning', '<div class="form-card auth"><p>Anyone on this network will be able to view and change settings. Continue only if you trust everyone with network access.</p>'+form('/setup/confirm-skip',token,'<input type="hidden" name="confirm" value="yes">','I understand — continue without a password')+'<a href="/setup">Choose a password instead</a></div>'); return
        if path in ('/setup','/password'):
            change=self.auth.mode=='password'
            if path=='/password' and not change: redirect('/setup'); return
            error=''
            if post:
                try:
                    with self.auth.lock:
                        if session['epoch'] != self.auth.state['epoch']:
                            self.respond(h,403); return
                        if self.auth.mode == 'password' and (not change or not session['granted']):
                            redirect('/login'); return
                        if change and not self.auth.verify(values.get('current','')): raise ValueError('Current password is incorrect.')
                        password, confirm = values.get('password',''), values.get('confirm','')
                        if not change:
                            validate_password(password,confirm)
                            if not self.auth.consume_setup_token(values.get('setup_token','')):
                                self.respond(h,403); return
                            self.enable_security()
                        self.auth.set_password(password,confirm)
                except ValueError as exc: error=str(exc)
                except OSError: error='Unable to enable HTTPS; check service permissions and listener port.'
                else:
                    if not change:
                        cookie = None; redirect('/login'); return
                    session,cookie=self.auth.issue(True); redirect('/'); return
            title='Change password' if change else 'Choose a password'
            first_run = self.auth.mode == 'unset'
            body=f'<div class="form-card auth"><p class="error">{e(error)}</p>'
            if not change: body+='<p>Enter the setup token printed by the installer or the ventanal setup-token command. If setup fails after the token is accepted, run the command for a new token.</p>'
            body+=form(path,token,(SETUP_TOKEN_FIELD if not change else '')+password_fields(change),title)
            if first_run: body+=form('/setup/skip',token,SETUP_TOKEN_FIELD,'Continue without a password…')
            send(400 if error else 200,title,body+'</div>'); return
        if path=='/login':
            if self.auth.mode!='password': redirect('/'); return
            error=''
            if post:
                ip=h.client_address[0]
                if not self.auth.allowed(ip): send(429,'Log in','Too many attempts. Try again in a minute.'); return
                if self.auth.verify(values.get('password','')):
                    session,cookie=self.auth.issue(True); redirect('/'); return
                self.auth.failed(ip); error='Incorrect password.'
            send(401 if error else 200,'Log in','<div class="form-card auth"><p class="error">'+e(error)+'</p>'+form('/login',token,'<label>Password<input type="password" name="password" autocomplete="current-password" required></label>','Log in')+'</div>'); return
        if path=='/security':
            if self.auth.mode != 'password' or not session['granted']:
                self.respond(h,403); return
            error=''
            reveal=False
            if post:
                action=values.get('action')
                if action in ('regenerate','repair') and not self.security.ready:
                    send(400,'Security','Enable security before rotating its certificate or key.'); return
                if action in ('enable','regenerate','restore','repair') and values.get('confirm')!='yes':
                    send(400,'Security','Confirm the security change before continuing.'); return
                try:
                    if action=='enable':
                        self.enable_security()
                        self.auth.invalidate()
                        cookie=None; redirect('/login'); return
                    elif action=='regenerate':
                        self.security.regenerate(discard_previous=values.get('discard_previous')=='yes')
                    elif action=='restore':
                        if not self.security.restore(): error=self.security.NO_PREVIOUS_CERTIFICATE
                    elif action=='repair': self.security.repair()
                    elif action=='reveal': reveal=True
                    else: self.respond(h,400); return
                except PreviousCertificateHeld as exc: error=str(exc)+' Restore it or confirm that it can be discarded.'
                except OSError: error='Unable to update security material; check service permissions and listener port.'
            cfg=load_config(self.config_path)
            body='<div class="form-card security-panel"><p>Security '+('ON' if self.security.ready else 'OFF — activation required for this existing password')+'</p>'
            if self.security.ready:
                import socket
                body+=f'<p>HTTPS URL: https://{e(socket.gethostname())}:{cfg.https_port}</p><p>Certificate SHA-256: <code>{e(self.security.fingerprint)}</code></p>'
                body+=form('/security',token,'<input type="hidden" name="action" value="reveal">','Reveal board values')
                if reveal:
                    body+=f'<p>Copy the PEM as a C++ raw string into VENTANAL_HOST_CERT_PEM. Set VENTANAL_HOST_URL to the HTTPS URL.</p><pre>{e(self.security.certificate)}</pre><label>VENTANAL_DEVICE_KEY<input readonly value="{e(self.security.key.hex())}"></label>'
                for action,label in [('regenerate','Regenerate certificate'),('repair','Re-pair board (new device key)')]:
                    fields = f'<input type="hidden" name="action" value="{action}"><label><input type="checkbox" name="confirm" value="yes" required>I understand the board must be updated afterwards.</label>'
                    if action == 'regenerate' and self.security.can_restore:
                        fields += '<label><input type="checkbox" name="discard_previous" value="yes" required>Discard the saved rollback certificate. The current certificate will become the new rollback certificate.</label>'
                    body+=form('/security',token,fields,label)
                if self.security.can_restore:
                    body+=form('/security',token,'<input type="hidden" name="action" value="restore"><label><input type="checkbox" name="confirm" value="yes" required>Present the previous certificate to the board again.</label>','Restore previous certificate')
            else:
                body+=form('/security',token,'<input type="hidden" name="action" value="enable"><label><input type="checkbox" name="confirm" value="yes" required>Enable HTTPS; the board must be updated afterwards.</label>','Enable security')
            send(400 if error else 200,'Security',body+e(error)+'</div>'); return
        if path == '/ham/refresh':
            if not post: send(405,'Method not allowed',''); return
            with self.lock:
                if not self.fcc_refreshing:
                    self.fcc_refreshing = True
                    def refresh_database():
                        try:
                            result = subprocess.run([sys.executable,'-m','ventanal.solar.worker',str(self.config_path)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                            self.fcc_result = {0:'Database refreshed',2:'Database refresh already running'}.get(result.returncode,'Database refresh failed; please try again.')
                        finally: self.fcc_refreshing = False
                    Thread(target=refresh_database,daemon=True).start()
            send(200,'Settings',self.settings_body(token)); return
        if path == '/battery':
            if self.auth.mode != 'password': self.respond(h,403); return
            if not post: self.respond(h,405); return
            action = values.get('action')
            if action == 'full' and values.get('confirm') != 'yes':
                send(200,'Confirm battery calibration',form('/battery',token,
                    '<p>Only do this after the frame has been charging for several hours</p>'
                    '<input type="hidden" name="action" value="full"><input type="hidden" name="confirm" value="yes">',
                    'Battery is full — set as 100%')); return
            error = ''
            full_mv = None
            if action == 'full':
                board = self.store.latest()
                if not board or 'bat_mv' not in board or not 0 <= (datetime.now(timezone.utc)-datetime.fromisoformat(board['time'])).total_seconds() <= 7200:
                    error = 'A recent voltage report is needed. Wake the frame and try again (within 2 hours).'
                elif not 4000 <= board['bat_mv'] <= 4300:
                    error = 'The report must be between 4000 and 4300 mV. Charge the frame for several hours and try again.'
                else: full_mv = board['bat_mv']
            elif action != 'reset': error = 'Choose a battery calibration action.'
            if not error:
                def mutate(doc):
                    if full_mv is None:
                        if 'battery' in doc: doc['battery'].pop('full_mv', None)
                    else: doc.setdefault('battery', tomlkit.table())['full_mv'] = full_mv
                browser = values.get('browser_session', token)
                if not re.fullmatch(r'[A-Za-z0-9_-]{1,80}', browser): browser = token
                try: save_settings(self.config_path, {}, mutate=mutate, browser_session=token+browser)
                except ConfigWriteError as exc: error = str(exc)
                except (OSError, ValueError): error = 'Unable to save battery calibration. Please try again.'
            if error: send(400,'Battery calibration',f'<p class="error">{e(error)}</p><a href="/">Return to Status</a>')
            else: redirect('/')
            return
        if path == '/settings/state':
            from ventanal.solar.licence import verify
            cfg = load_config(self.config_path)
            gate = verify(cfg.cache_dir/'fcc.sqlite3',cfg.ham_call_sign,cfg.zip,datetime.now(timezone.utc))
            self.respond(h,200,json.dumps({'status':self.render_result,'ham':{'reason':gate.reason,'refreshed':gate.refreshed or 'never','progress':'Refreshing licence database…' if self.fcc_refreshing else self.fcc_result}}), 'application/json'); return
        if path == '/settings/field':
            if not post: self.respond(h,405); return
            field = values.pop('field', '')
            values.pop('csrf', None)
            browser = values.pop('browser_session', token)
            if not re.fullmatch(r'[A-Za-z0-9_-]{1,80}', browser): browser = token
            allowed = {field, field+'.clear'}
            if re.fullmatch(r'art.seasons\[\d+\]', field):
                allowed = {field+'.'+name for name in ('name','start','end','terms')}
            if field == 'location.coordinates': allowed = {'location.latitude','location.longitude'}
            if field == 'seasons':
                allowed = {'seasons.count'} | {k for k in values if re.fullmatch(r'art.seasons\[\d+\]\.(name|start|end|terms)', k)}
            valid = field in ('seasons','location.coordinates') or field in FIELDS and field not in READ_ONLY or bool(re.fullmatch(r'art.seasons\[\d+\]', field))
            error = ''
            if not valid or not values or not set(values) <= allowed:
                error = 'Choose one editable setting.'
            else:
                try: save_settings(self.config_path, values, browser_session=token+browser)
                except (ValueError,ConfigError) as exc: error = str(exc)
                except OSError: error = 'Unable to save settings; check file permissions.'
            if not error: self.start_render(self.settings_effects(values), delay=self.render_delay)
            self.respond(h,400 if error else 200,json.dumps({'saved':not error, 'error':error}), 'application/json'); return
        if path=='/settings':
            error=''
            if post:
                for key, kind in FIELDS.items():
                    if kind == 'bool' and values.get(f'{key}.present') and key not in values:
                        values[key] = 'no'
                # Accept forms opened before the marker names changed.
                if values.get('screen_fit.present') and 'art.screen_fit_only' not in values:
                    values['art.screen_fit_only'] = 'no'
                if values.get('monotone.present') and 'art.include_monotone' not in values:
                    values['art.include_monotone'] = 'no'
                if values.get('museums.present') and 'art.museums' not in values: values['art.museums']=[]
                if values.get('action')=='rows':
                    try:
                        values['seasons.count'] = season_count(values.get('seasons.count'))
                        body=self.settings_body(token,values)
                    except ValueError as exc: send(400,'Settings',self.settings_body(token,values,str(exc))); return
                    send(200,'Settings',body); return
                try:
                    if 'seasons.count' in values: values['seasons.count'] = season_count(values['seasons.count'])
                    cfg=save_settings(self.config_path,values,browser_session=token)
                except (ValueError,ConfigError) as exc: error=str(exc)
                except OSError: error='config.file: Unable to save settings; check file permissions.'
                else:
                    self.start_render(self.settings_effects(values), delay=self.render_delay)
            send(400 if error else 200,'Settings',self.settings_body(token,values if post and error else None,error)); return
        if path == '/extras' or path.startswith('/extras/'):
            from ventanal.extras.ui import handle
            handle(self,h,path,post,values if post else {},token,send,redirect)
            return
        if path.startswith('/calibration'):
            from ventanal.calibration_ui import handle
            with self.calibration_lock:
                handle(self,h,path,post,values if post else {},token,send,redirect)
            return
        if path in ('/art/ignore','/art/unignore'):
            if not post: self.respond(h,405); return
            identity=values.get('identity','')
            if not re.fullmatch(r'[a-z][a-z0-9_-]*:[A-Za-z0-9_-]+',identity):
                send(400,'Art','Invalid artwork identity.'); return
            from ventanal.art_ui import update_ignored
            try: update_ignored(self,identity,path.endswith('/ignore'))
            except (ValueError,OSError): send(400,'Art','Unable to save ignored artwork.'); return
            redirect('/art'); return
        if path=='/art' and not post:
            from ventanal.art_ui import body
            send(200,'Art',body(self,token)); return
        if post: self.respond(h,405); return
        if path=='/': send(200,'FRAME STATUS',self.status_body(token)); return
        self.respond(h,404)
