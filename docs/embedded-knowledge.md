# Embedded Knowledge — Lessons from carlink_native

Hard-won knowledge from the original CPC200 adapter app. These findings were validated on real hardware (2024 Chevrolet Blazer EV, Raspberry Pi CM5, Khadas VIM4). **Read this before building any video, audio, or vehicle integration code.**

---

## Car Hardware (GM Blazer EV, gminfo platform)

### Display
- Physical: 2914×1134 pixels
- Usable: ~2628×800 (with nav bar hidden)
- DPI: 160
- SoC: **Qualcomm Snapdragon** (NOT Intel as some GM docs suggest, but some generation are intel, so we should try to support those as the are the same screen and AAOS, just a different processor and therefore decoders)

### HW Video Decoders
- `c2.qti.avc.decoder` — H.264, max 8192×4320 @ 480fps
- `c2.qti.hevc.decoder` — H.265, same limits
- `c2.qti.vp9.decoder` — VP9
- No AV1 hardware decoder
- Both OMX and C2 (Codec2) variants exist. Prefer C2

### AAOS Restrictions
- WiFi Direct NOT available (GM stripped P2P from framework)
- BLE advertising may timeout (needs runtime permission workaround)
- Third-party cluster services appear briefly then get killed (GM restriction)
- USB port enumerates USB Ethernet adapters → car is DHCP server on that subnet
- Car assigns IPs in 192.168.222.x range on USB Ethernet

---

## Video Pipeline Lessons

### MediaCodec Lifecycle
1. **Surface changes require full codec reset**: stop() → release() → create new → configure() → start()
2. **Pause/resume**: must release codec on pause, recreate on resume. Keeping codec alive across lifecycle causes ANR
3. **First frame must be codec config**: SPS/PPS for H.264, VPS/SPS/PPS for H.265. Without this, decoder produces garbage
4. **Drop frames until first IDR**: P-frames without a reference frame corrupt the display
5. **Force even dimensions**: `width and 0xFFFFFFFE.toInt()` — odd dimensions cause MediaCodec crash on some decoders
6. **SurfaceView over TextureView**: SurfaceView uses HWC overlay (zero-copy, lower latency). TextureView composites through GPU

### Video Recovery
- On corruption: release codec, request IDR from bridge (`keyframe_request` control message)
- After HOME press: codec restarts with no fresh IDR → black screen. Bridge must send IDR on reconnect without rate-limiting
- Debounce surface size changes: 150ms stabilization before codec reconfiguration (orientation changes fire multiple callbacks)

### Performance
- 28-52fps observed on real car (target 60fps). Codec resets and bridge frame pacing contribute to gaps
- 3 codec resets per typical session — each costs ~200ms of black screen
- Stats tracking: atomic counters for frames decoded/dropped, current FPS (rolling 1s window)

---

## Audio Pipeline Lessons

### 5-Purpose Routing (PROVEN — DO NOT CHANGE THE LOGIC)
The bridge sends audio with `purpose` and `sample_rate`. Routing:

```
purpose=MEDIA + 48kHz stereo    → Media AudioTrack (USAGE_MEDIA)
purpose=NAVIGATION + 16kHz mono → Nav AudioTrack (USAGE_ASSISTANCE_NAVIGATION_GUIDANCE)
purpose=ASSISTANT + 16kHz mono  → Siri AudioTrack (USAGE_ASSISTANT)
purpose=PHONE_CALL + 8kHz mono  → Call AudioTrack (USAGE_VOICE_COMMUNICATION)
purpose=ALERT + 24kHz mono      → Alert AudioTrack (USAGE_NOTIFICATION_RINGTONE)
```

### AudioTrack Behavior
- Pre-allocate all 5 AudioTracks at session start (creating during playback causes audible gaps)
- `AudioTrack.write()` BLOCKS — always call from a dedicated thread, never from the TCP read thread
- On underrun: fill with silence, don't stall. Track underrun count for diagnostics
- Navigation audio automatically ducks media (AAOS handles via AudioAttributes) — no manual volume needed

### Ring Buffer
- 500ms capacity absorbs TCP jitter
- Lock-free SPSC (single producer/single consumer) is sufficient — one TCP thread produces, one AudioTrack thread consumes
- On overflow: drop oldest (not newest). Freshest audio is always more relevant

### Microphone Capture
- Timer at 40ms intervals (~25 Hz), not continuous recording (reduces CPU)
- Sample rate: 8000 Hz for voice assistant, 16000 Hz for calls
- Bridge tells app when to start/stop via control messages
- RECORD_AUDIO permission: handle denial gracefully (no mic, but app still works)

---

## Touch Lessons

### Coordinate Scaling
- Android MotionEvent coordinates are in screen space
- Bridge expects video-space coordinates (the AA resolution, e.g. 1920×1080)
- Scale: `videoX = (screenX / surfaceWidth) * videoWidth`
- Multi-touch: POINTER_DOWN (action 5) and POINTER_UP (action 6) with pointer index encoded in upper bits

