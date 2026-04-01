# OpenAutoLink — Work Plan

---

## 🔄 Carry-Forward Issues (Bridge-Side)

These exist in the current bridge code and will need fixing regardless of the app rewrite.

### 1. Video Startup Delay (High Priority)
- 65s gap between phone connect and car app connect creates stale frame backlog
- 2666 frames dropped at startup (MAX_PENDING=120 cap)
- **Fix:** Don't queue video frames until car app is connected (`client_fd_ < 0` → skip). Clear pending on new connection

### 2. Video FPS Below Target
- Stats show 28-52fps (target 60fps)
- May be bridge sending at 30fps despite `OAL_AA_FPS=60`
- **Investigate:** Verify aasdk SDR actually requests 60fps from phone

### 3. Phone AA Session Drops (Error 33)
- Phone occasionally drops TCP with EOF
- Bridge cert files not deployed → search fails on restart
- **Fix:** Deploy headunit.crt/key to `/opt/openautolink/cert/`

### 4. Black Screen After Reconnect
- Bridge rate-limits keyframe replay to 5s
- After app reconnect, no fresh IDR available
- **Fix:** Bridge should bypass rate limit on first keyframe request after new app connection

### 5. Bluetooth HFP Not Working
- BT pairing works, BLE works, RFCOMM ch8 works, WiFi credential exchange works
- HFP (Hands-Free Profile) NOT connected → no BT audio routing for calls
- **Architecture:** Phone pairs via BT to the SBC/bridge, NOT to the car. Call/voice audio must flow: Phone → BT HFP → SBC → bridge captures SCO audio → forwards over TCP to app
- **Needed for:** phone calls, voice assistant, proper AA auto-connect

---

## 🔧 Bridge Protocol Migration (OAL)

The bridge needs to output OAL protocol to the app.

### Control Channel (Port 5288) → JSON Lines
- [ ] JSON line messages for all control communication
- [ ] Hello handshake with capabilities exchange
- [ ] Phone connected/disconnected events
- [ ] Audio start/stop per purpose
- [ ] Nav state forwarding
- [ ] Media metadata forwarding
- [ ] Config echo on settings change
- [ ] Mic start/stop signals

### Video Channel (Port 5290) → 16-byte Header
- [ ] OAL 16-byte header: payload_length, width, height, pts_ms, flags
- [ ] Flags: keyframe bit, codec config bit, EOS bit
- [ ] First frame must be codec config (SPS/PPS)

### Audio Channel (Port 5289) → 8-byte Header
- [ ] OAL 8-byte header: direction, purpose, sample_rate, channels, length
- [ ] Direction field (0=playback, 1=mic capture)
- [ ] Purpose field for routing (media/nav/assistant/call/alert)
- [ ] Bidirectional: bridge→app playback, app→bridge mic

### Touch/Input Channel (via Control 5288)
- [ ] JSON touch events with action, coordinates, pointer array
- [ ] GNSS NMEA forwarding
- [ ] Vehicle data JSON

---

## 📱 App Milestones (New Build)

See [docs/architecture.md](architecture.md) for full component island breakdown and public APIs.

### M1: Connection Foundation
- [x] Gradle project scaffold (min SDK 32, Compose, DataStore)
- [x] Transport island: TCP connect, JSON control parsing, reconnect
- [x] Session state machine (IDLE → CONNECTING → BRIDGE_CONNECTED → PHONE_CONNECTED → STREAMING)
- [x] ProjectionScreen with SurfaceView + connection status HUD

### M2: Video
- [ ] MediaCodec decoder with codec selection (H.264/H.265/VP9)
- [ ] OAL video frame parsing (16-byte header)
- [ ] NAL parsing for SPS/PPS extraction
- [ ] Stats overlay (FPS, codec, drops)

### M3: Audio
- [ ] 5-purpose AudioTrack slots with ring buffers
- [ ] OAL audio frame parsing (8-byte header)
- [ ] Audio focus management (request/release/duck)
- [ ] Purpose routing (media/nav/assistant/call/alert)
- [ ] Dual audio path support — all audio flows through the bridge via TCP:
  - **AA session audio** (aasdk channels): media, navigation, alerts — decoded by aasdk, sent as PCM over OAL
  - **BT HFP audio** (phone → SBC Bluetooth): phone calls, voice assistant — bridge captures SCO audio from HFP and forwards as PCM over OAL with call/assistant purpose
- [ ] Detect active audio purpose and manage focus (e.g., duck media during call)
- [ ] Handle call audio transitions: ring, in-call, call end

