# OpenAutoLink -- Project Guidelines

## What This Is

A wireless Android Auto bridge for AAOS head units. Purpose-built from scratch.

```
Android Phone --WiFi TCP:5277--> SBC (openautolink-relay)
                                   raw byte splice (zero AA processing)
                                   |
                              SBC TCP:5288 (control/signaling) --Ethernet--> Car App (AAOS)
                              SBC TCP:5291 (relay/AA data)     --Ethernet--> Car App (AAOS)
                                                                               |
                                                                          aasdk via NDK/JNI
                                                                          TLS + protobuf + AA
                                                                          MediaCodec + AudioTrack
```

Two components:
1. **App** (`app/`) -- Kotlin/Compose AAOS app with aasdk compiled in via NDK/JNI. Owns all AA protocol handling, video decode, audio playback, touch/sensor forwarding
2. **Bridge** (`bridge/`) -- Tiny C++ relay binary (~340 lines, 67KB stripped) + Python BT/WiFi services on an SBC. Splices TCP sockets, manages BT pairing, hosts WiFi AP

## Performance Priorities

**Video, audio, and touch performance are the #1 priority.** Every design decision must optimize for:

1. **Fast initial render**: Connection -> first video frame -> audio playing must be as fast as possible
2. **Stable streaming**: Zero dropped audio, minimal dropped video frames, immediate touch response
3. **Seamless reconnection**: Car sleeps -> wakes -> app reconnects outbound to bridge relay -> phone reconnects -> streaming resumes. No user interaction needed

> **Design test**: If a feature adds latency to connection, first-frame, or reconnection -- it needs exceptional justification.

## Cross-Component Rule

aasdk runs **in-process** in the app via NDK/JNI. When modifying transport, video, audio, or input code:
1. Read `app/src/main/cpp/aa_session.cpp` to understand how aasdk callbacks deliver data to Kotlin
2. Read `app/src/main/cpp/jni_bridge.cpp` for JNI function signatures and callback patterns
3. The bridge relay (`bridge/openautolink/relay/`) does NOT process AA protocol -- just raw bytes

## Architecture

### App (`app/`) -- Component Islands

| Island | Responsibility |
|--------|---------------|
| `transport/` | `DirectAaTransport` (outbound relay connection + aasdk JNI), `AasdkJni` (JNI wrapper), `ConnectionState`, `ControlMessage` |
| `video/` | MediaCodec lifecycle, Surface rendering, codec detection |
| `audio/` | Multi-purpose AudioTrack (5 slots), mic capture, ring buffer |
| `input/` | Touch forwarding, GNSS, vehicle data (VHAL), IMU sensors |
| `ui/` | Compose screens -- projection surface, settings, diagnostics |
| `navigation/` | Nav state from aasdk JNI, maneuver icons, cluster service |
| `session/` | Session orchestrator -- connects islands, manages lifecycle |

- **Min SDK 32**, target SDK 36, Kotlin, Jetpack Compose, DataStore preferences
- **MVVM** with `StateFlow` -- ViewModels own UI state, repositories own data
- **NDK/JNI** for aasdk: `app/src/main/cpp/` contains C++ source, linked as `liboal_jni.so`

### NDK/JNI (`app/src/main/cpp/`)

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | NDK build -- stub mode (no deps) or full mode (aasdk + OpenSSL + Boost + protobuf) |
| `jni_bridge.cpp` | JNI entry points: `startSession`, `startSessionWithFd`, `stopSession`, `sendTouch`, `sendSensorData`, `sendMicAudio`, `sendButton`. Native->Kotlin callbacks: `onVideoFrame`, `onAudioFrame`, `onPhoneConnected`, `onPhoneDisconnected` |
| `aa_session.cpp` | Full aasdk entity -- TCP listener or fd-based session, SSL, service discovery, video/audio/nav/input/BT/phone status handlers |
| `third_party/` | Boost headers (header-only), OpenSSL prebuilts (per-ABI `.a` files) |

### External Dependencies (`external/`)

| Submodule | Purpose |
|-----------|---------|
| `external/opencardev-aasdk/` | aasdk v1.6 -- AA protocol library, compiled into `liboal_jni.so` via NDK |

### Bridge (`bridge/`)

