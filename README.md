# OpenAutoLink

Wireless Android Auto for aftermarket AAOS head units — no adapter hardware needed.

An SBC (Raspberry Pi CM5, etc.) bridges your phone's Android Auto session to your car's display over WiFi + Ethernet. The car runs the OpenAutoLink app, the SBC runs the bridge.

```
Phone ──WiFi──▶ SBC Bridge ──Ethernet──▶ Car Head Unit App
```

## Components

| Component | Language | Location |
|-----------|----------|----------|
| **Car App** | Kotlin/Compose (AAOS) | `app/` |
| **Bridge** | C++20 + Python | `bridge/` |
| **aasdk** | C++ (forked) | `external/opencardev-aasdk/` |

## Quick Start

### Build the App
```powershell
.\gradlew :app:assembleDebug
```

### Build the Bridge (on SBC)
```bash
cd /opt/openautolink-src/build
cmake --build . --target openautolink-headless -j$(nproc)
```

### Run Tests
```powershell
.\gradlew :app:testDebugUnitTest
```

## Documentation

- [Architecture](docs/architecture.md) — component islands, milestone plan
- [Wire Protocol](docs/protocol.md) — OAL protocol spec (control + video + audio)
- [Embedded Knowledge](docs/embedded-knowledge.md) — hardware lessons from real-car testing
- [Networking](docs/networking.md) — three-network architecture
- [Local Testing](docs/testing.md) — emulator + SBC testing setup
- [Bridge Build Guide](bridge/sbc/BUILD.md) — SBC deployment

## Status

**Starting fresh.** This repo replaces the CPC200-based `carlink_native` app with a purpose-built bridge-only architecture. The bridge C++ code carries forward (it was already TCP-native). The app is being rewritten from scratch.

## License

TBD