### Multi-Touch
- Action codes: DOWN=0, UP=1, MOVE=2, POINTER_DOWN=5, POINTER_UP=6
- Pinch zoom on Maps requires proper POINTER_DOWN/UP with multiple pointer data
- Each pointer: id, x, y in the touch event

---

## Vehicle Data (VHAL) Lessons

### Property Monitoring
- 37 VHAL properties subscribed via `CarPropertyManager`
- Always check `isPropertyAvailable()` before subscribing — not all properties exist on all cars
- Use reflection to access `android.car.Car` — the API isn't in the standard SDK
- Properties include: speed, gear, turn signals, battery level, fuel level, tire pressure, ambient temp, steering angle
- Send as JSON on control channel → bridge → aasdk SensorBatch → phone

### GNSS
- Android LocationManager → NMEA sentences ($GPRMC, $GPGGA)
- Fine location permission required
- Send raw NMEA strings to bridge → aasdk sensor channel → phone's Google Maps

---

## Bridge (SBC) Lessons

### Wireless AA Flow (End-to-End, PROVEN)
```
1. BLE advertisement with AA UUID → phone discovers bridge
2. Bluetooth pairing (SSP JustWorks, auto-accept)
3. HSP Headset profile connect → keeps BT link alive
4. RFCOMM channel 8 → protobuf WiFi credential exchange
5. Phone joins bridge WiFi AP (5GHz ch149 preferred)
6. Phone TCP connects to bridge port 5277
7. aasdk v1.6 handshake → version → TLS → auth → ServiceDiscovery
8. Phone opens channels → video/audio/sensor/input/bluetooth streaming
```

### aasdk v1.6 Critical Findings
- Protocol version 1.6 required (phones respond 1.7 MATCH)
- **ServiceConfiguration must use typed services** (MediaSinkService, SensorSourceService, InputSourceService) — old ChannelDescriptor format (v1.1) causes phone to silently ignore ServiceDiscoveryResponse
- Must send ChannelOpenResponse for EVERY ChannelOpenRequest — omitting = phone disconnects after ~60s
- Must send MediaAckIndication for every video frame — without acks, phone's flow control backs up
- Phone sends PingRequests; bridge must respond. Bridge pings to phone may be ignored (don't quit on timeout)
- Session confirmed stable: 48,061 frames / 13+ minutes / zero protocol errors

### BlueZ Pitfalls
- SAP plugin steals RFCOMM channel 8 → `--noplugin=sap` in bluetoothd
- `--compat` flag required for sdptool/RFCOMM to work
- Can't use both D-Bus RegisterProfile and manual socket.bind on same channel
- HSP headset profile (0x1108) must connect to phone's AG (0x1112) to keep link alive

### WiFi AP
- 5GHz channel 149 with 80MHz VHT preferred (higher throughput)
- brcmfmac (CM5) does NOT support SU-BEAMFORMER or 802.11ax, only SU-BEAMFORMEE + SHORT-GI-80
- WPA2 CCMP, fixed SSID/password (not random per session)
- IP: 192.168.43.1, DHCP via dnsmasq

### Build/Deploy
- Source at `/opt/openautolink-src/`, runtime at `/opt/openautolink/`
- NEVER build in user home directories
- Touch files after scp to force CMake rebuild
- Stop service before replacing binary (`Text file busy` error)
- CM5: use `-j1` for builds (parallel compile crashes under load)
- CRLF: all SBC scripts MUST be LF. Windows scp creates CRLF → `start-wireless.sh` fails with `"1\r" != "1"`

### USB Gadget (CDC-ECM for Car Network)
- GM EVs need `mass_storage` function in composite gadget (UDisk mode) — pure USB rejected
- FFS IN endpoint writes only complete from the SAME THREAD that reads ep_out (Amlogic + DWC2)
- Link FFS before mass_storage in configfs so FFS gets interface 0 (app claims first bulk interface)
- These FFS lessons are documented for reference but the new app uses TCP, not USB direct

---

## What NOT to Carry Forward

These patterns from carlink_native are **anti-patterns** for the new app:

1. **CPC200 protocol** — the new app uses OAL protocol (JSON control + binary media). No magic headers, no inverted checksums, no heartbeat-gated writes
2. **DeviceWrapper abstraction** — no USB adapter support. TCP only
3. **God-object CarlinkManager** — replaced by Session orchestrator + independent islands
4. **Fake USB identity in TcpDeviceWrapper** — no pretending TCP is USB
5. **AdapterDriver init sequence** — no 50-packet handshake. Simple JSON `hello` exchange
6. **Heartbeat-gated writes** — TCP has proper flow control. Send when ready
7. **Conditional UI for USB vs bridge mode** — there is only bridge mode
