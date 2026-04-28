<p align="center">
  <img src="assets/play_store_512.png" alt="OpenAutoLink" width="128">
</p>

# OpenAutoLink

> Wireless Android Auto for AAOS head units. No extra hardware.
>
> **⚠️ Under active development.** Core car connections and AA functionality are working. See [Known Issues](#known-issues) for current limitations.

[![CI](https://github.com/mossyhub/openautolink/actions/workflows/ci.yml/badge.svg)](https://github.com/mossyhub/openautolink/actions/workflows/ci.yml)
[![Release](https://github.com/mossyhub/openautolink/actions/workflows/release-apk.yml/badge.svg)](https://github.com/mossyhub/openautolink/releases/latest)

OpenAutoLink runs the full Android Auto protocol stack natively on an AAOS head unit using the [aasdk](https://github.com/opencardev/aasdk) C++ library via JNI. No SBC, no USB adapter, no extra hardware — the car and phone talk directly over WiFi.

<p align="center">
  <img src="docs/screenshots/AA-Streaming-Screenshot.jpg" alt="Android Auto streaming on a 2024 Blazer EV via OpenAutoLink" width="720">
  <br>
  <em>Android Auto streaming wirelessly on a 2024 Chevrolet Blazer EV</em>
</p>

<p align="center">
  <img src="docs/screenshots/AA-EV-Battery-Maps.jpg" alt="Google Maps showing EV battery percentage via OpenAutoLink" width="720">
  <br>
  <em>Google Maps displaying the car's EV battery level — real vehicle data forwarded through OpenAutoLink into Android Auto</em>
</p>

> **First-of-its-kind EV integration:** OpenAutoLink forwards real EV battery percentage, range, fuel type, and charge port data from the car into Android Auto. Google Maps uses this to show battery level alongside navigation — something no other aftermarket solution provides.

> **Discuss on XDA:** [OpenAutoLink — Wireless Android Auto for AAOS (GM EVs)](https://xdaforums.com/t/open-source-openautolink-wireless-android-auto-bridge-for-aaos-gm-evs.4785192/)

## Contents

- [Why This Exists](#why-this-exists)
- [How It Works](#how-it-works)
- [Features](#features)
- [What You Need](#what-you-need)
- [Quick Start](#quick-start)
- [Video and Display](#video-and-display)
- [Repository Layout](#repository-layout)
- [Documentation](#documentation)
- [Status](#status)
- [Known Issues](#known-issues)
- [Compatibility](#compatibility)
- [Acknowledgments](#acknowledgments)
- [License](#license)

## Why This Exists

Starting with the 2024 model year, GM dropped Apple CarPlay and Android Auto from its electric vehicles in favor of Google built-in infotainment. OpenAutoLink brings Android Auto back — the car app runs the AA protocol directly, connecting to the phone over WiFi with no intermediate hardware.

## How It Works

OpenAutoLink embeds the [aasdk](https://github.com/opencardev/aasdk) v1.6 C++ library directly into the AAOS app via JNI. The native layer handles the full AA protocol pipeline — SSL handshake, encryption, message framing, and channel multiplexing — while the Kotlin layer manages transport, video rendering, audio playback, and UI.

Two connection methods:

**Wireless (phone hotspot):** A companion app on the phone uses Google Nearby Connections for discovery, then the car joins the phone's WiFi hotspot and runs the AA session over TCP. The phone hotspot is the primary transport — no router or home WiFi needed.

**USB (AOA v2):** Plug the phone directly into the head unit's USB port. The app performs the Android Open Accessory handshake and runs the AA session over bulk USB endpoints.

```
┌─────────────────┐                              ┌──────────────────────────────┐
│   Android Phone  │                              │   Car Head Unit (AAOS)       │
│                  │                              │                              │
│  OAL Companion   │◀── Nearby (discovery) ─────▶│   Kotlin: transport, UI,     │
│  + phone hotspot │◀── WiFi (hotspot) ─────────▶│   video, audio, sensors      │
│  (wireless)      │◀── AA protocol (TCP) ──────▶│          ▼                    │
│                  │                              │   C++ JNI: aasdk v1.6        │
│         or       │                              │   SSL → Cryptor → Messenger  │
│                  │                              │   → AA channels              │
│  USB cable       │◀── AOA v2 (bulk USB) ──────▶│                              │
│  (direct)        │                              │                              │
└─────────────────┘                              └──────────────────────────────┘
```

## Features

- **Zero hardware (wireless)** — phone hotspot → car joins WiFi → AA over TCP, no cables or router needed
- **USB cable support** — AOA v2 direct connection for wired setups
- **aasdk v1.6 native protocol** — battle-tested C++ AA library via JNI, not a reimplementation
- **EV battery data in Android Auto** — battery %, range, fuel type, charge port forwarded from VHAL into AA. Google Maps shows battery level alongside navigation
- **H.264, H.265, and VP9** video with auto-negotiation. Up to 4K with AA Developer Mode
- **PCM and AAC-LC audio** — PCM for compatibility, AAC-LC for ~10× WiFi bandwidth reduction
- **Multi-phone support** — set a default phone, switch between phones with an overlay chooser
- **Pixel-perfect display adaptation** — `width_margin` / `height_margin` auto-computed for wide and ultra-wide AAOS screens
- **Per-purpose audio volume** — separate sliders for media, navigation, and assistant
- **Custom key remapping** — map any physical button to any AA action
- **Microphone enhancement** — NoiseSuppressor, AGC, AcousticEchoCanceler
- **Instrument cluster** — turn-by-turn navigation and media metadata on supported vehicles
- **Full sensor suite** — GPS, accelerometer, gyroscope, compass, EV energy model (21 sensor types)
- **Steering wheel controls** — media, voice, and DPAD forwarded to AA
- **Configurable display** — fullscreen/windowed, safe area insets, DPI, margins, scaling mode
- **Stats overlay** — codec, resolution, FPS, bitrate, WiFi band, decoder info
- **Automatic reconnect** — car sleep → wake → projection resumes with no user interaction
- **Built-in diagnostics** — USB device scanner, network probe, remote log server (TCP 6555), VHAL browser
- **CI/CD** — GitHub Actions builds native deps, runs tests, and produces signed APKs on release

## What You Need

| Item | Notes |
|------|-------|
| **AAOS vehicle** | Tested on 2024 Chevrolet Blazer EV. Other GM EVs likely work |
| **Android phone** | Running the OpenAutoLink Companion app |
| **Google Play Console account** | To publish the AAOS app to your car |

That's it. No SBC, no Ethernet adapter, no extra hardware.

### Phone Setup

Install the **OpenAutoLink Companion** app on your phone. It handles:
- Nearby Connections advertising (phone discovery)
- Android Auto Service auto-start detection
- Custom device name for multi-phone identification

You can either download a prebuilt APK from [GitHub Actions](https://github.com/mossyhub/openautolink/actions/workflows/build-companion.yml) (click the latest run → Artifacts → `companion-debug-apk`) or build it yourself from the `companion/` directory.

## Quick Start

### 1. Install the Companion App (Phone)

**Option A — Download prebuilt APK:**
1. Go to [Build Companion APK](https://github.com/mossyhub/openautolink/actions/workflows/build-companion.yml) on GitHub Actions.
2. Click the latest successful run.
3. Download the `companion-debug-apk` artifact.
4. Unzip and install the APK on your phone (enable "Install from unknown sources" if prompted).

**Option B — Build from source:**
```powershell
cd companion
..\gradlew assembleDebug
adb install -r build/outputs/apk/debug/*.apk
```

The companion APK is signed with the Android debug key, which is fine for sideloading.

### 2. Build and Publish the Car App (AAOS)

Because this is an AAOS app, installation on the car goes through your own Google Play Console account:

1. Create a [Google Play Console](https://play.google.com/console/) developer account.
2. Create a new app and configure an AAOS release track.
3. Change the package name in `app/build.gradle.kts` from `com.openautolink.app` to your own unique ID.
4. Generate an upload keystore:
   ```powershell
   .\scripts\create-upload-keystore.ps1
   ```
5. Build and sign the release AAB:
   ```powershell
   .\scripts\bundle-release.ps1
   ```
6. Upload the AAB to Play Console, publish, and install on the car via Play Store.
7. Grant the **Car Information** permission: Settings → Apps → OpenAutoLink → Permissions.

### 3. Connect

**Wireless:**
1. Open the Companion app on the phone and tap **Start**.
2. Open OpenAutoLink on the car — it discovers the phone automatically.
3. Android Auto projection starts within seconds.

**USB:**
1. Plug the phone into the head unit's USB port.
2. OpenAutoLink detects the device and performs the AOA v2 handshake.
3. Android Auto projection starts over the USB connection.

## Video and Display

### Native Dependencies

The C++ JNI layer links against prebuilt static libraries (OpenSSL, Boost, aasdk + protobuf + abseil). These are built once and cached:

- **CI builds** download them automatically from a [GitHub Release](https://github.com/mossyhub/openautolink/releases/tag/native-deps).
- **Local builds** (WSL): run the build scripts once, then Gradle finds them:
  ```bash
  scripts/build-openssl-android.sh    # OpenSSL for ARM64
  scripts/setup-ndk-deps.sh           # Boost headers
  scripts/build-aasdk-android.sh      # aasdk + protobuf + abseil
  ```

### Resolution Tiers

| Resolution | Codec | Notes |
|-----------|-------|-------|
| 480p (800×480) | H.264 | Always available |
| 720p (1280×720) | H.264 | Always available |
| 1080p (1920×1080) | H.264, H.265, VP9 | Default tier |
| 1440p (2560×1440) | H.265, VP9 | Requires AA Developer Mode |
| 4K (3840×2160) | H.265, VP9 | Requires AA Developer Mode |

By default, the app uses auto-negotiation — the phone picks the best codec and resolution it supports.

### Display Adaptation

OpenAutoLink auto-computes `width_margin` (or `height_margin` for portrait displays) to tell the phone to render a wider viewport matching the display's aspect ratio. This avoids stretched circles and distorted UI that `pixel_aspect_ratio_e4` cannot fix (Qualcomm decoders ignore `MediaCodec` scaling modes). A safe area editor lets you push UI inward from screen edges to avoid curved bezels.

> **Blazer EV tip:** 1440p at 230 DPI works well. Pull the top safe area inset down ~50px.

## Repository Layout

| Component | Language | Location | Purpose |
|-----------|----------|----------|---------|
| **Car App** | Kotlin / Compose | `app/` | AAOS app — UI, video, audio, sensors |
| **C++ JNI Layer** | C++20 | `app/src/main/cpp/` | aasdk v1.6 AA protocol via JNI |
| **Companion App** | Kotlin / Compose | `companion/` | Phone-side Nearby Connections advertiser |
| **aasdk (submodule)** | C++ | `external/opencardev-aasdk/` | AA protocol library ([fork](https://github.com/mossyhub/aasdk)) |
| **Native build scripts** | Bash | `scripts/` | Cross-compile OpenSSL, Boost, aasdk for Android |
| **Documentation** | Markdown | `docs/` | Architecture, protocol, embedded knowledge |
| **Build/Deploy scripts** | PowerShell | `scripts/` | Release bundling, deployment, testing |

### C++ JNI Layer

The app embeds aasdk as a prebuilt static library (`.a`) and links it via CMake/NDK. The JNI bridge files handle the boundary between Kotlin and C++:

| File | Purpose |
|------|---------|
| `aasdk_jni.cpp` | JNI entry point — native method registration |
| `jni_session.{h,cpp}` | aasdk pipeline: SSLWrapper → Cryptor → Messenger → channels |
| `jni_channel_handlers.{h,cpp}` | Per-channel handler classes (audio, sensor, input, nav, mic, media, phone, BT) |
| `jni_transport.{h,cpp}` | `ITransport` backed by Kotlin byte streams (Nearby or USB) |

### Historical

The original architecture used an SBC (single-board computer) running a C++ bridge binary and Python Bluetooth/WiFi scripts to relay Android Auto from the phone to the car over Ethernet. That was replaced by direct mode. The app initially reimplemented the AA protocol in Kotlin, which was then replaced by the current aasdk C++ JNI approach for protocol correctness and performance. The bridge code is preserved on the [`bridge-mode`](https://github.com/mossyhub/openautolink/tree/bridge-mode) branch.

## Documentation

| Doc | Purpose |
|-----|---------|
| [Architecture](docs/architecture.md) | Component islands and system structure |
| [Embedded Knowledge](docs/embedded-knowledge.md) | Lessons from real-car testing — **read before touching video/audio/VHAL** |
| [USB Transport Plan](docs/usb-transport-plan.md) | AOA v2 design and implementation |
| [Local Testing](docs/testing.md) | Emulator testing, remote diagnostics, no-ADB debugging |
| [Wire Protocol](docs/protocol.md) | OAL protocol details (bridge-mode reference) |
| [Multi-Phone Plan](docs/multi-phone-nearby.md) | Multi-phone Nearby Connections design |
| [HUR Feature Comparison](docs/headunit-revived-feature-comparison.md) | Feature parity tracking vs Headunit Revived |

## Status

Active development, stable for daily driving on a 2024 Chevrolet Blazer EV.

**Working:**
- aasdk v1.6 AA protocol via C++ JNI (SSL, Cryptor, Messenger, all channels)
- Wireless connection via Nearby Connections (companion app)
- USB connection via AOA v2
- Video (H.264/H.265/VP9, up to 4K)
- Audio (PCM and AAC-LC, 5-purpose routing)
- Touch input with multi-touch
- Microphone (with NS + AGC + AEC)
- Vehicle data forwarding (VHAL → AA, 21 sensor types)
- EV energy model (battery %, range, fuel type, charge port)
- Cluster navigation (turn-by-turn) and media metadata
- Steering wheel controls
- Multi-phone (default + chooser)
- Automatic reconnect on car wake
- Built-in diagnostics and remote log server
- CI/CD: native deps build, unit tests, signed release APKs

## Known Issues

- **H.265 video may appear green-tinted** on first connection for 30–45 seconds. May be Qualcomm-specific — not yet confirmed on other SoCs
- **VHAL data and sensors** not flowing into AA properly — vehicle speed, gear, parking brake etc. not reaching the phone
- **Cluster service** not working — navigation turn-by-turn data not appearing on the instrument cluster

If you encounter other problems, please [open an issue](https://github.com/mossyhub/openautolink/issues).

## Compatibility

Validated on a **2024 Chevrolet Blazer EV** running AAOS 12L. Other GM EVs on similar AAOS platforms likely work but have not been broadly tested. Non-GM AAOS vehicles may have different restrictions.

The companion app runs on any Android phone with Google Play Services (Nearby Connections requires it).

## Acknowledgments

### Core Dependency

- **[opencardev/aasdk](https://github.com/opencardev/aasdk)** — The Android Auto protocol library at the heart of OpenAutoLink. Our [fork](https://github.com/mossyhub/aasdk) (branch `openautolink`) adds NavigationStatus extensions and EV energy model sensor types. The C++ library runs directly on the head unit via JNI.

### Where It Started

- **[metheos/carlink_native](https://github.com/metheos/carlink_native)** / **[lvalen91/carlink_native](https://github.com/lvalen91/carlink_native)** and the **[XDA CarLink thread](https://xdaforums.com/t/carlink.4774308)** inspired the original proof of concept.

### Projects I Learned From

- **[opencardev/openauto](https://github.com/opencardev/openauto)** — head unit emulator architecture.
- **[nickel110/WirelessAndroidAutoDongle](https://github.com/nickel110/WirelessAndroidAutoDongle)** — BT pairing and WiFi credential exchange reference.
- **[andrerinas/headunit-revived](https://github.com/nickel110/headunit-revived)** — AA receiver app reference for protocol implementation and feature ideas.

### On AI Assistance

This project is heavily AI-assisted, but grounded in extensive real hardware testing. The code moves faster with Copilot; the driveway testing, log analysis, and protocol debugging are what determine whether the result is actually good.

## License

TBD
