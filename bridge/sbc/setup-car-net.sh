#!/bin/bash
# setup-car-net.sh — Configure car-facing network for OpenAutoLink
#
# The SBC connects to the car via a USB Ethernet NIC plugged into the car's USB port.
# The onboard NIC (eth0) gets a static IP for the car network.
# SSH/management access uses a second NIC (eth1+) or WiFi.
set -eu

# Load config
for f in /etc/openautolink.env /boot/firmware/openautolink.env; do
    [ -f "$f" ] && source "$f" 2>/dev/null || true
done

CAR_IP="${OAL_CAR_NET_IP:-192.168.222.222}"
CAR_MASK="${OAL_CAR_NET_MASK:-24}"

# Helper: assign static IP to an interface
assign_static_ip() {
    local iface="$1"
    echo "[car-net] Assigning $CAR_IP/$CAR_MASK to $iface"
    ip addr flush dev "$iface" 2>/dev/null || true
    ip addr add "$CAR_IP/$CAR_MASK" dev "$iface" 2>/dev/null || true
    ip link set "$iface" up
    echo "[car-net] $iface configured: $(ip -4 addr show "$iface" 2>/dev/null | grep inet | head -1)"
}

case "${1:-setup}" in

setup)
    echo "[car-net] External NIC mode - onboard eth0 is the car network"
    IFACE="eth0"
    if ip link show "$IFACE" &>/dev/null; then
        ip addr flush dev "$IFACE" 2>/dev/null || true
        assign_static_ip "$IFACE"
        echo "[car-net] eth0 locked to car network ($CAR_IP)"
        echo "[car-net] SSH access: use USB NIC (eth1+) or WiFi"
    else
        IFACE=$(ip -o link show type ether | grep -v 'usb\|lo\|docker\|veth' | awk -F': ' '{print $2}' | head -1)
        if [ -n "$IFACE" ]; then
            ip addr flush dev "$IFACE" 2>/dev/null || true
            assign_static_ip "$IFACE"
            echo "[car-net] WARNING: eth0 not found, using $IFACE for car network"
        else
            echo "[car-net] ERROR: No onboard ethernet interface found"
            exit 1
        fi
    fi
    ;;

teardown)
    echo "[car-net] Nothing to tear down (external NIC mode)"
    ;;

*)
    echo "Usage: $0 setup|teardown"
    exit 1
    ;;
esac