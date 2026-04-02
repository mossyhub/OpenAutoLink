#!/bin/bash
# start-wireless.sh — OpenAutoLink WiFi AP with auto-capability detection
#
# Probes the WiFi hardware and picks the best configuration:
#   1. Checks if 5GHz is supported
#   2. Checks available channels (DFS-free preferred)
#   3. Checks VHT/HE support for 80MHz bandwidth
#   4. Falls back gracefully: 5GHz→2.4GHz, VHT→HT, 80MHz→40→20
#
# The app can request a band preference (OAL_WIRELESS_BAND=5ghz|24ghz)
# but this script respects it only if the hardware supports it.

set -u

# Load config
for f in /etc/openautolink.env /boot/firmware/openautolink.env; do
    [ -f "$f" ] && source "$f"
done

if [ "${OAL_WIRELESS_ENABLE:-0}" != "1" ]; then
    echo "[wifi] Wireless disabled in config"
    exit 0
fi

IFACE="${OAL_WIRELESS_INTERFACE:-}"
if [ -z "$IFACE" ]; then
    # Auto-detect: find the first wireless interface
    for path in /sys/class/net/*/wireless; do
        [ -d "$path" ] && IFACE=$(basename "$(dirname "$path")") && break
    done
    if [ -z "$IFACE" ]; then
        echo "[wifi] ERROR: No wireless interface found"
        echo "[wifi] Set OAL_WIRELESS_INTERFACE in /etc/openautolink.env"
        exit 1
    fi
    echo "[wifi] Auto-detected wireless interface: $IFACE"
fi
BAND_PREF="${OAL_WIRELESS_BAND:-5ghz}"
CHANNEL_OVERRIDE="${OAL_WIRELESS_CHANNEL:-}"
SSID="${OAL_WIRELESS_SSID:-}"
PASSWORD="${OAL_WIRELESS_PASSWORD:-}"
COUNTRY="${OAL_WIRELESS_COUNTRY:-US}"

# ── Auto-generate SSID/password if not set ────────────────────────────
if [ -z "$SSID" ]; then
    _PREFIX="${OAL_WIRELESS_SSID_PREFIX:-OpenAutoLink}"
    _MAC=$(cat /sys/class/net/${IFACE}/address 2>/dev/null | tr -d ':' | tail -c 5 | tr 'a-f' 'A-F')
    SSID="${_PREFIX}-${_MAC:-0000}"
fi
if [ -z "$PASSWORD" ]; then
    PASSWORD="$(cat /dev/urandom | tr -dc 'A-Za-z0-9' | head -c 16)"
    echo "WiFi password: ${PASSWORD}" > /opt/openautolink/wifi-password.txt
fi

# ── Probe hardware capabilities ──────────────────────────────────────
echo "[wifi] Probing ${IFACE} capabilities..."

killall -q hostapd dnsmasq 2>/dev/null || true
ip link set "${IFACE}" down 2>/dev/null || true
ip addr flush dev "${IFACE}" 2>/dev/null || true
ip addr add 192.168.43.1/24 dev "${IFACE}" 2>/dev/null || true
ip link set "${IFACE}" up 2>/dev/null || true
iw reg set "$COUNTRY" 2>/dev/null || true

# Disable power save to prevent signal drops during AA sessions
iw dev "${IFACE}" set power_save off 2>/dev/null || true
sleep 1

# Check supported bands
PHY=$(iw dev "${IFACE}" info 2>/dev/null | grep wiphy | awk '{print $2}')
SUPPORTS_5GHZ=false
SUPPORTS_VHT=false
SUPPORTS_HE=false
AVAILABLE_5G_CHANNELS=""

if [ -n "$PHY" ]; then
    PHY_INFO=$(iw phy "phy${PHY}" info 2>/dev/null)

    if echo "$PHY_INFO" | grep -q "5[12][0-9][0-9].*MHz"; then
        SUPPORTS_5GHZ=true
        # Find usable 5GHz channels:
        # - Must be in 5150-5885 MHz range (exclude 6GHz channels that start at 5955)
        # - Must not be disabled
        # - Must not require radar detection (DFS)
        # Channels with "no IR" are okay — hostapd with ieee80211d=1 can override
        AVAILABLE_5G_CHANNELS=$(echo "$PHY_INFO" | \
            grep -E '^\s+\* 5[12348][0-9]{2}\.0 MHz \[' | \
            grep -v "disabled" | \
            grep -v "radar" | \
            sed 's/.*\[\([0-9]*\)\].*/\1/' | tr '\n' ' ')
        echo "[wifi] 5GHz supported. Usable channels: ${AVAILABLE_5G_CHANNELS:-none}"
    fi

    if echo "$PHY_INFO" | grep -qi "VHT"; then
        SUPPORTS_VHT=true
        echo "[wifi] VHT (802.11ac) supported"
    fi

    if echo "$PHY_INFO" | grep -qi "HE Phy\|HE MAC"; then
        SUPPORTS_HE=true
        echo "[wifi] HE (802.11ax/WiFi 6) supported"
    fi
