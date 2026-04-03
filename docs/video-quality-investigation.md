# Video Quality Investigation — Real-Time UI Projection

## Problem Statement

Android Auto projection video appears blocky/pixelated during motion (map scrolling, UI transitions). This is a **real-time UI projection stream**, not video playback — latency matters more than perfect quality, but visible compression artifacts during normal use are unacceptable.

## Test Setup

- **Phone**: OnePlus CPH2655 → WiFi 5GHz → VIM4 bridge (aasdk wireless AA)
- **Bridge**: Khadas VIM4, `openautolink-headless`, OAL protocol over TCP
- **App**: AAOS emulator (BlazerEV_AAOS, x86_64, 2400×960)
- **Network path**: Emulator → adb reverse → PC localhost → tcp_relay.py → SBC SSH NIC (192.168.137.x) → bridge ports 5288/5289/5290
- **Codec**: H.264 (H.265 crashes emulator's goldfish decoder)
- **Resolution**: 1080p60 (phone encodes at 1920×1080 @ 60fps)
- **Display**: 2400×960 surface (AA content scaled to fit)

## What's Been Fixed So Far

| Fix | Impact |
|-----|--------|
| Video decode moved off main thread → dedicated `VideoDecodeInput` thread | Eliminated "Skipped N frames" main thread blocking |
| `KEY_LOW_LATENCY=1` + `priority=0` on MediaCodec | Decoder knows this is real-time, not file playback |
| `INPUT_TIMEOUT_US` changed from 0 (non-blocking drop) to 8ms | Dramatically reduced silent frame drops |
| `_needsKeyframe = true` after codec reconfigure | Fixed initial black/corrupt frames after codec change |
| 256KB socket receive buffer on video channel | Prevents bursty IDR keyframes from being truncated |
| `tcpNoDelay = true` on video socket | Disables Nagle's algorithm for lower latency |
| **Combined SPS/PPS+IDR frame handling** | When bridge sends both flags in one frame, codec is configured AND keyframe is queued. Previously the IDR data was lost, causing P-frame drops until next standalone IDR |
| **SharedFlow `DROP_OLDEST` + 8-frame buffer** | Video SharedFlow changed from `extraBufferCapacity=2` (blocking) to 8 with `DROP_OLDEST`. Uses `tryEmit()` to avoid suspension on the TCP reader thread |
| **Late frame drop policy** | Stale P-frames (PTS went backwards) are dropped immediately. Adaptive timeout: when decoder falls behind (3+ consecutive drops), P-frames use non-blocking check while keyframes still wait |
| **Bitrate tracking** | Rolling 1-second window calculates effective encoder bitrate (kbps). Warns when below 2 Mbps at 1080p60. Displayed in stats overlay and diagnostics screen |

## Remaining Issues

### 1. Compression artifacts during motion
- Visible blockiness when scrolling Google Maps or during UI transitions
- Static content looks fine — issue is specifically with motion
- 1080p60 H.264 on gigabit ethernet should be trivially smooth

### 2. Root cause analysis (from investigation)

#### A. Phone encoder bitrate — MOST LIKELY PRIMARY CAUSE
- The bridge sends resolution tier + codec type to the phone but **does NOT set a target bitrate**
- The phone's AA encoder chooses its own bitrate (typically 2-6 Mbps for 1080p60)
- At low bitrate, P-frames during motion need more bits for motion vectors/residuals → visible quantization artifacts
- **Bitrate tracking now implemented** — check the stats overlay to see actual bitrate
- **If bitrate < 2 Mbps**: the phone encoder is severely constrained. Options:
  - Lower resolution tier from 3 (1080p) to 2 (720p) in bridge config — fewer pixels = more bits per pixel
  - H.265 instead of H.264 — better compression at same bitrate
  - Accept that this is a phone-side limitation
- **If bitrate ≥ 4 Mbps**: encoder quality is adequate, blockiness is elsewhere in the pipeline

#### B. tcp_relay.py overhead — STILL OPEN
- Current path: Emulator → adb reverse → tcp_relay.py (Python) → SBC
- Python's `socket.recv(65536)` + `socket.sendall()` adds per-frame syscall overhead
- **Test with direct ethernet to SBC (Bridge NIC) to eliminate relay**
- If relay is the bottleneck, rewrite in C or use direct connection
- Note: this only affects the emulator test setup, not real car deployment

#### G. Combined SPS/PPS + IDR frame bug — FIXED
- **Bug**: When the bridge sends a combined frame with both `FLAG_CODEC_CONFIG | FLAG_KEYFRAME` flags, the `when` block checked `isCodecConfig` first. This configured the codec correctly but set `receivedIdr = false` and never queued the IDR data to the decoder.
- **Impact**: All P-frames were dropped until the next *standalone* IDR arrived (could be several seconds). This caused frozen video at session start or after reconnection.
- **Fix**: Added explicit `frame.isCodecConfig && frame.isKeyframe` case that configures the codec AND queues the keyframe. MediaCodec handles redundant SPS/PPS in the bitstream gracefully.

#### C. SharedFlow buffer and GC pressure — FIXED
- **Fixed**: SharedFlow now uses `extraBufferCapacity = 8` with `BufferOverflow.DROP_OLDEST`
- **Fixed**: Uses `tryEmit()` instead of suspending `emit()` — TCP reader thread never blocks on Flow
- GC pressure from ByteArray allocation at 60fps (~3 MB/s) is manageable by Android GC
- If GC pauses become visible, consider ByteBuffer pool (not implemented — premature optimization)

#### D. No frame drop policy for late frames — FIXED
- **Fixed**: Stale P-frames (PTS went backwards) are dropped immediately
- **Fixed**: Adaptive timeout — after 3+ consecutive drops, P-frames use `dequeueInputBuffer(0)` (non-blocking) while keyframes still wait up to 8ms
- Consecutive drop counter resets on successful queue — decoder self-recovers

#### E. MediaCodec output drain efficiency — ACCEPTABLE
- `releaseOutputBuffer(index, true)` renders immediately — correct for live streaming
- Timestamp-based rendering (`releaseOutputBuffer(index, timestampNs)`) would add latency for pacing
- 1ms poll timeout is fine — the drain thread is cheap and correctly dedicated

#### F. Video frame parsing overhead — ACCEPTABLE
- `readFully(header)` + `readFully(payload)` is 2 syscalls per frame — adequate
- Buffered stream at 64KB amortizes TCP reads
- ByteArray allocation per frame is the main cost, but at ~50KB/frame × 60fps = 3MB/s, Android GC handles this

## Investigation Method

For each hypothesis, follow this procedure:

1. **Add targeted logging** — timestamps at each pipeline stage (TCP read → SharedFlow emit → decoder input queue → decoder output → render)
2. **Build and deploy** — `.\gradlew :app:assembleDebug && adb install -r ...`
3. **Generate motion** — `adb shell input swipe 800 400 400 400 300` (fast map drag)
4. **Capture screenshot** — `adb exec-out screencap -p > screenshot.png` during motion
5. **Collect logs** — `adb logcat -d --pid=$(adb shell pidof com.openautolink.app)`
6. **Analyze** — check frame timing, drops, decode latency
7. **Also check bridge side** — `ssh khadas@192.168.137.167 "sudo journalctl -u openautolink -n 30 --no-pager"`

## Key Design Principles

From [copilot-instructions.md](.github/copilot-instructions.md):

> **Projection streams are live UI state, not video playback.**
> Late frames must be dropped. Corruption must trigger reset.
> Video may drop. Audio may buffer. Neither may block the other.
> On reconnect: flush decoder, discard stale buffers, wait for IDR before rendering.
> Keep codec and AudioTracks pre-warmed where possible.

## Environment Notes

- Emulator's goldfish HEVC decoder is broken (`config failed => CORRUPTED`). Use H.264 for emulator testing.
- VIM4 bridge: `khadas@192.168.137.167`, bridge on ports 5288/5289/5290
- TCP relay: `python d:\personal\carlink_native\scripts\tcp_relay.py <port> 192.168.137.167 <port>` (3 instances)
- ADB reverse: `adb reverse tcp:5288 tcp:5288` (3 ports)
- App bridge host must be `127.0.0.1` when using relay chain
- Phone AA session codec is determined at connection time. Changing `OAL_AA_CODEC` in env requires bridge restart + phone reconnect via "Save & Restart Bridge" button in app settings
