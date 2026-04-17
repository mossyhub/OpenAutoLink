# OpenAutoLink Bridge -- SBC Build & Deployment Guide

## Overview

The bridge relay (`openautolink-relay`) is a ~340-line C++ binary with **zero external
dependencies**. It splices TCP sockets between the phone and the car app -- all Android
Auto protocol processing happens inside the AAOS app via aasdk NDK/JNI.

## Quick Install (Fresh SBC)

```bash
curl -fsSL https://raw.githubusercontent.com/mossyhub/openautolink/main/bridge/sbc/install.sh | sudo bash
```

This installs the relay binary, scripts, config, and systemd services. See the
[main README](../../README.md) for hardware requirements and full setup instructions.

## Building from Source

### Prerequisites (WSL or native Linux)

Only two packages:
```bash
sudo apt install g++-14-aarch64-linux-gnu cmake
```

### Build

```bash
cd /path/to/openautolink
mkdir -p build-relay-arm64 && cd build-relay-arm64
cmake ../bridge/openautolink/relay \
    -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc-14 \
    -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++-14
make -j$(nproc)
aarch64-linux-gnu-strip -o openautolink-relay-stripped openautolink-relay
```

Output: `openautolink-relay-stripped` (~67KB)

### Deploy

```bash
scp openautolink-relay-stripped openautolink:/opt/openautolink/bin/openautolink-relay
ssh openautolink "sudo systemctl restart openautolink"
```

Or use the deploy script from Windows:
```powershell
scripts\deploy-bridge.ps1
```

## Architecture

```
Phone:5277 <-- raw bytes --> App:5291   (relay splice -- poll() loop)
App:5288   <-- JSON lines --> control   (signaling + diagnostics)
```

### TCP Ports

| Port | Direction | Purpose |
|------|-----------|---------|
| 5288 | App -> Bridge | Control channel (JSON lines): hello, relay_ready, paired_phones, diagnostics |
| 5291 | App -> Bridge | Relay: raw byte splice to phone's AA connection |
| 5277 | Phone -> Bridge | Phone's AA TCP connection (after BT pairing + WiFi join) |

### What the relay does NOT do

- No AA protocol processing (no TLS, no protobuf, no service discovery)
- No video/audio decoding or encoding
- No config file generation or env var management for AA settings
- No OpenSSL, no Boost, no protobuf, no aasdk

## Configuration

All settings are in `/etc/openautolink.env`. The relay itself needs no AA configuration --
all AA settings (resolution, codec, DPI, FPS) are owned by the car app and passed directly
to aasdk via JNI at session start.

The env file configures:
- Network mode (external-nic / USB gadget)
- Car network IP (default: 192.168.222.222)
- SSH access mode
- WiFi AP settings (band, channel, SSID, password)
- Bluetooth MAC and default phone
- mDNS discovery

## Systemd Services

| Service | Purpose |
|---------|---------|
| `openautolink.service` | Relay binary (TCP:5288/5291/5277) |
| `openautolink-network.service` | Car + SSH network setup |
| `openautolink-wireless.service` | WiFi AP (hostapd) |
| `openautolink-bt.service` | BT pairing (BLE + HSP + RFCOMM) |

## Troubleshooting

```bash
# Check relay status
systemctl status openautolink

# View relay logs
journalctl -u openautolink -f --no-pager

# Check listening ports
ss -tlnp | grep -E '5277|5288|5291'

# Restart relay
sudo systemctl restart openautolink

# Check all services
systemctl status 'openautolink-*'
```
