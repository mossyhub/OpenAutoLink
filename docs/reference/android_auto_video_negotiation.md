# Android Auto Resolution / Framerate / DPI / Codec Negotiation

Deep-dive analysis of the decompiled Android Auto APK (`external/aa_apk`), mapping how the phone determines what video resolution, framerate, DPI, and codec to use when projecting to a head unit.

---

## 1. Protocol Handshake Flow

The connection follows a **3-phase negotiation**:

1. `SDP_NEGOTIATION` — Bluetooth SDP service discovery
2. `VERSION_NEGOTIATION` — Protocol version agreement
3. `SSL_NEGOTIATION` — TLS encryption setup

After SSL completes, a **Service Discovery** exchange occurs where the HU sends its display capabilities and the phone determines what it can offer.

**Key files:** `smali/ikm.smali`, `smali/rcc.smali`, `smali/rgv.smali`

---

## 2. Resolution Tiers

The phone knows about **9 fixed resolution tiers** defined in the `wvk` enum (`smali/wvk.smali`):

| Field | Name | Proto ID |
|-------|------|----------|
| `a` | `VIDEO_800x480` | 1 |
| `b` | `VIDEO_1280x720` | 2 |
| `c` | `VIDEO_1920x1080` | 3 |
| `d` | `VIDEO_2560x1440` | 4 |
| `e` | `VIDEO_3840x2160` | 5 |
| `f` | `VIDEO_720x1280` | 6 (portrait) |
| `g` | `VIDEO_1080x1920` | 7 (portrait) |
| `h` | `VIDEO_1440x2560` | 8 (portrait) |
| `i` | `VIDEO_2160x3840` | 9 (portrait) |

---

## 3. Resolution-to-Codec Mapping

`smali/ifv.smali` maps resolution tiers to which resolutions are **allowed** at each tier:

- **480p**: Only `VIDEO_800x480`
- **720p**: `VIDEO_800x480`, `VIDEO_720x1280`, `VIDEO_1280x720`
- **1080p**: Above + `VIDEO_1080x1920`, `VIDEO_1920x1080`
- **1440p**: Above + `VIDEO_1440x2560`, `VIDEO_2560x1440`
- **2160p**: Above + `VIDEO_2160x3840`, `VIDEO_3840x2160`

A special case exists for **Subaru/HARMAN** head units (hardcoded allowlist in `ifv.b`).

---

## 4. HU Display Config (Protobuf Message)

The HU sends a protobuf message (`wvl` class, `smali/wvl.smali`) with **11 fields**:

| Field | Type | Purpose |
|-------|------|---------|
| `b` | int | Bitfield flags (bit 0x10 = "has density info") |
| `c`-`f` | int | Display rect dimensions |
| `g` | int | Density (DPI) — must be > 0 |
| `h`-`k` | int | Additional display params |
| `l` | int | Codec type (maps to `wqv` codec enum) |
| `m` | `wuy` | Additional sub-message |

---

## 5. FPS Handling

### Validation (`smali/ify.smali`)

- Only **30 FPS** (`0x1e`) and **60 FPS** (`0x3c`) are accepted
- Any other value throws `"wrong FPS"` error

### Dynamic FPS Restrictions (`smali/abvc.smali`)

The `FrameRateRestrictions` config can **cap FPS** based on:

| Parameter | Purpose |
|-----------|---------|
| `cap_fps_when_non_interactive_limit` | FPS cap when screen not interactive |
| `fps_limit_for_power_low_display_idle` | FPS cap on low battery + idle |
| `fps_limit_for_power_low_display_interactive` | FPS cap on low battery + active |
| `fps_limit_for_power_normal_display_idle` | FPS cap on normal battery + idle |
| `fps_limit_for_restriction_level_high/medium/low` | Tiered thermal throttling |
| `min_thermal_status_for_frame_rate_restrictions` | Thermal threshold to start capping |
| `min_number_of_displays_to_trigger_restrictions` | Multi-display trigger |
| `cap_fps_when_non_interactive_cars` | Per-car allowlist for idle FPS caps |
| `disable_interactivity_tracking` | Override flag |
| `use_power_save_mode_for_battery_level` | Battery-based throttling |

