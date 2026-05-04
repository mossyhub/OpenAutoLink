# Multi-Display AA Projection — Implementation Plan

**Status**: Draft — not yet started.
**Owner**: TBD
**Targets**: 2024+ Blazer EV (cluster + main), other GM AAOS vehicles with passenger / aux screens.

This is a long-horizon plan to add support for **secondary projected displays** (instrument cluster, passenger / HUD / "auxiliary" screen) in OpenAutoLink, alongside the existing main-display projection. The motivation came from observing that GM's own AA sink (`com.gm.hmi.androidauto`) registers up to four `VideoSink` services for `MAIN`, `CLUSTER`, `AUXILIARY`, and `BIG_CARD` displays, and that the AA wire protocol fully supports this via `DisplayType` on the `MediaSinkService` descriptor.

## TL;DR

The AA protocol supports it, the aasdk supports it, GM's stack does it. Mechanically possible. The hard parts are:
1. AAOS Display routing (getting a `Surface` bound to the *actual* cluster panel)
2. Focus arbitration with GM's native cluster service
3. The phone's AA framework only ships **simplified** UIs to non-main displays — don't expect "full Google Maps on the cluster"

---

## Phase 0 — Discovery (zero-risk, ~1 day)

**Goal**: confirm the phone will offer a cluster stream, and capture what it actually sends. No rendering. No surface routing. Just protocol.

### Tasks

1. **Add a `cluster` and `aux` display-sink stub to the SDR builder** in [`app/src/main/cpp/jni_session.cpp`](app/src/main/cpp/jni_session.cpp).
   - In the `MediaSinkService` descriptor for the cluster service: set `display_type = DISPLAY_TYPE_CLUSTER`, `display_id = 1`, advertise a single 800×480 H.264 BP config (matches typical cluster widget size).
   - Auxiliary: `display_type = DISPLAY_TYPE_AUXILIARY`, `display_id = 2`, advertise 1280×720 H.264 BP.
   - Use unique aasdk channel IDs — see `aasdk::messenger::ChannelId` enum for existing assignments.

2. **Wire the channel handlers** but make them logging-only:
   - On `onChannelOpenRequest`: ACK with `STATUS_SUCCESS`, log it.
   - On `onMediaChannelSetupRequest` (SDR-style negotiation per channel): log the request, ACK.
   - On `onMediaWithTimestampIndication` (video frames): drop, but log size, NAL types, IDR/SPS detection — same scanner as main video.
   - On `onVideoFocusRequest`: reply with `VIDEO_FOCUS_PROJECTED` so the phone keeps streaming, but log mode/reason.

3. **Add a Settings flag** `experimental_multi_display_advertise` (off by default) so we can A/B without breaking the main path.

4. **Capture & analyze**:
   - Drive a session with the flag on, navigate Maps with active turn-by-turn, take a phone call.
   - Pull `OalLog` traces.
   - Verify: does the phone open the cluster channel? Send frames? At what cadence? What resolution does it negotiate? Does it use H.264 or try H.265?

### Success criteria

- Phone opens at least one secondary video channel.
- We capture the SDR response, the video-config-index chosen, and at least one IDR.
- No regression on main-display projection.

### Output

A short report (`docs/multi-display-discovery-results.md`) with the findings: what gets sent to cluster vs aux, IDR cadence, resolution, codec, what user actions trigger streams (e.g. only during nav?).

---

## Phase 1 — Cluster decode-to-buffer (no display routing, ~3-5 days)

**Goal**: decode the cluster stream into a `SurfaceTexture` we own and render it into a debug overlay on the main display so we can *see* what we're receiving without yet caring about routing it to the actual cluster panel.

### Tasks

1. **Generalize `MediaCodecDecoder`** — currently single-instance, single-Surface. Extract a `DecoderId` (main/cluster/aux) and allow N parallel instances. Each owns its own:
   - `MediaCodec`
   - `Surface`
   - SPS/PPS cache
   - IDR cache
   - render gate / seed-IDR warmup state

2. **Build `ClusterRenderer`**:
   - Owns a `SurfaceTexture` + GL preview tile
   - Receives frames from JNI via a new callback `onClusterVideoFrame(...)` (analogous to `onVideoFrame`)
   - Renders into a debug overlay tile in the main projection screen (top-right corner, ~1/4 size)

3. **Add corresponding callback methods** on the JNI bridge:
   - `JniSession::onClusterMediaWithTimestampIndication(...)` — same NAL scanner, calls `cbMethods_.onClusterVideoFrame`
   - Same for aux

