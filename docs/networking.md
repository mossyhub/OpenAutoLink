# OpenAutoLink — Network Architecture

## Overview

The OpenAutoLink bridge has **three network connections**, each serving a different purpose:

```
┌─────────────────────────────────────────────────────────────────┐
│                    OpenAutoLink Bridge (SBC)                     │
│                                                                 │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐  │
│  │   Phone ↔    │  │   Car  ↔     │  │   SSH/Management     │  │
│  │   Bridge     │  │   Bridge     │  │   (optional)         │  │
│  │              │  │              │  │                      │  │
│  │  WiFi (wlan0)│  │  USB gadget  │  │  USB NIC (eth1+)     │  │
│  │  or USB host │  │  (usb0)      │  │  or onboard (eth0)   │  │
│  │              │  │  or onboard  │  │  if car uses USB     │  │
│  │  TCP :5277   │  │  (eth0)      │  │  gadget              │  │
│  │              │  │              │  │                      │  │
│  │  BT pairing  │  │  TCP :5288   │  │  DHCP client         │  │
│  │  + WiFi AP   │  │  TCP :5290   │  │  (laptop WiFi share) │  │
│  │              │  │  TCP :5289   │  │                      │  │
│  └──────────────┘  └──────────────┘  └──────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

## Network Roles

### 1. Phone → Bridge (Android Auto stream)

**How the phone connects:**
- **Wireless** (default): Phone pairs via Bluetooth, joins bridge's WiFi AP, connects AA over TCP port 5277
- **Wired USB**: Phone plugs into bridge USB host port, uses AOA protocol

**Configuration:**
```bash
OAL_PHONE_MODE=wireless    # "wireless" or "usb"
OAL_PHONE_TCP_PORT=5277    # TCP port for wireless AA (default 5277)
```

### 2. Bridge → Car (OAL protocol relay)

**Three TCP ports** for the car app connection:

| Port | Purpose | Direction | Format |
|------|---------|-----------|--------|
| 5288 | Control | Bidirectional | JSON lines |
| 5290 | Video | Bridge → App | Binary (12B header + codec) |
| 5289 | Audio | Bidirectional | Binary (8B header + PCM) |

See [protocol.md](protocol.md) for full wire format.

The connection can be over:
- **USB Gadget** — SBC presents as a USB network device (RNDIS/ECM/NCM) when plugged into car's USB port
- **External NIC** — Separate USB ethernet adapter plugged into car's USB, ethernet cable to SBC's onboard RJ45

**Configuration:**
```bash
OAL_CAR_NET_MODE=auto       # "auto" (recommended), "usb-gadget", or "external-nic"
OAL_CAR_NET_IP=192.168.222.222    # Static IP for bridge on car network
OAL_CAR_NET_MASK=24               # Subnet mask
OAL_CAR_TCP_PORT=5288             # Control port (video=5290, audio=5289 are fixed)
OAL_CAR_NET_PROTO=rndis           # USB gadget protocol: "rndis", "ecm", "ncm"
OAL_CAR_NET_UDISK=1               # Include mass storage in gadget (GM EVs need this)
OAL_CAR_NET_GADGET_TIMEOUT=10     # Seconds to wait for car to detect USB gadget
```

### 3. SSH/Management (optional)

**For development and debugging only.** The NIC that is NOT used for the car network becomes available for SSH access.

**Configuration:**
```bash
OAL_SSH_MODE=dhcp-client    # "dhcp-client" (default) or "dhcp-server"
OAL_SSH_IP=10.0.0.1         # Only used in dhcp-server mode
```

---

## Auto Mode (Recommended)

`OAL_CAR_NET_MODE=auto` handles all scenarios automatically:

```
Boot → Create USB gadget
     → Wait for car to detect it (carrier UP)
     ├── YES: usb0 = car network (192.168.222.222)
     │        eth0 = SSH (DHCP client from laptop)
     │
     └── NO (timeout): USB gadget failed
              eth0 = car network (192.168.222.222)
              USB NIC = SSH (if connected)
```

---

## USB Gadget Protocols

| Protocol | `OAL_CAR_NET_PROTO` | Compatibility | Notes |
|----------|---------------------|---------------|-------|
| **RNDIS** | `rndis` | Android, Windows | Default. Most compatible |
| CDC-ECM | `ecm` | Linux | Standard USB ethernet |
| CDC-NCM | `ncm` | Modern Linux | Better throughput |

### GM-Specific: UDisk (Mass Storage)

GM EVs reject USB devices without mass storage. `OAL_CAR_NET_UDISK=1` adds a 1MB FAT image as a second USB function, preventing the "unsupported device" popup.

---

## IP Addressing

| Network | IP | Subnet | Who |
|---------|-----|--------|-----|
| Car network | `192.168.222.222` | `/24` | Bridge (static) |
| Car network | `192.168.222.108` | `/24` | Car (DHCP assigned) |
| Phone WiFi | `192.168.43.1` | `/24` | Bridge AP |
| Phone WiFi | `192.168.43.x` | `/24` | Phone (DHCP from dnsmasq) |
| SSH | varies | `/24` | DHCP from laptop |
