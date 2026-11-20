#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set +x
set -euo pipefail
umask 077

config=/etc/ventanal-bench.conf
runtime=/run/ventanal-wifi-bench
template_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
subnet=192.168.50.0/24
address=192.168.50.1/24
qdisc_handle=50a0:
forwarding=/proc/sys/net/ipv4/conf

die() {
    printf '%s\n' "$*" >&2
    exit 1
}

read_config() {
    [[ -f "$config" && ! -L "$config" ]] ||
        die "Create a root-only $config first."
    case "$(stat -c '%u:%a' -- "$config")" in
        0:400|0:600) ;;
        *) die "$config must be owned by root with mode 0400 or 0600." ;;
    esac

    ap='' wired='' ssid='' passphrase=''
    export -n ssid passphrase
    local line
    while IFS= read -r line || [[ -n "$line" ]]; do
        case "$line" in
            AP_IF=*) ap=${line#*=} ;;
            WIRED_IF=*) wired=${line#*=} ;;
            SSID=*) ssid=${line#*=} ;;
            PASSPHRASE=*) passphrase=${line#*=} ;;
            ''|'#'*) ;;
            *) die "Unexpected entry in $config." ;;
        esac
    done < "$config"

    local ifname='^[a-zA-Z0-9_][a-zA-Z0-9_.-]{0,14}$'
    [[ "$ap" =~ $ifname && "$wired" =~ $ifname && "$ap" != "$wired" ]] ||
        die "AP_IF and WIRED_IF must name distinct interfaces."
    case "$ap:$wired" in
        all:*|default:*|*:all|*:default) die "Use interface names for AP_IF and WIRED_IF." ;;
    esac
    [[ -d "/sys/class/net/$ap/phy80211" ]] ||
        die "AP_IF must be a Wi-Fi interface."
    [[ "$wired" != lo && -d "/sys/class/net/$wired" && ! -d "/sys/class/net/$wired/phy80211" ]] ||
        die "WIRED_IF must be a wired uplink."
}

