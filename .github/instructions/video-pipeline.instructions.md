---
description: "Use when working on video decoding, MediaCodec configuration, Surface/SurfaceView lifecycle, codec selection (H.264/H.265/VP9), or video frame handling. Contains AAOS-specific hardware decoder knowledge."
---
# Video Pipeline Knowledge

## MediaCodec Lifecycle (Critical)
1. `createDecoderByType(mime)` — select codec
2. `configure(format, surface, null, 0)` — bind to Surface
3. `start()` — begin decode loop
4. On pause: `codec.stop()` then `codec.release()` — MUST release, not just stop
5. On resume: create new codec instance, reconfigure, start
6. Surface change (orientation, resize): full codec reset required

## Codec Selection
- H.264: `video/avc` — universal, safe default. Most tested
- H.265: `video/hevc` — better compression, same quality. GM Snapdragon has HW decoder
- VP9: `video/x-vnd.on2.vp9` — skip NAL sync gate (no start codes). GM has HW decoder
- AV1: `video/av01` — future. Check `MediaCodecList` for HW support before offering

## GM Blazer EV Snapdragon HW Decoders
- `c2.qti.avc.decoder` — H.264 up to 8192x4320 (HW)
- `c2.qti.hevc.decoder` — H.265 same limits (HW)
- `c2.qti.vp9.decoder` — VP9 (HW)
- Both OMX and C2 (Codec2) variants available. Prefer C2

## Frame Handling
- Video frames are OAL protocol: 12-byte header + raw codec data
- Header: `[width:u16le][height:u16le][pts_ms:u32le][flags:u16le][reserved:u16le]`
- Flags bit 0: keyframe (IDR). Flags bit 1: codec config (SPS/PPS/VPS)
- First frame MUST be codec config (SPS/PPS for H.264, VPS/SPS/PPS for H.265)
- Drop frames until first IDR received — P-frames without reference corrupt display
- Force even dimensions for macroblock alignment: `width and 0xFFFFFFFE.toInt()`

## Surface Sizing
- AAOS display: 2914x1134 physical on GM. Usable area smaller (nav bar, status bar)
- SCALE_TO_FIT_WITH_CROPPING if bridge resolution != display aspect ratio
- SurfaceView preferred over TextureView (HWC overlay, lower latency)

## H.264 NAL Parsing
- Start code: `0x00 0x00 0x00 0x01` or `0x00 0x00 0x01`
- NAL type: `byte & 0x1F` — 7=SPS, 8=PPS, 5=IDR, 1=non-IDR
- H.265: NAL type `(byte >> 1) & 0x3F` — 32=VPS, 33=SPS, 34=PPS, 19/20=IDR

## Video Rules (Non-Negotiable)
- Projection is LIVE UI STATE, not video playback
- Late frames: DROP. Never queue, never buffer
- Corruption: RESET codec immediately, request IDR from bridge
- Video may drop. Audio may buffer. Neither may block the other
