#!/bin/bash
# setup-eth-ssh.sh — Configure SSH access on a USB NIC (NOT the onboard NIC).
#
# The onboard NIC (eth0) is ALWAYS reserved for the car network.
# This script finds the first USB ethernet adapter (eth1, eth2, etc.)
# and configures it for SSH access with DHCP.
#
# Two modes:
#   dhcp-client — USB NIC gets IP from whatever is on the other end
#                 (e.g., laptop with ICS/WiFi sharing). Default.
#   dhcp-server — USB NIC runs its own DHCP server for direct laptop connection.
#
# Just plug a USB NIC into the SBC and connect to your laptop.

set -eu

# Load config
for f in /etc/openautolink.env /boot/firmware/openautolink.env; do
    [ -f "$f" ] && source "$f" 2>/dev/null || true
done

# Find the first USB ethernet adapter (not eth0, not usb0)
find_ssh_nic() {
    for iface in $(ls /sys/class/net/ 2>/dev/null); do
        [ "$iface" = "lo" ] && continue
        [ "$iface" = "eth0" ] && continue  # reserved for car network
        [ "$iface" = "usb0" ] && continue  # reserved for USB gadget
        [[ "$iface" == wlan* ]] && continue
        [[ "$iface" == docker* ]] && continue
        [[ "$iface" == veth* ]] && continue
        [[ "$iface" == dummy* ]] && continue
        # Check if it's an ethernet interface
        if [ -d "/sys/class/net/$iface" ]; then
            local devpath=$(readlink -f "/sys/class/net/$iface/device" 2>/dev/null || echo "")
            if [[ "$devpath" == *usb* ]] || [[ "$iface" == eth[1-9]* ]]; then
                echo "$iface"
                return 0
            fi
        fi
    done
    return 1
}

ETH_IFACE="${OAL_SSH_INTERFACE:-}"
ETH_IP="${OAL_SSH_IP:-10.0.0.1}"
ETH_SUBNET="${OAL_ETH_SUBNET:-255.255.255.0}"
ETH_DHCP_START="${OAL_ETH_DHCP_START:-10.0.0.10}"
ETH_DHCP_END="${OAL_ETH_DHCP_END:-10.0.0.50}"
DNSMASQ_PID="/var/run/openautolink-eth-dnsmasq.pid"
DNSMASQ_CONF="/tmp/openautolink-eth-dnsmasq.conf"

start() {
    # Auto-detect USB NIC if not configured
    if [ -z "$ETH_IFACE" ]; then
        ETH_IFACE=$(find_ssh_nic) || true
        if [ -z "$ETH_IFACE" ]; then
            echo "[ssh-nic] No USB ethernet adapter found. SSH via USB NIC not available."
            echo "[ssh-nic] Plug in a USB ethernet adapter, or use WiFi for SSH."
            exit 0
        fi
    fi

    echo "[ssh-nic] Setting up SSH access on ${ETH_IFACE}"
    echo "[ssh-nic] (eth0 is reserved for car network — not used for SSH)"

    # Don't conflict with NetworkManager or dhcpcd on this interface
    if command -v nmcli > /dev/null 2>&1; then
        nmcli device set "${ETH_IFACE}" managed no 2>/dev/null || true
    fi

    # Configure static IP on eth0
    ip link set "${ETH_IFACE}" up 2>/dev/null || true
    ip addr flush dev "${ETH_IFACE}" 2>/dev/null || true
    ip addr add "${ETH_IP}/${ETH_SUBNET}" dev "${ETH_IFACE}" 2>/dev/null || true

    # Start a dedicated dnsmasq instance for Ethernet DHCP only
    cat > "${DNSMASQ_CONF}" << EOF
# OpenAutoLink Ethernet SSH DHCP server
interface=${ETH_IFACE}
bind-interfaces
dhcp-range=${ETH_DHCP_START},${ETH_DHCP_END},${ETH_SUBNET},24h
# Don't interfere with DNS
port=0
# Log to syslog
log-facility=/var/log/openautolink-eth-dhcp.log
EOF

    # Kill any stale instance
    if [ -f "${DNSMASQ_PID}" ]; then
        kill "$(cat "${DNSMASQ_PID}")" 2>/dev/null || true
        rm -f "${DNSMASQ_PID}"
    fi

    dnsmasq -C "${DNSMASQ_CONF}" --pid-file="${DNSMASQ_PID}" || {
        echo "WARNING: dnsmasq for Ethernet failed. You can still use static IP ${ETH_IP}"
    }

    echo "Ethernet SSH ready: plug laptop into Ethernet, connect to ${ETH_IP}"
    echo "DHCP range: ${ETH_DHCP_START} - ${ETH_DHCP_END}"
}

stop() {
    if [ -f "${DNSMASQ_PID}" ]; then
        kill "$(cat "${DNSMASQ_PID}")" 2>/dev/null || true
        rm -f "${DNSMASQ_PID}"
    fi
    ip addr flush dev "${ETH_IFACE}" 2>/dev/null || true
}

case "${1:-start}" in
    start)  start ;;
    stop)   stop ;;
    *)      echo "Usage: $0 {start|stop}"; exit 1 ;;
esac