Dynamic frame rate can be disabled per-car via:
`ProjectionWindowManager__disable_dynamic_frame_rate_list`

---

## 6. DPI/Density Handling

DPI comes from the HU's protobuf message:

1. Bit `0x10` in the flags field (`wvl.b`) must be set — otherwise `"density missing"` error
2. DPI value (`wvl.g`) must be > 0 — otherwise `"wrong density"` error
3. Gets stored in `DisplayParams.i` (the `dpi` field)
4. Used to set `Configuration.densityDpi` on the `ProjectedPresentation` virtual display

---

## 7. Codec Selection

### Supported Video Codecs (`smali/wqv.smali`)

| Field | Name | Proto ID |
|-------|------|----------|
| `a` | `MEDIA_CODEC_AUDIO_PCM` | 1 |
| `b` | `MEDIA_CODEC_AUDIO_AAC_LC` | 2 |
| `c` | `MEDIA_CODEC_VIDEO_H264_BP` | 3 |
| `d` | `MEDIA_CODEC_AUDIO_AAC_LC_ADTS` | 4 |
| `e` | `MEDIA_CODEC_VIDEO_VP9` | 5 |
| `f` | `MEDIA_CODEC_VIDEO_AV1` | 6 |
| `g` | `MEDIA_CODEC_VIDEO_H265` | 7 |

### Codec Selection Flow (`smali/ify.smali`)

1. **HU requests** a codec type via the display config protobuf
2. Phone checks if codec is in the **supported set** (`H264_BP`, `H265`, `VP9`)
3. For **H.265 specifically**:
   - Enumerates `MediaCodecList` for hardware encoders supporting `video/hevc`
   - Checks `profileLevels` from `MediaCodecInfo.CodecCapabilities`
   - Runs a **real test encode** of 2 frames with a 3-second semaphore timeout
   - If test fails → logs `"The H.265 video codec requested by the HU is not supported by the phone."`
4. Fallback to H.264 if H.265 is unsupported
5. `VideoEncoderParams__force_software_codec` can force software encoding
6. `VideoEncoderParams__enable_max_h264_encoder_compatibility` widens H.264 encoder selection

### H.264 vs H.265 Encoder Classes

- `smali/ihc.smali` — H.264 encoder (checks `video/avc` profile levels)
- `smali/ihd.smali` — H.265 encoder (checks `video/hevc` profile levels)
- `smali/iho.smali` — Base encoder class (MediaCodecList enumeration, hardware detection)

---

## 8. Video Encoder Configuration (DisplayParams)

The final `DisplayParams` structure (`smali/ifs.smali`) passed to the encoder:

| Field | Type | Name |
|-------|------|------|
| `a` | int | selectedIndex |
| `b` | int | codecWidth |
| `c` | int | codecHeight |
| `d` | int | fps |
| `e` | int | dispWidth |
| `f` | int | dispHeight |
| `g` | int | dispLeft |
| `h` | int | dispTop |
| `i` | int | dpi |
| `j` | float | pixelAspectRatio |
| `k` | int | decoderAdditionalDepth |
| `l` | float | scaledPixelAspectRatio |
| `m` | Size | scaledDimensions |
| `n` | Rect | stableInsets |
| `o` | Rect | contentInsets |
| `p` | List | cutouts |
| `q` | CarDisplayUiFeatures | UI features |

The encoder logs: `"Configuring %s codec with width: %d height: %d bit rate: %d iframe interval: %d"`

---

## 9. Bitrate Adaptation (VideoEncoderParams)

`smali/acce.smali` controls adaptive bitrate with per-codec tuning:

