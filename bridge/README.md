# Bridge

The bridge relay (`openautolink-relay`) is a thin C++ binary that splices TCP sockets
between the phone and the car app. It does **zero** AA protocol processing -- all
Android Auto handling (TLS, protobuf, video, audio) runs inside the AAOS app via
aasdk NDK/JNI.

## Architecture

```
Phone:5277 <-- raw bytes --> App:5291   (relay splice)
App:5288   <-- JSON lines --> control   (signaling + diagnostics)
```

- **Relay binary**: `relay/src/main.cpp` -- ~340 lines, zero external dependencies, 67KB stripped
- **BT/WiFi pairing**: `scripts/aa_bt_all.py` -- BLE advertising, BT pairing, HSP, RFCOMM WiFi credential exchange
- **SBC deployment**: `sbc/` -- systemd services, env config, install script

### TCP Ports

| Port | Purpose |
|------|---------|
| 5288 | Control channel: JSON lines signaling (`hello`, `relay_ready`, `relay_disconnected`, `paired_phones`, diagnostics) |
| 5291 | Relay: raw byte splice between app and phone |
| 5277 | Phone listener: accepts phone's AA TCP connection |

### Key Directories

| Path | Purpose |
|------|---------|
| `openautolink/relay/` | Relay binary source (C++) |
| `openautolink/scripts/` | BT/WiFi pairing service (Python) |
| `sbc/` | Systemd services, env config, install script, build guide |

## Build

See [sbc/BUILD.md](sbc/BUILD.md) for build and deployment instructions.

## Configuration

All settings in `/etc/openautolink.env`. The relay needs no AA configuration -- all
AA settings (resolution, codec, DPI) are owned by the car app.

The env file configures network, WiFi AP, Bluetooth, and SSH access.
See [sbc/openautolink.env](sbc/openautolink.env) for all available settings.
