---
description: "Bootstrap repo memory with project history and embedded knowledge from carlink_native. Run this once when first opening the workspace."
---
# Bootstrap Repo Memory

Read `docs/embedded-knowledge.md` and `docs/architecture.md`, then create repo memory entries with these facts:

## Project History
- OpenAutoLink started as `carlink_native`, an app for the CPC200-CCPA USB adapter (VID 0x1314, PID 0x1520)
- Evolution: USB adapter app → TCP bridge adapter → purpose-built bridge-only app (this repo)
- The bridge C++ code (`bridge/`) carries forward from carlink_native — already proven on real hardware
- The app (`app/`) is a complete rewrite — no CPC200 protocol, no USB adapter support
- Key decision: separate TCP connections for control/video/audio eliminates head-of-line blocking (validated in carlink_native v1.14.0)

## Hardware Validated On
- **Car**: 2024 Chevrolet Blazer EV, Qualcomm Snapdragon SoC, 2914×1134 display, gminfo platform
- **SBC**: Raspberry Pi CM5 (primary), Khadas VIM4 (secondary)
- **Phone**: Google Pixel (Android Auto), iPhone (CarPlay via adapter)

## Key Technical Decisions
- 3 TCP connections (control JSON:5288, video binary:5290, audio binary:5289) — not CPC200 multiplexed
- aasdk v1.6 ServiceConfiguration format required (typed services, not old ChannelDescriptor)
- 5 pre-allocated AudioTrack slots by purpose (media/nav/assistant/call/alert) — proven routing
- Video = disposable real-time projection (drop late frames). Audio = continuous signal (buffer, never stall)
- Component island architecture with test-first development
- MVVM + StateFlow, Repository pattern, coroutines

## Populate repo memory with these entries so future Copilot sessions have context.
