# Bridge

The bridge code carries forward from `carlink_native` (already TCP-native).

## What to Copy

From `d:\personal\carlink_native\bridge\`:
- `openautolink/headless/` — C++ headless binary (all headers, source, CMakeLists.txt, patches)
- `openautolink/scripts/` — `aa_bt_all.py` (BT/WiFi service)
- `openautolink/README.md` — Feature overview
- `sbc/` — All systemd services, scripts, env config, BUILD.md, install.sh

From `d:\personal\carlink_native\external\`:
- `opencardev-aasdk/` → copy to `external/opencardev-aasdk/`

## Bridge Changes Needed

The bridge currently outputs CPC200-framed packets on TCP 5288. It needs to be updated to output OAL protocol:

1. **Control channel** (port 5288): JSON lines instead of CPC200 binary packets
2. **Video channel** (port 5290): 12-byte OAL header instead of CPC200 VIDEO_DATA (16+20 byte headers)
3. **Audio channel** (port 5289): 8-byte OAL header instead of CPC200 AUDIO_DATA headers

The aasdk session side (phone ↔ bridge) is UNCHANGED. Only the car-facing transport changes.

### Files to Modify
- `headless/include/openautolink/tcp_car_transport.hpp` — OAL framing
- `headless/include/openautolink/cpc_session.hpp` → rename to `oal_session.hpp`
- `headless/src/cpc_session.cpp` → rename to `oal_session.cpp`
- `headless/include/openautolink/cpc200.hpp` → replace with `oal_protocol.hpp`

### Files Unchanged
- `headless/src/live_session.cpp` — aasdk integration (phone side)
- `headless/include/openautolink/live_session.hpp`
- `headless/src/main.cpp` — CLI args (mostly)
- All handler files (video, audio, input, sensor, bluetooth)
- `scripts/aa_bt_all.py` — BT/WiFi (phone side)
- `sbc/` — All systemd/deploy infra
