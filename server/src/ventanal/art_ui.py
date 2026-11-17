# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Published art, skip history and managed exclusions."""
from contextlib import closing
from dataclasses import asdict
from datetime import datetime
from zoneinfo import ZoneInfo
import fcntl
import json
from ventanal.art.pool import ArtPool
from ventanal.config import load_config
from ventanal.manifest import read_manifest
from ventanal.settings import save_settings


def metadata(store, identity):
    source, object_id = identity.split(':', 1)
    candidate = ArtPool(store.path).get(source, object_id)
    if candidate: return asdict(candidate)
    with closing(store.connect()) as db:
        for row in db.execute('SELECT metadata FROM published_art'):
            item = json.loads(row[0])
            if item['source'] == source and item['object_id'] == object_id: return item
    return {'title':identity, 'artist':'Unknown artist', 'museum':'Unknown museum'}


def update_ignored(ui, identity, ignore):
    cfg = load_config(ui.config_path)
    # Keep this ordering consistent with tick and feedback: tick lock, then DB.
    with (cfg.cache_dir/'tick.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        def mutate(doc):
            values = list(doc['art'].get('ignored', []))
            if ignore and identity not in values: values.append(identity)
            elif not ignore and identity in values: values.remove(identity)
            doc['art']['ignored'] = values
        save_settings(ui.config_path, {}, mutate=mutate)
        source, object_id = identity.split(':',1)
        with closing(ui.store.connect()) as db, db:
            if ignore:
                db.execute('INSERT OR IGNORE INTO ignore_replacements SELECT slot FROM spare_slots WHERE source=? AND object_id=? AND replace_due=0', (source, object_id))
                db.execute('UPDATE spare_slots SET replace_due=1 WHERE source=? AND object_id=?', (source, object_id))
            else:
                db.execute('UPDATE spare_slots SET replace_due=0 WHERE source=? AND object_id=? AND slot IN (SELECT slot FROM ignore_replacements)', (source, object_id))
                db.execute('DELETE FROM ignore_replacements WHERE slot IN (SELECT slot FROM spare_slots WHERE source=? AND object_id=?)', (source, object_id))


def body(ui, token):
    from ventanal.ui import e, form
    cfg = load_config(ui.config_path)
    manifest = read_manifest(cfg.output_dir)
    pool = ArtPool(ui.store.path)
    def fit_label(credit):
        c = pool.get(credit.get("source", ""), credit.get("object_id", ""))
        score = pool.fit_score(c, cfg.panel_rgb) if c else None
        return f"Screen fit {score:.0f}" if score is not None else "Screen fit pending"
    board = ui.store.latest()
    from ventanal.status import showing
    shown = showing(board, manifest, ui.store.frame_role(board.get('frame')) if board else None)
    result = '<p>Board showing: '+e(shown)+'. Thumbnails simulate the published frames with your panel palette.</p><div class="previews">'
    main = manifest.get('screens',{}).get('art')
    entries = [('Main painting','art',main)]
    spares = {slot: (entry, due) for slot, entry, _, _, due in ui.store.spares()}
    for i in range(3): entries.append((f'Spare {i+1}',f'spare{i}',spares.get(i,(None,False))[0]))
    for label, role, entry in entries:
        credit = entry.get('credit') or {} if entry else {}
        active = bool(board and board.get('screen')=='art' and entry and board.get('frame')==entry['id'])
        picture = f'<img src="/ui-preview/{role}/{entry["id"]}.png" alt="{e(label)} panel preview">' if entry else '<div class="empty">No frame published</div>'
        result += f'<article class="pv"><div class="lbl"><span>{e(label)}</span><span>{"ON BOARD" if active else ""}</span></div>{picture}<div class="credits"><h2>{e(credit.get("title","Unknown artwork"))}</h2><p>{e(credit.get("artist",""))} · {e(credit.get("date",""))}<br>{e(credit.get("museum",""))}</p>'
        result += '<p>'+e(fit_label(credit))+'</p>'
        if role.startswith('spare') and spares.get(int(role[-1]),(None,False))[1]: result += '<p class="notice">Replacement queued for the next tick.</p>'
        result += '</div></article>'
    result += '</div><p>Ignoring a spare queues its replacement at the next tick. An ignored main painting stays until the next art slot.</p><h2>Skipped artworks</h2><div class="table-scroll"><table><thead><tr><th>Title</th><th>Artist</th><th>Museum</th><th>Screen fit</th><th>Times skipped</th><th>Last skipped</th><th>Action</th></tr></thead><tbody>'
    rows = ui.store.skipped(limit=None)
    for item in rows:
        item = {**item, 'fit': fit_label(item), 'last_skipped': datetime.fromisoformat(item['last_skipped']).astimezone(ZoneInfo(cfg.timezone)).strftime('%d %b %H:%M')}
        identity = item['source']+':'+item['object_id']
        action = '<span>Ignored</span>' if identity in cfg.ignored else form('/art/ignore',token,f'<input type="hidden" name="identity" value="{e(identity)}">','Ignore')
        result += '<tr>'+''.join(f'<td data-label="{label}">'+e(item[key])+'</td>' for key,label in [('title','Title'),('artist','Artist'),('museum','Museum'),('fit','Screen fit'),('count','Times skipped'),('last_skipped','Last skipped')])+f'<td>{"<span class=notice>likely ignore</span>" if item["count"]>=2 else ""}{action}</td></tr>'
    if not rows: result += '<tr><td colspan="7">No skips recorded.</td></tr>'
    result += '</tbody></table></div><h2>Ignored artworks</h2><div class="grid">'
    for identity in cfg.ignored:
        item = metadata(ui.store,identity)
        result += '<div class="tile"><h3>'+e(item['title'])+'</h3><p>'+e(item['artist'])+' · '+e(item['museum'])+'</p><small>'+e(identity)+'</small>'+form('/art/unignore',token,f'<input type="hidden" name="identity" value="{e(identity)}">','Un-ignore')+'</div>'
    return result + ('<p>No ignored artworks.</p>' if not cfg.ignored else '')+'</div>'