render_configs() {
    # Keep credentials out of child process arguments and exported variables.
    local credentials ssid_hex psk line
    credentials=$(printf '%s\n' "$ssid" "$passphrase" | python3 -c '
import hashlib
import sys

ssid, passphrase, _ = sys.stdin.buffer.read().split(b"\n")
if not (1 <= len(ssid) <= 32 and 8 <= len(passphrase) <= 63
        and all(32 <= byte <= 126 for byte in passphrase)):
    sys.exit("Use a 1–32 byte SSID and an 8–63 character ASCII passphrase.")
print(ssid.hex(), hashlib.pbkdf2_hmac("sha1", passphrase, ssid, 4096, 32).hex())
')
    read -r ssid_hex psk <<< "$credentials"
    unset ssid passphrase credentials
    while IFS= read -r line; do
        line=${line//@AP_IF@/$ap}
        line=${line//@SSID_HEX@/$ssid_hex}
        line=${line//@PSK@/$psk}
        printf '%s\n' "$line"
    done < "$template_dir/hostapd.conf.in" > "$runtime/hostapd.conf"
    while IFS= read -r line; do
        printf '%s\n' "${line//@AP_IF@/$ap}"
    done < "$template_dir/dnsmasq.conf.in" > "$runtime/dnsmasq.conf"
}

firewall() {
    local action=$1 marker=$2 table=$3 chain=$4 status
    shift 4
    local rule=("$@" -m comment --comment "ventanal-bench-$token")
    if [[ "$action" == add ]]; then
        record "$marker" iptables -w -t "$table" -I "$chain" 1 "${rule[@]}"
    elif [[ -f "$runtime/$marker" ]]; then
        if iptables -w -t "$table" -C "$chain" "${rule[@]}" 2>/dev/null; then
            iptables -w -t "$table" -D "$chain" "${rule[@]}" || return
        else
            status=$?
            [[ "$status" == 1 ]] || return "$status"
        fi
        rm -- "$runtime/$marker"
    fi
}

firewall_rules() {
    local failed=0
    firewall "$1" nat nat POSTROUTING -s "$subnet" -o "$wired" -j MASQUERADE || failed=1
    firewall "$1" forward-out filter FORWARD -i "$ap" -o "$wired" -s "$subnet" -j ACCEPT || failed=1
    firewall "$1" forward-in filter FORWARD -i "$wired" -o "$ap" -d "$subnet" \
        -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT || failed=1
    firewall "$1" dhcp filter INPUT -i "$ap" -p udp --dport 67 -j ACCEPT || failed=1
    firewall "$1" dns-udp filter INPUT -i "$ap" -s "$subnet" -d "${address%/*}" -p udp --dport 53 -j ACCEPT || failed=1
    firewall "$1" dns-tcp filter INPUT -i "$ap" -s "$subnet" -d "${address%/*}" -p tcp --dport 53 -j ACCEPT || failed=1
    return "$failed"
}

# Record ownership before a change so cleanup can also undo interrupted setup.
record() {
    local marker=$1
    shift
    touch -- "$runtime/$marker" || return
    "$@"
}

undo() {
    local marker=$1
    shift
    [[ -f "$runtime/$marker" ]] || return 0
    "$@" && rm -- "$runtime/$marker"
}

start_service() {
    local name=$1
    shift
    systemd-run --quiet --collect --unit="ventanal-bench-$name-$token" --service-type=forking \
        --property="PIDFile=$runtime/$name.pid" --property=TimeoutStartSec=15s \
        --property=TimeoutStopSec=15s --property=StandardOutput=null --property=StandardError=null \
        "$@" || { printf 'Could not start %s.\n' "$name" >&2; return 1; }
}

stop_service() {
    local unit="ventanal-bench-$1-$token.service" load_state
    if systemctl stop "$unit" 2>/dev/null; then
        return 0
    fi
    load_state=$(systemctl show -p LoadState --value "$unit" 2>/dev/null) || :
    [[ "$load_state" == not-found ]]
}

lro_enabled() {
    local features
    features=$(ethtool -k "$1") || return
    [[ "$features" =~ (^|$'\n')"large-receive-offload: on" ]] && printf '1\n' || printf '0\n'
}

set_forwarding() {
    if [[ -d "/sys/class/net/$1" ]]; then
        printf '%s\n' "$2" > "$forwarding/$1/forwarding"
    fi
}

restore_forwarding() {
    local enabled
    set_forwarding "$1" "$2" || return
    # The kernel can disable large receive offload when enabling forwarding.
    if [[ "$3" == 1 && -d "/sys/class/net/$1" ]]; then
        enabled=$(lro_enabled "$1") || return
        if [[ "$enabled" == 0 ]]; then
            ethtool -K "$1" lro on
        fi
    fi
}

remove_qdisc() {
    local queues
    [[ -d "/sys/class/net/$ap" ]] || return 0
    queues=$(tc qdisc show dev "$ap") || return
    if [[ "$queues" =~ (^|$'\n')"qdisc netem $qdisc_handle root " ]]; then
        tc qdisc del dev "$ap" root handle "$qdisc_handle"
    fi
}

remove_address() {
    local addresses
    [[ -d "/sys/class/net/$ap" ]] || return 0
    addresses=$(ip -o -4 addr show dev "$ap" to "$address") || return
    if [[ -n "$addresses" ]]; then
        ip address del "$address" dev "$ap"
    fi
}

set_ap_link() {
    if [[ -d "/sys/class/net/$ap" ]]; then
        ip link set dev "$ap" "$1"
    fi
}

down() {
    [[ -d "$runtime" ]] || return 0
    local state failed=0
    if [[ -f "$runtime/state" ]]; then
        mapfile -t state < "$runtime/state" || return
        ap=${state[0]} wired=${state[1]} token=${state[2]}
        rm -f -- "$runtime/profile" || return
        undo dnsmasq stop_service dnsmasq || failed=1
        undo hostapd stop_service hostapd || failed=1
        undo qdisc remove_qdisc || failed=1
        firewall_rules remove || failed=1
        undo forward-ap restore_forwarding "$ap" "${state[3]}" "${state[6]}" || failed=1
        undo forward-wired restore_forwarding "$wired" "${state[4]}" "${state[7]}" || failed=1
        undo address remove_address || failed=1
        undo link set_ap_link "${state[5]}" || failed=1
    fi
    if (( failed )); then
        printf 'Bench cleanup incomplete. Run bench.sh down again.\n' >&2
        return 1
    fi
    rm -r -- "$runtime"
}

rollback() {
    local result=$1
    if (( result )); then
        down || :
    fi
    exit "$result"
}

active() {
    [[ -s "$runtime/profile" ]] || return 1
    local state
    mapfile -t state < "$runtime/state"
    systemctl is-active --quiet "ventanal-bench-hostapd-${state[2]}.service" &&
        systemctl is-active --quiet "ventanal-bench-dnsmasq-${state[2]}.service"
}

up() {
    local profile=$1
    shift
    if active && [[ $(< "$runtime/profile") == "$profile" ]]; then
        return
    fi
    down
    read_config

    local queues queue_handle scope addresses routes ap_forward wired_forward ap_lro wired_lro link=down
    queues=$(tc qdisc show dev "$ap")
    while read -r _ _ queue_handle scope _; do
        if [[ "$scope" == root && "$queue_handle" != 0: ]]; then
            die "AP_IF already has a custom root queue."
        fi
    done <<< "$queues"
    addresses=$(ip -o -4 addr show dev "$ap")
    [[ -z "$addresses" ]] || die "AP_IF already has an IPv4 address."
    routes=$(ip -4 route show table all root "$subnet")
    [[ -z "$routes" ]] || die "The bench subnet is already in use."
    routes=$(ip -4 route show table all match "$subnet" | awk '$1 != "default" && $2 != "default"')
    [[ -z "$routes" ]] || die "An existing route covers the bench subnet."

    token=$(< /proc/sys/kernel/random/uuid)
    ap_forward=$(< "$forwarding/$ap/forwarding")
    wired_forward=$(< "$forwarding/$wired/forwarding")
    ap_lro=$(lro_enabled "$ap")
    wired_lro=$(lro_enabled "$wired")
    if (( $(< "/sys/class/net/$ap/flags") & 1 )); then
        link=up
    fi

    mkdir -- "$runtime"
    trap 'rollback "$?"' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM HUP
    printf '%s\n' "$ap" "$wired" "$token" "$ap_forward" "$wired_forward" "$link" "$ap_lro" "$wired_lro" > "$runtime/state.tmp"
    mv -- "$runtime/state.tmp" "$runtime/state"
    render_configs
    record link set_ap_link up
    record address ip address add "$address" dev "$ap"
    record forward-ap set_forwarding "$ap" 1
    record forward-wired set_forwarding "$wired" 1
    firewall_rules add
    record hostapd start_service hostapd hostapd -B -P "$runtime/hostapd.pid" "$runtime/hostapd.conf"
    record qdisc tc qdisc add dev "$ap" root handle "$qdisc_handle" netem "$@"
    record dnsmasq start_service dnsmasq dnsmasq --pid-file="$runtime/dnsmasq.pid" --conf-file="$runtime/dnsmasq.conf"
    printf '%s\n' "$profile" > "$runtime/profile"
}

usage() { die "Usage: bench.sh up baseline|worse, bench.sh down, or bench.sh status"; }

case "${1:-}" in
    up) [[ $# == 2 && ( "$2" == baseline || "$2" == worse ) ]] || usage ;;
    down|status) [[ $# == 1 ]] || usage ;;
    *) usage ;;
esac
(( EUID == 0 )) || die "Run bench.sh as root."
exec 9>> /run/ventanal-wifi-bench.lock
flock -x 9
case "$1:${2:-}" in
    up:baseline) up baseline loss 5% delay 300ms 100ms rate 256kbit ;;
    up:worse) up worse loss 15% delay 600ms ;;
    down:) down ;;
    status:) if active; then cat -- "$runtime/profile"; fi ;;
esac