### M4: Touch + Input
- [ ] Touch forwarding with coordinate scaling
- [ ] Multi-touch (POINTER_DOWN/UP for pinch zoom)
- [ ] JSON touch serialization to control channel

### M5: Microphone + Voice
- [ ] Timer-based mic capture from car's mic (via AAOS AudioRecord)
- [ ] Send on audio channel (direction=1)
- [ ] Bridge mic_start/mic_stop control messages
- [ ] Coordinate mic routing: bridge forwards mic PCM to aasdk for AA voice, and to BT SCO for phone calls

### M6: Settings + Config
- [ ] DataStore preferences (codec, resolution, fps, display mode)
- [ ] Settings Compose UI
- [ ] Config sync: app → bridge → echo
- [ ] Bridge discovery (mDNS + manual IP)

### M7: Vehicle Integration
- [ ] GNSS forwarding (LocationManager → NMEA → bridge)
- [ ] VHAL properties (37 properties via Car API reflection)
- [ ] Navigation state display + maneuver icons

### M8: Cluster Display
- [ ] Cluster service for navigation: turn-by-turn maneuver icons, distance, road names
- [ ] Cluster service for media: album artwork, track info from Spotify/Apple Music/etc.
- [ ] Handle GM restrictions (third-party cluster services may be killed — detect and recover)
- [ ] Fallback rendering if cluster service is blocked

### M9: Steering Wheel Controls
- [ ] Media button mapping: skip forward, skip back, play/pause via `KeyEvent` interception
- [ ] Volume controls via `AudioManager` or `KeyEvent`
- [ ] Voice button interception: intercept the AAOS voice/assistant `KeyEvent` (currently launches Google Assistant) and forward as AA voice trigger to activate Gemini on the phone
- [ ] Investigate `KEYCODE_VOICE_ASSIST` / `KEYCODE_SEARCH` interception feasibility on GM AAOS (may require accessibility service or input method)

### M10: Polish
- [ ] Diagnostics screen
- [ ] Error recovery (reconnect, codec reset)
- [ ] Display modes (fullscreen, system bars)
- [ ] Overlay buttons (settings, stats)

---

## 🧭 Development Workflow

### One Milestone Per Conversation
Each milestone should be completed in a **single Copilot conversation**. When a milestone's exit criteria are met, **stop and tell the user to start a new conversation** for the next milestone. This keeps context focused and avoids degraded output from overly long conversations.

### How to Start Each Conversation
Open a new Copilot chat and say:
> "Let's build M[N]: [milestone name]. Start with [first task]."

Copilot will read the instruction files, repo memory, and this work plan automatically — no need to re-explain the project.

### Within a Milestone
- Prompt by island or logical task (e.g., "Build the Transport island", "Add unit tests for JSON parsing")
- Let Copilot finish each piece, verify no compile errors, then move to the next
- Copilot should check off `[ ]` items in this plan as they're completed

### Milestone Boundaries
- **Do not start the next milestone in the same conversation** — context quality degrades
- Between milestones: build, deploy to device/emulator, test manually, note any issues
- Start the next conversation with any issues or adjustments discovered during testing

### Parallel Work
Parallel Copilot sessions are **not recommended** for this project:
- Sessions don't communicate or coordinate file writes
- Build state isn't shared — one session can't see another's compile errors
- Island architecture helps in theory, but merge conflicts aren't worth the risk
- Sequential milestones have hard dependencies (M2 needs M1's transport, M3 needs M1's transport, M4 needs M2's surface, etc.)

### If a Conversation Gets Too Long
If Copilot starts losing context or producing lower quality output mid-milestone, it's fine to start a new conversation and say:
> "Continuing M[N]. [Island X] is done, [Island Y] still needs [specific tasks]."

---

## 💡 Future Ideas

### Two-Way Config Sync
- Bridge sends config echo after settings update
- App populates settings dialog from bridge echo, showing actual running config

### Stats Overlay Enhancements
- Parse SPS/PPS for actual stream resolution (not just codec init dims)
- Bridge-side stats (frames queued/dropped/written) sent via control channel
- Audio: PCM frame count, ring buffer fill level

### mDNS Discovery
- Bridge advertises `_openautolink._tcp` via Avahi
- App discovers automatically — no manual IP entry needed
- Fallback to manual IP for networks without mDNS

---

## Car Hardware Reference
- **SoC:** Qualcomm Snapdragon (2024 Chevrolet Blazer EV)
- **Display:** 2914×1134 physical, ~2628×800 usable (nav bar hidden)
- **HW Decoders:** H.264 (`c2.qti.avc.decoder`), H.265, VP9 — all 8K@480fps max
- **Network:** USB Ethernet NIC (car USB port), 100Mbps
