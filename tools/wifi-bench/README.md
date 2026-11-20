# Marginal Wi-Fi bench

Run this bench on a Raspberry Pi with an unused, unmanaged AP-capable Wi-Fi
interface in managed (station) mode and a separate wired Internet uplink.
The host needs Bash, systemd, iproute2 with the `netem` kernel module,
iptables, ethtool, hostapd, dnsmasq, flock, and Python 3. Keep the AP interface
free of addresses and other network services, and reserve `192.168.50.0/24`
for the bench.

As root, create `/etc/ventanal-bench.conf` with mode `0600` and fill in the
credentials locally. Use one literal assignment per line without shell
quotes. The SSID can be 1–32 bytes and the passphrase 8–63 printable ASCII
characters.

```ini
AP_IF=wlan0
WIRED_IF=eth0
SSID=
PASSPHRASE=
```

```sh
sudo ./tools/wifi-bench/bench.sh up baseline
sudo ./tools/wifi-bench/bench.sh status
sudo ./tools/wifi-bench/bench.sh down
```

| Profile | AP egress loss | Delay | Rate limit |
| --- | --- | --- | --- |
| `baseline` | 5% | 300 ms with 100 ms jitter | 256 kbit/s |
| `worse` | 15% | 600 ms | None |

The bench uses `192.168.50.1` for its IPv4 gateway and DNS. Connect a board or
phone to the bench SSID and load a page to check the link. Use `up worse` to
switch profiles. Repeating `up` with the active profile or repeating `down`
is safe. `status` prints the active profile and is silent when inactive.
The script renders its daemon configs under root-only
`/run/ventanal-wifi-bench`. `down` stops the bench daemons, removes the
bench's queue, address, and firewall rules, and restores the previous
forwarding and AP link settings. The wired interface keeps its addressing.

## Score a run

Capture the board's serial output with `HH:MM:SS` timestamps, starting before
boot. Then run `./tools/wifi-bench/score_log.py LOG`. The scorer emits one
JSON object and exits 1 if it sees a download-failed label, reboot, or
watchdog line.

Each refresh-end line counts as one pass. The scorer measures from boot to
the first refresh end and between consecutive refresh ends. It counts
standalone weather and solar frames, art and spare-art queue entries, and
successful host frame writes. A completed resume counts its `from` offset as
skipped bytes. For host frames, the frame result must also confirm that the
remaining bytes were transferred; a server that ignores Range does not count.
