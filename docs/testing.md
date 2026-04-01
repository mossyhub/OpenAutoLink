# OpenAutoLink — Local Testing Guide

## Overview

Full end-to-end testing requires the AAOS app talking to the bridge over a real network. Since the GM head unit has **no ADB access** due to GM restrictions, we use the Android SDK AAOS emulator as a stand-in. The bridge runs on a physical SBC connected to the development PC via two separate network cables.

```
┌──────────────────┐         ┌─────────────────────────────────────┐
│  Dev PC (Windows) │         │          SBC (Bridge)               │
│                   │         │                                     │
│  AAOS Emulator    │◄──eth──►│  Onboard NIC (eth0)                │
│  (192.168.222.108)│  bridge │  Static IP: 192.168.222.222        │
│                   │  traffic│  Ports: 5288, 5289, 5290            │
│                   │         │                                     │
│  PC NIC / USB-NIC │◄──eth──►│  USB NIC (eth1+)                   │
│  (DHCP / static)  │   SSH   │  SSH access                        │
│                   │         │                                     │
│  adb → emulator   │         │  Phone ── WiFi ──► wlan0 (5277)    │
└──────────────────┘         └─────────────────────────────────────┘
```

## Prerequisites

- Android SDK with emulator and platform-tools installed
- AAOS emulator AVD: **`BlazerEV_AAOS`** (Android 33 Automotive, x86_64, 2400×960 landscape)
- SBC with bridge built and running (see [bridge/sbc/BUILD.md](../bridge/sbc/BUILD.md))
- Two ethernet cables
- One USB ethernet adapter (for the SBC's SSH connection)
- A phone with Android Auto for full end-to-end tests

## 1. Hardware Setup

### Two-Cable Network Connection

The SBC requires **two separate network connections** to your PC:

| Cable | SBC Side | PC Side | Purpose |
|-------|----------|---------|---------|
| **Cable 1 — Bridge traffic** | **Onboard NIC** (eth0, RJ45 on the board) | Dedicated NIC or USB ethernet adapter on PC | OAL protocol (control/video/audio) |
| **Cable 2 — SSH** | **USB NIC** (USB ethernet adapter plugged into SBC) | Any available NIC on PC | SSH access for development |

> **Why two cables?** The onboard NIC is locked to the car network subnet (192.168.222.0/24) with a static IP. Mixing SSH and bridge traffic on one interface creates routing conflicts and doesn't match the real car topology.

### IP Addressing

This mirrors the real car environment:

| Device | IP | Role |
|--------|-----|------|
| SBC onboard NIC (eth0) | `192.168.222.222` | Bridge — already configured by `setup-car-net.sh` |
| Emulator (acting as head unit) | `192.168.222.108` | App — matches the IP the GM head unit assigns to its USB NIC |
| SBC USB NIC (eth1+) | DHCP or static | SSH management — configured by `setup-eth-ssh.sh` |
| PC SSH NIC | DHCP or static | SSH client access |

## 2. Emulator Setup

### Starting the Emulator

```powershell
# From Android SDK (typical path)
& "$env:LOCALAPPDATA\Android\Sdk\emulator\emulator.exe" -avd BlazerEV_AAOS
```

Or launch from Android Studio → Device Manager → `BlazerEV_AAOS`.

### Configuring the Emulator's Network for Bridge Traffic

The emulator must be reachable at `192.168.222.108` on the bridge network — this is the IP the GM head unit always assigns when a USB NIC is plugged in. Configuring the emulator this way ensures the app behaves identically to the real car.

#### Option A: Host-Side Port Forwarding (Simplest)

Use `adb` to forward the emulator's ports through the host PC's bridge NIC:

```powershell
# Forward bridge ports from PC (192.168.222.108) to emulator
# Run these after the emulator is booted

# On the PC NIC connected to the SBC, set a secondary IP:
# (Control Panel → Network Adapter → Properties → IPv4 → Advanced → Add 192.168.222.108)
# Or via PowerShell (run as Administrator):
New-NetIPAddress -InterfaceAlias "Ethernet 2" -IPAddress 192.168.222.108 -PrefixLength 24

# Forward ports from PC to emulator
adb forward tcp:5288 tcp:5288
adb forward tcp:5289 tcp:5289
adb forward tcp:5290 tcp:5290
```

> **Note:** Replace `"Ethernet 2"` with the name of your PC NIC connected to the SBC's onboard NIC. Run `Get-NetAdapter` to list adapters.

With this approach, the bridge connects to `192.168.222.108:5288/5289/5290` and the traffic is forwarded into the emulator.

#### Option B: Emulator Network Bridging (Advanced)

If port forwarding causes issues (e.g., with connection direction — the app connects *to* the bridge, not vice versa), configure the emulator to bridge directly onto the physical network:

```powershell
# Not natively supported by the stock Android emulator.
# Use TAP adapter + emulator's -netdev option if needed.
# For most testing, Option A with reversed connection direction is sufficient.
```

#### Option C: Reverse ADB (App Connects Outbound)

Since the app initiates the TCP connections (app → bridge), you can use `adb reverse` to let the emulator reach the SBC:

```powershell
# Make the emulator see 192.168.222.222:5288 as localhost:5288
adb reverse tcp:5288 tcp:5288
adb reverse tcp:5289 tcp:5289
adb reverse tcp:5290 tcp:5290
```

Then configure the app to connect to `localhost` (or `127.0.0.1`) as the bridge address. The traffic will route through the host to the SBC at `192.168.222.222`.

> **Recommended:** Option C is the most reliable for the app's connection model (app connects to bridge). Set the bridge IP in the app's settings to `127.0.0.1` and use `adb reverse` to tunnel.

## 3. Installing and Running the App

### Build and Install

```powershell
# Build debug APK
.\gradlew :app:assembleDebug

# Install to running emulator
adb install -r app\build\outputs\apk\debug\app-debug.apk
```

### Launch the App

```powershell
adb shell am start -n com.openautolink.app/.ui.MainActivity
```

### View Logs

```powershell
# All app logs
adb logcat --pid=$(adb shell pidof com.openautolink.app)

# Filter for OpenAutoLink tags
adb logcat -s OAL:* Transport:* Video:* Audio:*
```

## 4. Bridge Setup (SBC Side)

### Verify Car Network

SSH into the SBC via the USB NIC connection:

```bash
# Check the onboard NIC has the correct IP
ip addr show eth0
# Should show 192.168.222.222/24

# Verify the bridge service is running
sudo systemctl status openautolink.service

# Check bridge logs
sudo journalctl -u openautolink.service -f
```

### Verify Network Connectivity

From the SBC, confirm the emulator (or its host) is reachable:

```bash
ping -c 3 192.168.222.108
```

From the PC, confirm the bridge is reachable:

```powershell
Test-NetConnection -ComputerName 192.168.222.222 -Port 5288
```

## 5. End-to-End Test Workflow

1. **Boot SBC** — bridge starts automatically via systemd
2. **SSH into SBC** — verify `eth0` is `192.168.222.222`, bridge is running
3. **Start emulator** — launch `BlazerEV_AAOS`
4. **Configure networking** — set up port forwarding or `adb reverse` (see Section 2)
5. **Install app** — `adb install` the debug APK
6. **Launch app** — app should show "Connecting..." and attempt to reach the bridge
7. **Pair phone** — use Bluetooth on the SBC to pair with a phone running Android Auto
8. **Verify projection** — video should render in the emulator, audio should play, touch should respond

### What to Verify

| Component | How to Test | Expected Behavior |
|-----------|-------------|-------------------|
| **TCP connection** | App status UI / logcat | Three channels connect (control, video, audio) |
| **Video** | Emulator display | Phone screen projected, smooth rendering |
| **Audio** | Emulator audio output | Media/nav audio plays through correct purposes |
| **Touch** | Click/drag in emulator | Touch events forwarded to phone, UI responds |
| **Reconnection** | Kill bridge, restart | App shows "Connecting...", reconnects when bridge returns |
| **Settings** | App settings screen | Bridge IP, codec preferences persisted via DataStore |

## 6. Unit and Integration Tests

These run without hardware:

```powershell
# Unit tests (no emulator needed)
.\gradlew :app:testDebugUnitTest

# Instrumentation tests (requires running emulator)
.\gradlew :app:connectedDebugAndroidTest
```

## 7. Testing in the Real Car

When deployed to the actual GM head unit:

- **No ADB access** — GM locks down ADB on production head units
- **No logcat** — rely on the app's built-in diagnostics screen for status
- **No hot-deploy** — install via sideloading (USB drive or network install)
- **Bridge networking is automatic** — the car assigns `192.168.222.108` to its USB NIC; the SBC's onboard NIC is `192.168.222.222`

This is why emulator testing is critical — it's the only environment where you get full `adb` access to debug the app in real-time against a live bridge.

## Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| Emulator can't reach `192.168.222.222` | Port forwarding / reverse not set up | Run `adb reverse` commands (Section 2, Option C) |
| Bridge can't reach `192.168.222.108` | PC NIC missing secondary IP | Add `192.168.222.108` to the PC NIC on the bridge subnet |
| App shows "Connecting..." forever | Bridge not running or wrong IP in app | Check `systemctl status openautolink.service`; verify app bridge IP setting |
| Video renders but no audio | Audio purpose routing mismatch | Check logcat for AudioTrack errors; see [embedded-knowledge.md](embedded-knowledge.md) |
| Emulator is slow / video stutters | x86_64 emulated decoder limitations | Expected — emulator hardware decoding is weaker than the real head unit |
| SSH connection drops | SBC USB NIC lost / wrong cable | Verify USB NIC is `eth1+` not `eth0`; check `setup-eth-ssh.sh` logs |
