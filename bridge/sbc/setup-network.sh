#!/bin/bash
# setup-network.sh — Unified network setup for OpenAutoLink bridge
#
# Handles the entire network topology in one script:
#   1. Determine which NIC is car network (onboard ethernet)
#   2. Assign static car IP to car NIC
#   3. Configure remaining NIC(s) for SSH with DHCP server
#
# The onboard ethernet NIC gets the car static IP, USB NICs get SSH/DHCP.
set -eu

# Load config
for f in /etc/openautolink.env /boot/firmware/openautolink.env; do
    [ -f "$f" ] && source "$f" 2>/dev/null || true
done

CAR_IP="${OAL_CAR_NET_IP:-192.168.222.222}"
CAR_MASK="${OAL_CAR_NET_MASK:-24}"
SSH_IP="${OAL_SSH_IP:-10.0.0.1}"
SSH_DHCP_START="${OAL_SSH_DHCP_START:-10.0.0.10}"
SSH_DHCP_END="${OAL_SSH_DHCP_END:-10.0.0.50}"
DNSMASQ_PID="/var/run/openautolink-ssh-dnsmasq.pid"
DNSMASQ_CONF="/tmp/openautolink-ssh-dnsmasq.conf"

# Track which NIC is assigned to what role
CAR_IFACE=""
SSH_IFACE=""

# ── NIC Detection ─────────────────────────────────────────────────────
# Auto-detect onboard ethernet: different SBCs use different names.
#   RPi:   eth0
#   Radxa: end0
#   Others: enp*, eno*, ens*
# USB NICs typically show as enx* (MAC-based name) or eth0/eth1 on DietPi.
# We distinguish by checking if the device lives on the USB bus (sysfs path
# contains "/usb") vs platform/PCI bus (onboard).

is_usb_nic() {
    local iface="$1"
    local devpath=$(readlink -f "/sys/class/net/$iface/device" 2>/dev/null)
    case "$devpath" in
        */usb*) return 0 ;;
        *)      return 1 ;;
    esac
}