| Parameter | Type | Purpose |
|-----------|------|---------|
| `adaptive_bitrate_baseline_h264` | long | Starting bitrate for H.264 |
| `adaptive_bitrate_baseline_h265` | long | Starting bitrate for H.265 |
| `adaptive_bitrate_minimal_h264` | long | Floor bitrate for H.264 |
| `adaptive_bitrate_minimal_h265` | long | Floor bitrate for H.265 |
| `adaptive_bitrate_gradient_h264` | double | Ramp rate for H.264 |
| `adaptive_bitrate_gradient_h265` | double | Ramp rate for H.265 |
| `sixty_fps_bitrate_multiplier` | double | Extra bitrate multiplier for 60fps |
| `sixty_fps_bitrate_multiplier_h265` | double | Extra bitrate multiplier for 60fps (H.265) |
| `bitrate_adjustment_exponent` | double | Exponent for bitrate adjustment curve |
| `key_frame_interval_wireless` | long | I-frame interval for wireless connections |
| `key_frame_interval_ackless` | long | I-frame interval for ackless mode |
| `max_qp_wireless` | long | Max quantization parameter (wireless) |
| `min_qp_wireless` | long | Min quantization parameter (wireless) |
| `roi_qp_offset` | long | Region-of-interest QP offset |
| `roi_qp_offset_margin` | long | ROI QP offset margin |
| `throw_encoder_errors_percent` | double | Error threshold percentage |
| `force_software_codec` | bool | Force software encoding |
| `enable_max_h264_encoder_compatibility` | bool | Widen H.264 encoder selection |
| `enable_max_h264_encoder_compatibility_fast_track` | bool | Fast-track compatibility mode |

---

## 10. Video Configuration Change Flow

When the HU requests a config change:

1. `doVideoConfigChange` is called (`smali/iab.smali`)
2. `doVideoConfigChangeInner` processes it with `type` and `config` (`smali/iac.smali`)
3. `onVideoConfigurationChanged` notifies components (`smali/iac.smali`, `smali/irn.smali`)
4. `ProjectedPresentation#setVideoConfiguration` updates the virtual display config (`smali/qkc.smali`)
5. `Resources.updateConfiguration()` is called with the new `Configuration` + `DisplayMetrics`
6. `onVideoConfigurationChangedComplete` signals completion

---

## 11. Widescreen / Aspect Ratio Handling

`smali/acat.smali` (SystemUi config) controls:

| Parameter | Purpose |
|-----------|---------|
| `widescreen_aspect_ratio_breakpoint` | Aspect ratio threshold for widescreen mode |
| `semi_widescreen_breakpoint_dp` | DP threshold for semi-widescreen |
| `widescreen_breakpoint_dp` | DP threshold for full widescreen |
| `block_content_area_requests_in_immersive_mode` | Immersive mode handling |

---

## Summary: What Determines What Gets Sent

1. **The HU declares** its display size (resolution rect), density (DPI), FPS (30 or 60), and preferred codec type via a protobuf message during service discovery
2. **The phone validates** FPS (must be 30 or 60), density (must be > 0 with flag bit set), and resolution (must have positive width and height)
3. **The phone probes** its hardware encoders via `MediaCodecList` to determine codec support — especially for H.265, where a real test encode is performed
4. **If H.265 is requested but unsupported**, falls back to H.264
5. **Bitrate is adaptively managed** with separate tuning curves per codec, with 60fps getting a bitrate multiplier
6. **FPS can be dynamically capped** by thermal state, battery level, and car-specific allowlists
7. **Resolution is constrained** to the fixed tier set — there are no arbitrary resolutions

---

## Key Obfuscated Class Index

| Obfuscated | Purpose |
|------------|---------|
| `wvk` | Video resolution enum (9 tiers) |
| `wqv` | Media codec type enum (H264/H265/VP9/AV1/AAC) |
| `wvl` | HU display config protobuf message |
| `wra` | HU service discovery response |
| `ifs` | DisplayParams — final encoder config |
| `ifv` | Resolution tier → allowed resolutions mapping |
| `ify` | Main video negotiation logic |
| `iho` | Base video encoder (MediaCodecList scanner) |
| `ihc` | H.264 encoder |
| `ihd` | H.265 encoder |
| `ihj` | MediaCodec wrapper |
| `acce` | VideoEncoderParams config |
| `abvc` | FrameRateRestrictions config |
| `abzp` | ProjectionWindowManager config |
| `acat` | SystemUi config (widescreen breakpoints) |
| `qkc` | ProjectedPresentation (virtual display) |
| `iac` | Video config change handler |
| `ikm` | Connection negotiation (SDP/VERSION/SSL) |
| `ivm` | Head unit info database |