4. **Settings UI**: gear → "Experimental" → "Show cluster preview" toggle. Off by default.

### Success criteria

- Cluster preview tile shows live phone-side cluster widget content during nav
- No FPS / latency hit on main display
- Cluster decoder survives reconnect / mode flip same as main

### Risks / Notes

- **Two parallel HW decoders** on the SoC. Qualcomm decoders on the Blazer can do this, but it costs concurrent decode slots. If we hit `IllegalStateException` on cluster init, automatically fall back to software decoder (`OMX.google.h264.decoder`) for the cluster only — main keeps HW.
- **SDR negotiation order** — the phone may open services in any order. Don't assume main comes first.

---

## Phase 2 — AAOS Display routing for cluster (the hard part, ~1-3 weeks)

**Goal**: actually push the decoded cluster surface to the physical cluster panel.

### The AAOS Display problem

On AAOS the cluster is a separate `Display` (multi-display device). To draw on it, an app needs:
- A `Surface` bound to a `Presentation` on that `Display`, OR
- Permission to create a `VirtualDisplay` that the system mirrors to the cluster, OR
- A privileged system signature that lets us call `DisplayManager.createDisplay(...)` with the cluster's display ID.

**On the Blazer specifically**: GM's `com.gm.cluster` and `com.gm.hmi.androidauto` are platform-signed and the cluster Display is owned by the cluster service. A regular Play Store app **cannot** present on it.

### Tasks

1. **Investigation (do first)**:
   - On a real Blazer head unit with shell access (or via remote logs):
     - `dumpsys display` to enumerate all Displays and their flags
     - `dumpsys window` to see who owns which display
     - Check if `Display.FLAG_PRIVATE` or `FLAG_OWN_CONTENT_ONLY` is set on the cluster
   - Determine if we can list the cluster Display from a normal app context

2. **Decide path**:
   - **Path A — System app**: ship OpenAutoLink as a system app installed via OEM/dealer flash. Gives us `WRITE_SECURE_SETTINGS`, the ability to enumerate all displays, and `Presentation` rights on the cluster. **Not viable for a Play Store app.**
   - **Path B — Companion service signed by GM**: requires a partnership we don't have. Skip.
   - **Path C — User-side ADB enable**: a one-time `pm grant com.openautolink.app android.permission.INTERNAL_SYSTEM_WINDOW` (or similar) via ADB. Possible on dev units, not user-friendly.
   - **Path D — Document the limitation**: cluster decode works (Phase 1 gives us a preview tile in the main display) but cluster routing requires OEM cooperation. Ship Phase 1 as "experimental cluster preview" and stop there for the public app.

3. **If Path A or C is chosen**, implement:
   - `ClusterPresentation extends Presentation` — minimal, single SurfaceView
   - Bind to cluster Display by ID once enumerated
   - Hand its `Surface` to the cluster `MediaCodecDecoder` via `setOutputSurface()`

### Success criteria

- Cluster panel shows projected AA cluster widget when projecting
- AAOS native cluster app re-takes the panel cleanly when projection stops or AA surrenders focus

### Risks

- **High probability we cannot ship this in the public app.** The investigation should determine that early — don't burn weeks on routing if Path D is the only option.
- Even with system signing, GM's cluster service may not yield the panel on demand — they have a focus state machine we'd be fighting.

---

## Phase 3 — Cluster focus arbitration (~3-5 days, only if Phase 2 succeeds)

**Goal**: handle `VideoFocusRequest` / `VIDEO_FOCUS_NATIVE` for the cluster channel correctly, mirroring GM's `onGALClusterSwitchToGaugeView()` pattern.

### Tasks

1. **Track per-display focus** — our current `JniSession::onVideoFocusRequest` only knows about main. Extend to a map keyed by channel.
2. **On cluster channel `VIDEO_FOCUS_NATIVE` request**:
   - Send the cluster decoder's surface back to the native cluster service (release the Presentation)
   - Reply `VIDEO_FOCUS_NATIVE` with `unsolicited=false`
3. **On cluster `VIDEO_FOCUS_PROJECTED` request after surrendering**:
   - Re-acquire the cluster Presentation
   - Reply `VIDEO_FOCUS_PROJECTED`
4. **Settings**: "Cluster projection mode" → Always projected / Only during nav / Off.

---

## Phase 4 — Auxiliary / passenger display (~1 week)

**Goal**: support an optional second main-style display (passenger screen, HUD).