else
    echo "[wifi] WARNING: Cannot determine PHY info for ${IFACE}"
fi

# ── Select channel and mode ──────────────────────────────────────────
USE_5GHZ=false
CHANNEL=6
HW_MODE="g"

if [ -n "$CHANNEL_OVERRIDE" ]; then
    # User explicitly set a channel
    CHANNEL="$CHANNEL_OVERRIDE"
    if [ "$CHANNEL" -ge 36 ] 2>/dev/null; then
        USE_5GHZ=true
        HW_MODE="a"
    fi
    echo "[wifi] Using override channel $CHANNEL"
elif [ "$BAND_PREF" = "5ghz" ] && [ "$SUPPORTS_5GHZ" = true ]; then
    USE_5GHZ=true
    HW_MODE="a"
    # Pick best DFS-free channel (prefer 149 for maximum compatibility)
    if echo "$AVAILABLE_5G_CHANNELS" | grep -q "149"; then
        CHANNEL=149
    elif echo "$AVAILABLE_5G_CHANNELS" | grep -q "36"; then
        CHANNEL=36
    elif echo "$AVAILABLE_5G_CHANNELS" | grep -q "44"; then
        CHANNEL=44
    elif [ -n "$AVAILABLE_5G_CHANNELS" ]; then
        CHANNEL=$(echo "$AVAILABLE_5G_CHANNELS" | awk '{print $1}')
    else
        echo "[wifi] No DFS-free 5GHz channels, falling back to 2.4GHz"
        USE_5GHZ=false
        HW_MODE="g"
        CHANNEL=6
    fi
    [ "$USE_5GHZ" = true ] && echo "[wifi] Selected 5GHz channel $CHANNEL"
elif [ "$BAND_PREF" = "24ghz" ] || [ "$SUPPORTS_5GHZ" = false ]; then
    echo "[wifi] Using 2.4GHz (band preference or no 5GHz support)"
    CHANNEL=6
fi

# ── Compute VHT center frequency ─────────────────────────────────────
CENTER_IDX=0
if [ "$USE_5GHZ" = true ] && [ "$SUPPORTS_VHT" = true ]; then
    if [ "$CHANNEL" -le 48 ]; then CENTER_IDX=42;
    elif [ "$CHANNEL" -le 64 ]; then CENTER_IDX=58;
    elif [ "$CHANNEL" -le 144 ]; then CENTER_IDX=138;
    else CENTER_IDX=155; fi
fi

# ── Generate hostapd config ──────────────────────────────────────────
CONF="/opt/openautolink/hostapd.conf"

if [ "$USE_5GHZ" = true ]; then
    cat > "$CONF" << EOF
