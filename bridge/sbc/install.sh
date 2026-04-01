#!/bin/bash
# install.sh — One-shot setup for OpenAutoLink on any Linux SBC
# Tested: Raspberry Pi CM5, ROCK 3A, VIM4 (any ARM64 with WiFi+BT)
#
# Usage: sudo bash install.sh
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_DIR="/opt/openautolink"

# Find source directories (repo layout or tarball)
if [ -d "${SCRIPT_DIR}/../../external/aasdk" ]; then
    REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
    AASDK_DIR="${REPO_ROOT}/external/aasdk"
    HEADLESS_DIR="${REPO_ROOT}/bridge/openautolink/headless"
elif [ -d "${SCRIPT_DIR}/../external/aasdk" ]; then
    REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
    AASDK_DIR="${REPO_ROOT}/external/aasdk"
    HEADLESS_DIR="${REPO_ROOT}/headless"
else
    echo "ERROR: Cannot find aasdk source." >&2
    exit 1
fi

echo "=== OpenAutoLink Installer ==="
echo "Install dir: ${INSTALL_DIR}"
echo ""

# 1. System packages
echo ">>> Installing system packages..."
apt-get update -qq
apt-get install -y -qq \
    cmake g++ make \
    libboost-system-dev libboost-log-dev \
    libprotobuf-dev protobuf-compiler \
    libssl-dev libusb-1.0-0-dev \
    hostapd dnsmasq \
    bluez python3-dbus python3-gi \
    avahi-daemon avahi-utils
echo ""

# 2. Deploy files
echo ">>> Deploying to ${INSTALL_DIR}..."
mkdir -p "${INSTALL_DIR}/bin" "${INSTALL_DIR}/scripts"

cp "${SCRIPT_DIR}/setup-car-net.sh" "${INSTALL_DIR}/"
cp "${SCRIPT_DIR}/run-openautolink.sh" "${INSTALL_DIR}/"
cp "${SCRIPT_DIR}/start-wireless.sh" "${INSTALL_DIR}/" 2>/dev/null || true
chmod +x "${INSTALL_DIR}/setup-car-net.sh" "${INSTALL_DIR}/run-openautolink.sh" "${INSTALL_DIR}/start-wireless.sh" 2>/dev/null || true

if [ -d "${SCRIPT_DIR}/../openautolink/scripts" ]; then
    cp "${SCRIPT_DIR}/../openautolink/scripts/aa_bt_all.py" "${INSTALL_DIR}/scripts/" 2>/dev/null || true
fi

if [ ! -f /etc/openautolink.env ]; then
    cp "${SCRIPT_DIR}/openautolink.env" /etc/openautolink.env
    echo "  Created /etc/openautolink.env"
fi

if [ -d "${SCRIPT_DIR}/../openautolink/headless/avahi" ]; then
    cp "${SCRIPT_DIR}/../openautolink/headless/avahi/openautolink.service" \
       /etc/avahi/services/ 2>/dev/null || true
fi
echo ""

# 3. Install headless binary (prebuilt or build from source)
echo ">>> Installing headless binary..."
ARCH=$(uname -m)
PREBUILT="${SCRIPT_DIR}/prebuilt/${ARCH}/openautolink-headless"

if [ -f "$PREBUILT" ]; then
    cp "$PREBUILT" "${INSTALL_DIR}/bin/openautolink-headless"
    chmod +x "${INSTALL_DIR}/bin/openautolink-headless"
    echo "  Installed prebuilt binary for ${ARCH}"
elif [ -d "${AASDK_DIR}" ] && [ -f "${HEADLESS_DIR}/CMakeLists.txt" ]; then
    echo "  No prebuilt found, building from source..."
    BUILD_DIR="/tmp/openautolink-build"
    rm -rf "${BUILD_DIR}"
    cmake -S "${HEADLESS_DIR}" -B "${BUILD_DIR}" \
        -DPI_AA_AASDK_SOURCE_DIR="${AASDK_DIR}" \
        -DPI_AA_ENABLE_AASDK_LIVE=ON \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "${BUILD_DIR}" -j$(nproc)
    cp "${BUILD_DIR}/openautolink-headless" "${INSTALL_DIR}/bin/"
    echo "  Installed openautolink-headless"