### Why this is easier than cluster

- No native service fighting for the panel — passenger screens on Blazer trims that have them are owned by AAOS app context, not a privileged service.
- The phone's auxiliary stream is a *full* projection (more like a second main), not a stripped widget.
- Routing via `Presentation` works without system signing on most setups.

### Tasks

Mostly the same as Phase 1+2 but:
- `display_type = DISPLAY_TYPE_AUXILIARY`
- Decoder/Renderer mirrors main, not cluster
- User chooses which display via Settings dropdown (enumerate all non-main Displays via `DisplayManager.getDisplays()`)

---

## Phase 5 — Polish & ship (~1 week)

- Settings UI: Multi-display section
  - Toggle per display type (main always-on, cluster/aux opt-in)
  - "Cluster mode" picker
  - "Show cluster preview tile" debug toggle
- Diagnostics: per-display decoder stats in the existing diagnostics screen
- Reconnect: ensure all decoders flush + wait-for-IDR independently after wake
- Tests: integration test with two simulated channels (mock phone)

---

## Protocol & code references

### Wire protocol

- [`DisplayType.proto`](../external/opencardev-aasdk/protobuf/aap_protobuf/service/media/sink/message/DisplayType.proto) — `MAIN=0, CLUSTER=1, AUXILIARY=2`
- [`MediaSinkService.proto`](../external/opencardev-aasdk/protobuf/aap_protobuf/service/media/sink/MediaSinkService.proto) — has `display_type` and `display_id` on the SDR descriptor
- [`VideoFocusRequestNotification.proto`](../external/opencardev-aasdk/protobuf/aap_protobuf/service/media/video/message/VideoFocusRequestNotification.proto) — focus request includes mode + reason
- [`VideoFocusReason.proto`](../external/opencardev-aasdk/protobuf/aap_protobuf/service/media/video/message/VideoFocusReason.proto) — `PHONE_SCREEN_OFF=1, LAUNCH_NATIVE=2`

### GM reference (decompiled, do not copy)

Treat as design inspiration only:
- `recon_dump/apks/com_gm_hmi_androidauto/java_src/com/google/android/projection/sink/video/GALDisplayManager.java` — registers main/cluster/aux/bigcard
- `GALDisplayManager.onVideoFocusRequest` (lines 313-332) — cluster-specific arbitration
- `ProjectionRenderer.java` — single-codec lifecycle (we want N of these)

### aasdk

- `aasdk::channel::av::IVideoServiceChannel` — already supports any number of instances; we just don't currently create more than one.
- `aasdk::messenger::ChannelId` — need to assign new IDs for the cluster/aux services. Coordinate with the `mossyhub/aasdk` fork in `external/opencardev-aasdk` (the `openautolink` branch).

### App

- [`jni_session.cpp:1080-1290`](../app/src/main/cpp/jni_session.cpp) — current single-display SDR builder. Needs to emit additional `MediaSinkService` blocks for cluster/aux when the experimental flag is on.
- [`MediaCodecDecoder.kt`](../app/src/main/java/com/openautolink/app/video/MediaCodecDecoder.kt) — needs to become per-decoder-instance.

---

## Decision points

| When | Decide |
|------|--------|
| End of Phase 0 | Does the phone offer a cluster stream at all? If no, kill the project. |
| Mid Phase 2 | Can we route to the physical cluster Display from a non-system app? If no, ship Phase 1 as "preview tile" only. |
| End of Phase 2 | Does GM's cluster service yield the panel on focus surrender? If no, restrict cluster to "only during projection start" not on-demand. |

---

## Out of scope

- "Full Google Maps UI on the cluster" — the phone's AA framework chooses what to send, and historically only ships a turn-arrow widget for cluster surfaces. Native AAOS Maps uses a different mechanism (`DisplayManager` directly) that is not part of the AA wire protocol.
- BIG_CARD display — GM uses it for some Cadillac/Hummer pillar displays. Skip until someone reports a vehicle that needs it.
- Multi-phone with multi-display — already complex enough with one phone.

---

## Open questions

1. Does the phone require us to advertise `available_while_in_call=true` on cluster to get any stream? (Test in Phase 0.)
2. Does the phone honor `display_id` (i.e. can we have two main-type displays on different IDs), or is it strictly one stream per `display_type`?
3. On reconnect after car-sleep, does the cluster channel need a separate IDR request, or does main's request also kick the cluster?
4. Audio implications: cluster has no speakers, but does the phone try to route any audio types differently when it sees a cluster sink?