interface=${IFACE}
driver=nl80211
ssid=${SSID}
country_code=${COUNTRY}
ieee80211d=1
ieee80211h=1
hw_mode=a
channel=${CHANNEL}
ieee80211n=1
ht_capab=[HT40+][SHORT-GI-20][SHORT-GI-40]
wpa=2
wpa_passphrase=${PASSWORD}
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
wmm_enabled=1
EOF

    # Add VHT (802.11ac) if supported
    if [ "$SUPPORTS_VHT" = true ] && [ "$CENTER_IDX" -gt 0 ]; then
        cat >> "$CONF" << EOF
ieee80211ac=1
vht_oper_chwidth=1
vht_oper_centr_freq_seg0_idx=${CENTER_IDX}
vht_capab=[SHORT-GI-80][SU-BEAMFORMEE]
EOF
        echo "[wifi] 802.11ac VHT80 enabled (center=$CENTER_IDX)"
    fi
else
    # 2.4GHz config — maximize throughput with HT40 if available
    cat > "$CONF" << EOF
interface=${IFACE}
driver=nl80211
ssid=${SSID}
country_code=${COUNTRY}
ieee80211d=1
hw_mode=g
channel=${CHANNEL}
ieee80211n=1
ht_capab=[HT40+][SHORT-GI-20][SHORT-GI-40][RX-STBC1]
wmm_enabled=1
wpa=2
wpa_passphrase=${PASSWORD}
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
EOF
fi

# ── Start hostapd with fallback ──────────────────────────────────────
echo "[wifi] Starting hostapd: SSID=${SSID} CH=${CHANNEL} MODE=${HW_MODE}"

if ! hostapd -B "$CONF"; then
    if [ "$USE_5GHZ" = true ]; then
        echo "[wifi] 5GHz hostapd failed, falling back to 2.4GHz channel 6"
        cat > "$CONF" << EOF
interface=${IFACE}
driver=nl80211
ssid=${SSID}
country_code=${COUNTRY}
ieee80211d=1
hw_mode=g
channel=6
ieee80211n=1
ht_capab=[HT40+][SHORT-GI-20][SHORT-GI-40][RX-STBC1]
wmm_enabled=1
wpa=2
wpa_passphrase=${PASSWORD}
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
EOF
        hostapd -B "$CONF" || echo "[wifi] ERROR: hostapd failed on both bands"
    else
        echo "[wifi] ERROR: hostapd failed"
    fi
fi

# ── Start DHCP ───────────────────────────────────────────────────────
cat > /opt/openautolink/dnsmasq.conf << EOF
interface=${IFACE}
dhcp-range=192.168.43.10,192.168.43.50,255.255.255.0,24h
bind-interfaces
EOF
dnsmasq -C /opt/openautolink/dnsmasq.conf || true

# ── Enable Bluetooth ─────────────────────────────────────────────────
bluetoothctl power on 2>/dev/null || true
bluetoothctl discoverable on 2>/dev/null || true
bluetoothctl pairable on 2>/dev/null || true
bluetoothctl discoverable-timeout 0 2>/dev/null || true

# ── Persist generated credentials for BT script ─────────────────────
# The BT script (aa_bt_all.py) reads SSID/password from env to send
# to the phone during the RFCOMM WiFi credential exchange. If these are
# empty in the env, the phone won't know which WiFi to join.
if [ -f /etc/openautolink.env ]; then
    sed -i "s/^OAL_WIRELESS_SSID=.*/OAL_WIRELESS_SSID=${SSID}/" /etc/openautolink.env 2>/dev/null || true
    sed -i "s/^OAL_WIRELESS_PASSWORD=.*/OAL_WIRELESS_PASSWORD=${PASSWORD}/" /etc/openautolink.env 2>/dev/null || true
    echo "[wifi] Persisted SSID=${SSID} to env"
fi

echo "[wifi] Wireless ready: SSID=${SSID} CH=${CHANNEL} BAND=${HW_MODE} IP=192.168.43.1"
