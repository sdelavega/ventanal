# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Status page controls for firmware release checks and installation."""

from email.parser import BytesParser
from email.policy import default
from urllib.parse import urlsplit

from ventanal.ota import MAX_METADATA, SLOT_SIZE
from ventanal.release import firmware_updates_enabled
from ventanal.ui import e, form

MAX_UPLOAD = SLOT_SIZE + 16384
PART_LIMITS = {"image": SLOT_SIZE, "metadata": MAX_METADATA, "signature": 88, "csrf": 256}
BOARD_NAMES = {"inkplate13spectra": "Inkplate 13SPECTRA", "lcd43b": "Waveshare 4.3B LCD"}


def upload(handler, length):
    content_type = handler.headers.get("Content-Type", "")
    if not 0 < length <= MAX_UPLOAD or not content_type.startswith("multipart/form-data;"):
        raise ValueError("Invalid firmware upload")
    handler.continue_body()
    payload = handler.rfile.read(length)
    if len(payload) != length:
        raise ValueError("Incomplete firmware upload")
    message = BytesParser(policy=default).parsebytes(
        f"Content-Type: {content_type}\r\nMIME-Version: 1.0\r\n\r\n".encode() + payload
    )
    if message.get_content_type() != "multipart/form-data" or message.defects:
        raise ValueError("Invalid firmware upload")
    values = {}
    for part in message.iter_parts():
        name = part.get_param("name", header="content-disposition")
        if (name not in PART_LIMITS or name in values or part.is_multipart() or part.defects
                or part.get_content_disposition() != "form-data"):
            raise ValueError("Invalid firmware upload field")
        data = part.get_payload(decode=True)
        if not isinstance(data, bytes) or not 0 < len(data) <= PART_LIMITS[name]:
            raise ValueError("Invalid firmware upload field size")
        values[name] = data
    if values.keys() != PART_LIMITS.keys():
        raise ValueError("Missing firmware upload field")
    values["csrf"] = values["csrf"].decode("utf-8")
    return values


def notes_link(value):
    try:
        url = urlsplit(value)
        if url.scheme == "https" and url.hostname and not url.username and not url.password:
            return f' · <a href="{e(value)}" rel="noreferrer">Release notes</a>'
    except ValueError:
        pass
    return ""


def body(store, devices, token, checking):
    last = store.last_poll()
    parts = ['<section class="form-card"><h2>Firmware updates</h2>']
    if checking:
        parts.append('<p role="status">Checking GitHub…</p>')
    if last:
        checked = last["checked"].replace("T", " ")[:16]
        parts.append(f"<p>Last GitHub check {e(checked)} UTC</p>")
        if last["error"]:
            parts.append(f'<p class="error" role="status">{e(last["error"])}</p>')
    enabled = firmware_updates_enabled()
    if enabled:
        parts.append(form("/updates/check", token, "", "Check for updates"))
    boards = [(pairing, device, state, releases) for pairing, device in devices.items()
              for state, releases in store.boards(device)]
    if not boards:
        parts.append("<p>No board has checked in with a board ID.</p>")
    for pairing, device, state, releases in boards:
        board = state["board"]
        fields = f'<input type="hidden" name="board" value="{e(board)}">'
        name = BOARD_NAMES[board]
        if pairing:
            name += f' ({pairing})'
            fields += f'<input type="hidden" name="pairing" value="{e(device)}">'
        parts.append(f'<h3>{e(name)}</h3><p>Running version {e(state["running"] or "Unknown")}</p>')
        if state["result"]:
            parts.append(f'<p>Last OTA result {e(state["result"])}</p>')
        if state["pending"]:
            parts.append(f'<p>Approved to install {e(state["pending"])}</p>')
        if enabled and releases:
            latest = releases[0]
            uploaded = max((r for r in releases if r.uploaded), key=lambda r: r.uploaded, default=None)
            choices = [("Update available", latest)]
            if uploaded and uploaded != latest:
                choices.append(("Uploaded release", uploaded))
            for label, release in choices:
                available = release.info.version
                parts.append(f'<p>{label}: {e(available)}{notes_link(release.info.notes_url)}</p>')
                exhausted = (state["result"] or "").startswith("failed:attempts")
                if available != state["pending"] or exhausted:
                    install = fields + f'<input type="hidden" name="version" value="{e(available)}">'
                    parts.append(form("/updates/install", token, install, "Install"))
        if board == "lcd43b":
            parts.append("<p>Confirm installation on the LCD.</p>")
        elif enabled:
            checked = " checked" if state["automatic"] else ""
            auto = fields + f'<label class="checkbox-row"><input type="checkbox" name="automatic" value="yes"{checked}><span>Install automatically</span></label>'
            parts.append(form("/updates/automatic", token, auto, "Save update preference"))
    if not enabled:
        return "".join(parts) + "</section>"
    parts.append(
        '<form method="post" action="/updates/upload" enctype="multipart/form-data">'
        f'<input type="hidden" name="csrf" value="{e(token)}">'
        '<label>Firmware image<input type="file" name="image" accept=".bin" required></label>'
        '<label>Release metadata<input type="file" name="metadata" accept=".json" required></label>'
        '<label>Release signature<input type="file" name="signature" accept=".sig" required></label>'
        '<button class="btn" type="submit">Upload signed release</button></form></section>'
    )
    return "".join(parts)