The bridge relay is a ~340-line C++ binary with zero external dependencies.

| Directory | Purpose |
|-----------|---------|
| `bridge/openautolink/relay/` | `openautolink-relay` binary -- control server (5288), relay server (5291), phone listener (5277), raw socket splice |
| `bridge/openautolink/scripts/` | `aa_bt_all.py` -- BLE, BT pairing (AA profiles), HSP, RFCOMM WiFi credential exchange |
| `bridge/sbc/` | Systemd services, env config, install script, build guide |

## Build & Test

### App
```powershell
$env:JAVA_HOME = "C:\Program Files\Eclipse Adoptium\jdk-21.0.10.7-hotspot"
.\gradlew :app:assembleDebug             # Debug APK (includes liboal_jni.so)
.\gradlew :app:bundleRelease             # AAB for Play Store
.\gradlew :app:testDebugUnitTest          # Unit tests
```

### Bridge (WSL cross-compile + deploy)
```powershell
scripts\deploy-bridge.ps1          # Build in WSL + deploy to SBC
scripts\deploy-bridge.ps1 -Clean    # Clean rebuild + deploy
```
See [bridge/sbc/BUILD.md](bridge/sbc/BUILD.md) for SBC setup.

## Conventions

### Relay Control Protocol
- **2 TCP connections**: control (5288, JSON lines) + relay (5291, raw bytes)
- **Control**: newline-delimited JSON signaling -- `hello`, `relay_ready`, `relay_disconnected`, `phone_bt_connected`, `paired_phones`, `app_log`, `app_telemetry`
- **Relay**: raw byte splice between app and phone -- aasdk handles TLS/protobuf/AA internally

### Video Rules
> Projection streams are live UI state, not video playback.
> Late frames must be dropped. Corruption must trigger reset.
> Video may drop. Audio may buffer. Neither may block the other.
> On reconnect: flush decoder, discard stale buffers, wait for IDR before rendering.

### Code Patterns
- **Island independence**: Each component island has its own package and public interface
- **ViewModel per screen**: Compose screens observe `StateFlow` from ViewModels
- **Coroutines for async**: `viewModelScope` for UI, `Dispatchers.IO` for network/disk, dedicated threads for real-time audio/video

### Naming
- Project: **OpenAutoLink**
- App package: `com.openautolink.app`
- Bridge binary: `openautolink-relay`
- Native library: `liboal_jni.so`
- Systemd services: `openautolink-*.service`
- Env file: `/etc/openautolink.env`

## Key Documentation

| Doc | Purpose |
|-----|---------|
| [docs/architecture.md](docs/architecture.md) | Component island architecture |
| [docs/direct-aa-plan.md](docs/direct-aa-plan.md) | Direct AA migration plan and status |
| [docs/embedded-knowledge.md](docs/embedded-knowledge.md) | Hardware lessons (MUST READ before touching video/audio/VHAL) |
| [docs/networking.md](docs/networking.md) | Three-network architecture (phone, car, SSH) |
| [bridge/sbc/BUILD.md](bridge/sbc/BUILD.md) | SBC build and deployment guide |
| [docs/testing.md](docs/testing.md) | Local testing with AAOS emulator + SBC |

## Pitfalls

- **CRLF**: SBC scripts and env files must be LF. Windows scp creates CRLF -- always convert
- **aasdk v1.6**: Phone requires v1.6 ServiceConfiguration format
- **BlueZ SAP plugin**: Steals RFCOMM channel 8. Disable with `--noplugin=sap`
- **MediaCodec lifecycle**: Must release codec on pause, recreate on resume. Surface changes require full codec reset
- **AudioTrack purpose routing**: See [docs/embedded-knowledge.md](docs/embedded-knowledge.md)
- **JNI thread safety**: aasdk runs on a dedicated `io_service` thread. JNI callbacks use `AttachCurrentThread`. ByteArray allocation on every frame is unavoidable (JNI requires it)
- **Socket fd reflection**: `DirectAaTransport.getSocketFd()` uses reflection to extract fd from `Socket.impl.fd` -- may break on future Android versions
- **NDK OpenSSL**: Prebuilt `.a` files in `third_party/openssl/<ABI>/` -- must match NDK version. Rebuild if NDK is updated
