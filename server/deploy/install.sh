#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
set -eu
[ "$(id -u)" -eq 0 ] || { echo 'Run with sudo' >&2; exit 1; }
SOURCE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if ! python3 -m venv --help >/dev/null 2>&1 || ! dpkg-query -W -f='${Status}' python3-venv 2>/dev/null | grep -q 'install ok installed'; then
    apt-get update
    apt-get install -y python3-venv
fi
id ventanal >/dev/null 2>&1 || useradd --system --home-dir /var/lib/ventanal --shell /usr/sbin/nologin ventanal
install -d /opt/ventanal /opt/ventanal/package /etc/ventanal
install -d -o ventanal -g ventanal /var/lib/ventanal /var/cache/ventanal
# Install from a durable copy so the staging tree can be removed safely.
rm -rf /opt/ventanal/package/src
cp -R "$SOURCE/src" /opt/ventanal/package/src
cp "$SOURCE/pyproject.toml" /opt/ventanal/package/
python3 -m venv /opt/ventanal/venv
/opt/ventanal/venv/bin/pip install /opt/ventanal/package
if [ ! -e /etc/ventanal/ventanal.toml ]; then
    install -m 644 "$SOURCE/ventanal.example.toml" /etc/ventanal/ventanal.toml
fi
chown root:ventanal /etc/ventanal/ventanal.toml
chmod 0660 /etc/ventanal/ventanal.toml
install -d -o root -g ventanal -m 2770 /etc/ventanal/backups
for unit in ventanal-serve.service ventanal-tick.service ventanal-tick.timer ventanal-fcc.service ventanal-fcc.timer; do
    install -m 644 "$SOURCE/deploy/$unit" "/etc/systemd/system/$unit"
done
systemctl daemon-reload
systemctl enable ventanal-serve.service ventanal-tick.timer ventanal-fcc.timer
systemctl restart ventanal-serve.service ventanal-tick.timer ventanal-fcc.timer