find_onboard_nic() {
    # First pass: check common names, but verify they're NOT USB
    for name in eth0 end0; do
        if [ -d "/sys/class/net/$name" ] && ! is_usb_nic "$name"; then
            echo "$name"
            return 0
        fi
    done
    # Second pass: any non-lo, non-usb-bus, non-wlan interface
    for path in /sys/class/net/*; do
        local iface=$(basename "$path")
        case "$iface" in
            lo|usb*|wlan*|enx*) continue ;;
            *)
                if ! is_usb_nic "$iface"; then
                    echo "$iface"
                    return 0
                fi
                ;;
        esac
    done
    return 1
}

# Find USB/external NICs
find_usb_nics() {
    # enx* = MAC-based naming from udev (common on Ubuntu/Debian)
    for path in /sys/class/net/enx*; do
        [ -d "$path" ] && basename "$path"
    done
    # Also check eth* interfaces that are actually on the USB bus (DietPi)
    for path in /sys/class/net/eth*; do
        [ -d "$path" ] || continue
        local iface=$(basename "$path")
        if is_usb_nic "$iface"; then
            echo "$iface"
        fi
    done
}

ONBOARD_NIC=$(find_onboard_nic)
if [ -z "$ONBOARD_NIC" ]; then
    echo "[net] WARNING: Could not detect onboard NIC"
fi

# ── Helpers ───────────────────────────────────────────────────────────

assign_car_ip() {
    local iface="$1"
    echo "[net] Car network: $iface → $CAR_IP/$CAR_MASK"
    # Kill any DHCP client that DietPi/ifupdown may have started on this NIC
    pkill -f "dhclient.*$iface" 2>/dev/null || true
    pkill -f "dhcpcd.*$iface" 2>/dev/null || true
    ip addr flush dev "$iface" 2>/dev/null || true
    ip addr add "$CAR_IP/$CAR_MASK" dev "$iface" 2>/dev/null || true
    ip link set "$iface" up
    CAR_IFACE="$iface"
}

schedule_static_ssh_reconcile() {
    local iface="$1"
    local gw="$2"
    local script="/run/openautolink-ssh-reconcile.sh"

    cat > "$script" <<EOF
#!/bin/bash
set -eu

iface="$iface"
ssh_ip="$SSH_IP"
gw="$gw"

for _ in \
    \
    $(seq 1 300); do
    [ -d "/sys/class/net/\$iface" ] || {
        sleep 1
        continue
    }

    carrier=\$(cat "/sys/class/net/\$iface/carrier" 2>/dev/null || echo 0)
    has_ip=\$(ip -4 -o addr show dev "\$iface" | grep -F "\$ssh_ip/" || true)

    if [ "\$carrier" = "1" ] && [ -n "\$has_ip" ]; then
        exit 0
    fi

    if [ "\$carrier" = "1" ]; then
        ip addr flush dev "\$iface" 2>/dev/null || true
        ip addr add "\$ssh_ip/24" dev "\$iface" 2>/dev/null || true
        ip route replace default via "\$gw" dev "\$iface" 2>/dev/null || true
        {
            echo "nameserver \$gw"
            echo "nameserver 8.8.8.8"
        } > /etc/resolv.conf 2>/dev/null || true
        logger -t openautolink-network "[net] Reapplied static SSH IP \$ssh_ip on \$iface after late carrier"
        exit 0
    fi

    sleep 1
done

exit 0
EOF

    chmod 700 "$script"
    nohup "$script" >/dev/null 2>&1 &
}

setup_ssh_dhcp() {
    local iface="$1"
    SSH_IFACE="$iface"

    # Don't let NetworkManager manage this interface (we handle it)
    if command -v nmcli > /dev/null 2>&1; then
        nmcli device set "$iface" managed no 2>/dev/null || true
    fi

    ip link set "$iface" up

    # SSH NIC mode:
    #   dhcp-client (default) — get IP from whatever is on the other end
    #     (e.g., laptop sharing WiFi via ICS, which runs its own DHCP server)
    #   static — set a fixed IP, no DHCP client or server
    #     (use with laptop ICS when you want a known IP on the bridge)
    #   dhcp-server — run our own DHCP server for direct laptop connections
    #     (use when laptop has no DHCP server, just a plain ethernet cable)
    local ssh_mode="${OAL_SSH_MODE:-dhcp-client}"

    if [ "$ssh_mode" = "static" ]; then
        echo "[net] SSH network: $iface → $SSH_IP (static mode)"
        ip addr flush dev "$iface" 2>/dev/null || true
        ip addr add "$SSH_IP/24" dev "$iface" 2>/dev/null || true
        # Derive gateway from IP (assume .1 on same subnet)
        local gw=$(echo "$SSH_IP" | sed 's/\.[0-9]*$/.1/')
        ip route replace default via "$gw" dev "$iface" 2>/dev/null || true
        # Ensure DNS works
        echo "nameserver $gw" > /etc/resolv.conf 2>/dev/null || true
        echo "nameserver 8.8.8.8" >> /etc/resolv.conf 2>/dev/null || true
        schedule_static_ssh_reconcile "$iface" "$gw"
        echo "[net] SSH ready: connect to $SSH_IP (gateway $gw)"
    elif [ "$ssh_mode" = "dhcp-server" ]; then
        echo "[net] SSH network: $iface → $SSH_IP (DHCP server mode)"
        ip addr flush dev "$iface" 2>/dev/null || true
        ip addr add "$SSH_IP/24" dev "$iface" 2>/dev/null || true

        cat > "$DNSMASQ_CONF" << EOF
interface=$iface
bind-interfaces
dhcp-range=$SSH_DHCP_START,$SSH_DHCP_END,255.255.255.0,24h
port=0
log-facility=/var/log/openautolink-ssh-dhcp.log
EOF
        if [ -f "$DNSMASQ_PID" ]; then
            kill "$(cat "$DNSMASQ_PID")" 2>/dev/null || true
            rm -f "$DNSMASQ_PID"
        fi
        dnsmasq -C "$DNSMASQ_CONF" --pid-file="$DNSMASQ_PID" 2>/dev/null || {
            echo "[net] WARNING: dnsmasq failed. Use static IP $SSH_IP"
        }
        echo "[net] SSH ready: connect to $SSH_IP"
    else
        echo "[net] SSH network: $iface → DHCP client (getting IP from other end)"
        # Request IP via DHCP — works with laptop WiFi sharing (ICS), routers, etc.
        # Kill any stale DHCP clients on this interface first
        pkill -f "dhclient.*$iface" 2>/dev/null || true
        ip addr flush dev "$iface" 2>/dev/null || true
        if command -v dhclient > /dev/null 2>&1; then
            # Run dhclient as a daemon — it will keep retrying in background
            dhclient "$iface" 2>/dev/null &
        elif command -v dhcpcd > /dev/null 2>&1; then
            dhcpcd "$iface" --timeout 15 2>/dev/null &
        elif command -v udhcpc > /dev/null 2>&1; then
            udhcpc -i "$iface" -t 10 -n 2>/dev/null &
        else
            echo "[net] WARNING: No DHCP client found. SSH NIC has no IP."
            echo "[net] Install dhclient, dhcpcd, or udhcpc for automatic IP."
        fi
        echo "[net] SSH ready: check 'ip addr show $iface' for assigned IP"
    fi
}

wait_for_carrier() {
    local iface="$1"
    local timeout="$2"
    local elapsed=0
    while [ $elapsed -lt $timeout ]; do
        local carrier=$(cat /sys/class/net/"$iface"/carrier 2>/dev/null || echo 0)
        if [ "$carrier" = "1" ]; then
            echo "[net] $iface carrier UP after ${elapsed}s"
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    echo "[net] $iface no carrier after ${timeout}s"
    return 1
}

# ── Main Logic ────────────────────────────────────────────────────────

do_setup() {
    echo "[net] Onboard NIC (${ONBOARD_NIC:-?}) → car, USB NIC → SSH"
    if [ -n "$ONBOARD_NIC" ] && ip link show "$ONBOARD_NIC" &>/dev/null; then
        assign_car_ip "$ONBOARD_NIC"
    else
        echo "[net] ERROR: No onboard NIC found (tried: eth0, end0)"
        return 1
    fi
    # SSH on any USB NIC
    for iface in $(find_usb_nics); do
        setup_ssh_dhcp "$iface"
        break
    done

    echo "[net] Setup complete: car=$CAR_IFACE(${CAR_IP}) ssh=${SSH_IFACE:-none}"
}

do_teardown() {
    echo "[net] Tearing down network..."

    # Stop SSH DHCP
    if [ -f "$DNSMASQ_PID" ]; then
        kill "$(cat "$DNSMASQ_PID")" 2>/dev/null || true
        rm -f "$DNSMASQ_PID"
    fi

    echo "[net] Teardown complete"
}

# ── Entry Point ───────────────────────────────────────────────────────

case "${1:-setup}" in
    setup)   do_setup ;;
    teardown) do_teardown ;;
    *)       echo "Usage: $0 {setup|teardown}"; exit 1 ;;
esac