fi
echo ""

# 4. USB gadget kernel support (platform-specific)
echo ">>> Configuring USB gadget support..."
source /etc/openautolink.env 2>/dev/null || true
if [ "${OAL_CAR_NET_MODE:-usb-gadget}" = "usb-gadget" ]; then
    # Detect platform and configure USB gadget overlay
    if [ -f /boot/firmware/config.txt ]; then
        # Raspberry Pi (CM4, CM5, Pi 4, Pi Zero 2 W)
        grep -q "^dtoverlay=dwc2" /boot/firmware/config.txt || \
            echo "dtoverlay=dwc2,dr_mode=peripheral" >> /boot/firmware/config.txt
        echo "  Raspberry Pi: dwc2 overlay configured"
    elif [ -f /boot/config.txt ]; then
        # Older Raspberry Pi OS layout
        grep -q "^dtoverlay=dwc2" /boot/config.txt || \
            echo "dtoverlay=dwc2,dr_mode=peripheral" >> /boot/config.txt
        echo "  Raspberry Pi (legacy): dwc2 overlay configured"
    else
        echo "  Non-RPi platform detected."
        echo "  USB gadget may need manual DT configuration for your SBC."
        echo "  Supported SBCs with USB OTG:"
        echo "    - Raspberry Pi CM4/CM5, Pi 4B, Pi Zero 2 W (dwc2)"
        echo "    - Rockchip RK3568/RK3588 boards (dwc3)"
        echo "    - Amlogic S905/A311D boards (crgudc2)"
        echo "  Check your board's docs for 'dr_mode=peripheral' or OTG configuration."
        echo "  Or use OAL_CAR_NET_MODE=external-nic to avoid USB gadget entirely."
    fi
    for mod in libcomposite usb_f_ecm usb_f_mass_storage; do
        grep -q "^${mod}$" /etc/modules 2>/dev/null || echo "$mod" >> /etc/modules
    done
    echo "  Kernel modules configured"
else
    echo "  Skipped (external-nic mode)"
fi
echo ""

# 5. Hostname + mDNS
hostnamectl set-hostname openautolink 2>/dev/null || true
grep -q "openautolink" /etc/hosts || echo "127.0.1.1 openautolink" >> /etc/hosts

# 6. systemd services
echo ">>> Installing systemd services..."
cp "${SCRIPT_DIR}/openautolink-car-net.service" /etc/systemd/system/
cp "${SCRIPT_DIR}/openautolink.service" /etc/systemd/system/
cp "${SCRIPT_DIR}/openautolink-wireless.service" /etc/systemd/system/ 2>/dev/null || true
[ -f "${SCRIPT_DIR}/openautolink-bt.service" ] && \
    cp "${SCRIPT_DIR}/openautolink-bt.service" /etc/systemd/system/
[ -f "${SCRIPT_DIR}/openautolink-eth-ssh.service" ] && \
    cp "${SCRIPT_DIR}/openautolink-eth-ssh.service" /etc/systemd/system/

systemctl daemon-reload
systemctl disable pi-aa-bridge pi-aa-gadget pi-aa-headless pi-aa-tcp pi-aa-wireless pi-aa-bt 2>/dev/null || true
systemctl enable openautolink-car-net openautolink openautolink-wireless openautolink-bt 2>/dev/null || true

echo ""
echo "=== Installation complete ==="
echo "Config: /etc/openautolink.env"
echo "Mode:   ${OAL_CAR_NET_MODE:-usb-gadget}"
echo "Reboot to activate."
