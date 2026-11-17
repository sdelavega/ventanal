# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Escaped extras controls behind the shared authentication and CSRF gate."""

from ventanal.extras import registry
from ventanal.extras.licence import Store, MAX_TOKEN
from ventanal.ui import e, form


def upload(h, length):
    from email.parser import BytesParser
    from email.policy import default

    if not 0 < length <= MAX_TOKEN + 8192:
        raise ValueError("Upload too large")
    message = BytesParser(policy=default).parsebytes(
        (
            "Content-Type: "
            + h.headers["Content-Type"]
            + "\r\nMIME-Version: 1.0\r\n\r\n"
        ).encode()
        + h.rfile.read(length)
    )
    if not message.is_multipart() or message.defects:
        raise ValueError("Invalid upload")
    values = {}
    for part in message.iter_parts():
        name = part.get_param("name", header="content-disposition")
        data = part.get_payload(decode=True)
        if (
            name not in ("csrf", "token")
            or name in values
            or part.is_multipart()
            or part.defects
            or part.get_content_disposition() != "form-data"
            or not isinstance(data, bytes)
        ):
            raise ValueError("Invalid upload")
        if len(data) > (MAX_TOKEN if name == "token" else 256):
            raise ValueError("Upload too large")
        values[name] = data.decode("utf-8")
    if set(values) != {"csrf", "token"}:
        raise ValueError("Invalid upload")
    return values


def body(store, csrf, error=""):
    installed = registry.discover()
    licences = store.licences()
    enabled = {m.id for m in registry.active(installed, store, licences=licences)}
    settings = store.read()["settings"]
    cards = ""
    for m in installed:
        state = settings.get(m.id, {})
        controls = f'<input type="hidden" name="id" value="{e(m.id)}"><label class="checkbox-row"><input type="checkbox" name="enabled" value="yes"{" checked" if state.get("enabled") else ""}><span>Enable {e(m.name)}</span></label>'
        for f in m.settings:
            value = state.get("values", {}).get(f.id, f.default)
            control = (
                f'<input type="checkbox" name="{e(f.id)}" value="yes"{" checked" if value else ""}>'
                if f.kind == "bool"
                else f'<input name="{e(f.id)}" value="{e(value)}" maxlength="1024">'
            )
            controls += f"<label>{e(f.name)}{control}</label>"
        controls += '<span class="field-state" role="status" aria-live="polite"></span><noscript><button class="btn">Apply</button></noscript>'
        cards += f'<article class="form-card extra-card"><h3>{e(m.name)}</h3><p>{e(m.description)}</p><p data-extra-status>{"Active" if m.id in enabled else "Inactive — requires a licence and enablement"} · {e(m.kind)}</p>'
        if m.kind == "hosted":
            cards += f"<p>Privacy: {e(m.privacy)}</p>"
        cards += f'<form method="post" action="/extras/configure" data-extra><input type="hidden" name="csrf" value="{e(csrf)}">{controls}</form></article>'
    result = "<p>Optional add-ons and hosted services. All existing core features remain free.</p>"
    if error:
        result += f'<p class="error" role="alert">{e(error)}</p>'
    result += (
        "<section><h2>Installed</h2>"
        + (cards or "<p>No extras installed yet</p>")
        + "</section><section><h2>Available</h2>"
    )
    available = ""
    for m in registry.catalogue():
        available += f'<article class="form-card"><h3>{e(m["name"])}</h3><p>{e(m["description"])}</p>'
        if m.get("kind") == "hosted":
            available += f'<p>Privacy: {e(m["privacy"])}</p>'
        available += f'<a class="btn" href="{e(m["checkout_url"])}" target="_blank" rel="noopener noreferrer">Buy</a></article>'
    result += (
        available or "<p>No extras available to buy yet.</p>"
    ) + "</section><section><h2>Licences</h2>"
    for ident, r in licences:
        p = r.payload or {}
        result += (
            '<article class="form-card"><p>'
            + e(p.get("licence_id", "Unrecognized licence"))
            + " · "
            + e(r.status)
            + "</p><p>Features: "
            + e(", ".join(p.get("features", [])))
            + "</p><p>Kind: "
            + e(p.get("kind", "unknown"))
            + " · Expiry: "
            + e(
                p.get("expires", "perpetual")
                if p.get("kind") == "perpetual"
                else expiry(p.get("expires"))
            )
            + "</p>"
            + form(
                "/extras/remove",
                csrf,
                f'<input type="hidden" name="id" value="{e(ident)}">',
                "Remove licence",
            )
            + "</article>"
        )
    if not licences:
        result += "<p>No licences added.</p>"
    result += form(
        "/extras/licences",
        csrf,
        f'<label>Paste licence token<textarea name="token" maxlength="{MAX_TOKEN}" required autocomplete="off" spellcheck="false"></textarea></label>',
        "Add licence",
    )
    result += f'<form method="post" action="/extras/upload" enctype="multipart/form-data"><input type="hidden" name="csrf" value="{e(csrf)}"><label>Licence file (up to 16 KiB)<input type="file" name="token" accept=".txt,.licence" required></label><button class="btn">Upload licence</button></form></section><script src="/static/extras.js?v=extras1" defer></script>'
    return result


def expiry(value):
    from datetime import datetime, timezone

    return (
        datetime.fromtimestamp(value, timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
        if isinstance(value, int)
        else "unknown"
    )


def handle(ui, h, path, post, values, csrf, send, redirect):
    store = Store(ui.auth.path.parent)
    error = ""
    if path != "/extras" and not post:
        ui.respond(h, 405)
        return
    if post:
        try:
            if path in ("/extras/licences", "/extras/upload"):
                store.add(values.get("token", ""))
            elif path == "/extras/remove":
                if not store.remove(values.get("id", "")):
                    raise ValueError("Licence not found")
            elif path == "/extras/configure":
                m = next(
                    (m for m in registry.discover() if m.id == values.get("id")), None
                )
                if m is None:
                    raise ValueError("Extra not installed")
                if values.get("enabled", "no") not in ("yes", "no"):
                    raise ValueError("Expected yes or no")
                fields = {
                    f.id: values.get(f.id, "no" if f.kind == "bool" else f.default)
                    for f in m.settings
                }
                if set(values) - {"csrf", "id", "enabled", *(f.id for f in m.settings)}:
                    raise ValueError("Unknown setting")
                store.configure(m, values.get("enabled") == "yes", fields)
                import json

                enabled = any(
                    item.id == m.id
                    for item in registry.active(registry.discover(), store)
                )
                ui.respond(
                    h,
                    200,
                    json.dumps({"saved": True, "active": enabled}),
                    "application/json",
                )
                return
            else:
                ui.respond(h, 404)
                return
        except ValueError as exc:
            error = str(exc)
        except OSError:
            error = "Unable to save extras; check state directory permissions."
        if not error:
            redirect("/extras")
            return
    send(400 if error else 200, "Extras", body(store, csrf, error))
