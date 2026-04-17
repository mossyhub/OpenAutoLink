# Undocumented Android Auto Features — Reverse Engineering Reference

## Source

Decompiled from the Android Auto phone APK (JADX), April 2026.
Files are in `Gmaps_teardown/aa_apk_src/`.

---

## 1. HU Capability Flags (`qmr.java`)

The phone AA app checks a bitmask of **53 head unit capabilities**. These are negotiated
during session setup and gate which features the phone enables. Each flag is a bit position
in a capabilities bitmask.

### Flag Definitions

| ID | Name | Description |
|----|------|-------------|
| 0 | `RESERVED_1` | Reserved |
| 1 | `RESERVED_2` | Reserved |
| 2 | `CAR_WINDOW_RESIZABLE` | HU supports window resize (resize type enum) |
| 3 | `INT_SETTINGS_AVAILABLE` | HU has internal settings screen |
| 4 | `THIRD_PARTY_ACCESSIBLE_SETTINGS` | Third-party apps can access HU settings |
| 5 | `CLIENT_SIDE_FLAGS` | HU sends client-side feature flags |
| 6 | `CONTENT_WINDOW_INSETS` | HU reports content area insets (safe areas) |
| 7 | `ASSISTANT_Z` | Z-ordering control for Google Assistant overlay |
| 8 | `START_CAR_ACTIVITY_WITH_OPTIONS` | HU supports activity launch options |
| 9 | `STICKY_WINDOW_FOCUS` | Window focus persists across interactions |
| 10 | `NON_CONTENT_WINDOW_ANIMATIONS_SAFE` | Safe to animate non-content windows |
| 11 | `WINDOW_REQUIRES_ONE_TEXTURE_UPDATE_TO_DRAW` | Rendering optimization flag |
| 12 | `CONNECTION_STATE_HISTORY` | HU tracks connection state history |
| 13 | `CLEAR_DATA` | HU supports clearing cached data |
| 14 | `START_DUPLEX_CONNECTION` | HU can initiate bidirectional connections |
| 15 | `WINDOW_OUTSIDE_TOUCHES` | HU reports touches outside projected window |
| 16 | `CAR_WINDOW_REQUEST_FOCUS` | HU can request focus on car window |
| 17 | `SUPPORTS_SELF_MANAGED_CALLS` | HU handles phone calls natively |
| 18 | `START_FIRST_CAR_ACTIVITY_ON_FOCUS_GAINED` | Auto-launch app on focus |
| 19 | `MULTI_DISPLAY` | **HU has multiple displays (cluster, passenger, etc.)** |
| **20** | **`ENHANCED_NAVIGATION_METADATA`** | **Enables VehicleEnergyForecast from Maps back to HU. Required for EV routing data return path.** |
| 21 | `INDEPENDENT_NIGHT_MODE` | HU controls its own night/day theme |
| 22 | `CLUSTERSIM` | Cluster simulation mode |
| 23 | `MULTI_REGION` | HU supports multiple display regions |
| 24 | `MICROPHONE_DIAGNOSTICS` | HU provides mic diagnostic data |
| 25 | `AUDIO_STREAM_DIAGNOSTICS` | HU provides audio stream diagnostics |
| 26 | `MANIFEST_QUERYING` | HU can query app manifests |
| 27 | `PREFLIGHT` | Pre-session validation checks |
| 28 | `LIFECYCLE_BEFORE_LIFETIME` | Activity lifecycle ordering control |
| 29 | `POWER_SAVING_CONFIGURATION` | HU reports power saving state |
| 30 | `INITIAL_FOCUS_SETTINGS` | HU controls initial focus target |
| **31** | **`COOLWALK`** | **Enables the split-screen Coolwalk UI layout (newer AA design)** |
| 32 | `USE_CONFIGURATION_CONTEXT` | HU uses configuration-aware context |
| **33** | **`DRIVER_POSITION_SETTING`** | **Tells AA which side the driver sits on (LHD/RHD)** |
| 34 | `UPDATE_PRESENTATION_INPUT_CONFIGURATION_AT_STARTUP_END` | Input config timing |
| 35 | `PROJECTED_PRESENTATION_WAIT_UNTIL_CONFIGURED` | Wait for config before rendering |
| 36 | `CRASH_PROJECTED_PRESENTATION_IF_NOT_CONFIGURED` | Error handling mode |
| 37 | `COOLWALK_ROTARY_PROXIMITY_NAVIGATION` | Rotary input for Coolwalk UI |
| 38 | `GUARD_AGAINST_NO_WINDOW_FOCUS_CHANGE_KILL_SWITCH` | Focus error recovery |
| 39 | `GH_DRIVEN_RESIZING` | Gearhead-driven display resize |
| **40** | **`NATIVE_APPS`** | **Enables native app tiles in AA launcher** |
| 41 | `HIDE_TURN_CARDS_ON_SECONDARY_DISPLAYS` | Cluster turn card control |
| **42** | **`HERO_CAR_CONTROLS`** | **Enables the car controls panel (HVAC, seat heat, defrost) in AA** |
| **43** | **`HERO_CAR_LOCAL_MEDIA`** | **Enables local media playback controls from HU sources** |
| 44 | `HERO_PUNCH_THROUGH` | Native UI punch-through for Hero cards |
| 45 | `REMOVE_WINDOW_FOCUS_THROUGH_ON_INPUT_FOCUS_CHANGED_KILL_SWITCH` | Focus fix |
| **46** | **`HERO_THEMING`** | **Enables OEM color/brand theming in AA interface** |
| 47 | `PERSIST_PROJECTION_CONFIGURATION_CONTEXT` | Persist display config |
| 48 | `APP_CONTROLLED_IMMERSIVE_MODE` | App-driven immersive/fullscreen mode |
| 49 | `USE_INTERNAL_CONTEXT` | Internal context usage flag |
| 50 | `ONLY_PROCESS_CAR_CONFIGURATION_CHANGE` | Filter config changes |
| **51** | **`CIELO`** | **New rendering engine (codename), possibly successor to Coolwalk** |
| 52 | `PROCESS_FONT_WEIGHT_ADJUSTMENT_CONFIGURATION_CHANGE` | Font weight changes |

### How to Set

These flags are NOT in the aasdk ServiceDiscoveryResponse protobuf. They're communicated
through the GMS CarService Binder API. To implement:

1. Find how the phone reads these from the HU (likely via `CarDisplayUiFeatures` or a
   GMS IPC call during session setup)
2. Implement a GMS-compatible service that responds with our capability bitmask
3. OR find if any of these map to aasdk ServiceDiscoveryResponse fields

### Priority for Implementation

- `ENHANCED_NAVIGATION_METADATA` (20) — Already working implicitly (phone requested sensor 23)
- `COOLWALK` (31) — Would enable the modern split-screen AA layout
- `DRIVER_POSITION_SETTING` (33) — Simple, useful for RHD markets
- `HERO_CAR_CONTROLS` (42) — HVAC controls in AA (requires car controls API)
- `HERO_THEMING` (46) — Custom OEM look and feel

---

## 2. UI Element Flags (`wys.java`)

Bitmask flags in `CarDisplayUiFeatures` indicating what the HU natively displays,
so the phone AA app can hide redundant elements.

| Bit | Name | Effect |
|-----|------|--------|
| 1 | `UI_ELEMENT_CLOCK` | Phone hides clock from status bar (HU has its own) |
| 2 | `UI_ELEMENT_BATTERY_LEVEL` | Phone hides battery indicator |
| 4 | `UI_ELEMENT_PHONE_SIGNAL` | Phone hides signal strength |
| 8 | `UI_ELEMENT_NATIVE_UI_AFFORDANCE` | HU has native hamburger/home button |
| 16 | `UI_ELEMENT_NAVIGATION_TURN_DATA_AVAILABLE` | HU shows turn-by-turn natively |

### Wire Format (`CarDisplayUiFeatures.java`)

```java
CarDisplayUiFeatures {
    int resizeType;    // display resize mode
    int uiElements;    // bitmask of UI_ELEMENT_* flags
}
```

### How to Set

This is sent via the GMS `CarDisplayUiFeatures` Parcelable through the car service
Binder interface. We would need to register as a GMS car service provider.

### Currently Used

Our `sessionConfig` bitmask (bit0=hideClock, bit1=hideSignal, bit2=hideBattery) in
`DirectAaTransport` already controls similar behavior through the aasdk
ServiceDiscoveryResponse. The UI element flags are the GMS-level equivalent.

---

## 3. Car Controls API (`com.google.android.gms.car.control/`)

The car controls feature lets AA display HVAC, seat heating, defrost, and other vehicle
controls directly in the AA interface. OEM head units use this to expose climate controls.

### Data Model

```
CarControl {
    CarPropertyControl property;    // VHAL property binding
    CarAction action;               // what happens on tap
    CarControlGroup group;          // grouping (HVAC, seats, etc.)
    boolean isToggleable;           // on/off vs slider
    List<CarAreaId> areaIds;        // zone (driver/passenger/rear)
    int controlType;                // control UI type
}

CarPropertyControl {
    int propertyId;                 // VHAL property ID
    int areaId;                     // VHAL area ID
    CarPropertyConfig config;       // min/max/step
}

CarControlGroup — groups controls into categories:
    - Climate (temp, fan, AC, defrost)
    - Seats (heat, ventilation, position)
    - Steering wheel heat
    - Mirrors
```

### VHAL Properties Needed

For a Chevy Blazer EV, the relevant HVAC properties are:
- `HVAC_TEMPERATURE_SET` (0x11600503) — temperature setpoint
- `HVAC_FAN_SPEED` (0x11400500) — fan speed
- `HVAC_AC_ON` (0x11200505) — AC on/off
- `HVAC_DEFROSTER` (0x11200504) — front/rear defrost
- `HVAC_SEAT_TEMPERATURE` (0x1140050B) — seat heat level
- `HVAC_STEERING_WHEEL_HEAT` (0x1140050C) — steering wheel heat

### How to Implement

1. Read HVAC VHAL properties via CarPropertyManager (same pattern as VehicleDataForwarder)
2. Build `CarControl` objects describing each control
3. Register them via the GMS car controls Binder service
4. Handle `CarPropertySetError` callbacks for control state changes
5. Requires `android.car.permission.CONTROL_CAR_CLIMATE`

### Complexity: HIGH
This requires implementing a GMS-compatible Binder service, handling property writes
(not just reads), and managing control state synchronization.

---

## 4. Car Local Media (`com.google.android.gms.car.carlocalmedia/`)

Enables the HU to expose its own media sources (AM/FM radio, SiriusXM, USB media) to
the phone AA interface, so users can browse and control local media through AA.

### Data Model

```
CarLocalMediaPlaybackMetadata {
    String title;
    String artist;
    String album;
    Bitmap albumArt;
}

CarLocalMediaPlaybackRequest {
    int action;        // play, pause, skip, etc.
    String mediaId;    // media item identifier
}

CarLocalMediaPlaybackStatus {
    int state;         // playing, paused, stopped
    long position;     // playback position ms
    long duration;     // total duration ms
}
```

### How to Implement

1. Implement a `MediaBrowserService` on the HU that exposes local media sources
2. Register it with GMS car local media service
3. Forward playback control requests to the HU's native media system
4. Send metadata/status updates back to the phone

### Complexity: MEDIUM
Requires MediaBrowserService implementation and GMS registration, but the pattern
is standard Android media.

---

## 5. Destination Details with Charging (`com.google.android.apps.auto.sdk.nav.state/`)

Beyond the VEM sensor data, the AA SDK has a rich navigation state API for
EV routing feedback.

### Data Model (already documented in ev-energy-model-reverse-engineering.md)

```
VehicleEnergyForecast {
    EnergyAtDistance energyAtNextStop;        // {distanceMeters, arrivalBatteryEnergyWh, timeToArrivalSeconds}
    EnergyAtDistance distanceToEmpty;          // when battery reaches 0
    int forecastQuality;
    ChargingStationDetails nextChargingStop;  // {minDepartureEnergyWh, maxRatedPowerWatts, estimatedChargingTimeSeconds}
    List<StopDetails> stopDetails;
    List<DataAuthorization> dataAuthorizations;
}

DestinationDetails {
    String address;
    ChargingStationDetails chargingStationDetails;
}
```

### INavigationStateCallback (Binder transaction codes)

| Code | Method | Data |
|------|--------|------|
| 1 | `setNavigationSummary` | Route summary with turn-by-turn |
| 2 | `setTurnEvent` | Current turn instruction |
| 3 | `getClusterConfig` | Query cluster display config |
| **4** | **`setVehicleEnergyForecast`** | **EV battery estimates from Maps** |

### How to Receive

To receive `VehicleEnergyForecast` data back from Maps:
1. Set capability flag `ENHANCED_NAVIGATION_METADATA` (20) — tells Maps to compute
2. Implement `INavigationStateCallback` Binder service
3. Register with GMS navigation manager
4. Maps calls `setVehicleEnergyForecast()` with computed battery-on-arrival data

This would let us show OEM-quality battery-at-arrival on the HU's own UI
(not just in the projected Maps display).

---

## 6. Display Layout Configuration

### CarDisplayLayoutConfig / CarActivityLayoutConfig

Controls how the projected AA window is positioned and sized relative to the
HU's native UI elements.

```
CarDisplayLayoutConfig {
    Rect contentBounds;        // main content area
    Rect statusBarBounds;      // top status bar
    Rect navigationBarBounds;  // bottom nav bar
}

CarActivityLayoutConfig {
    Rect bounds;
    int gravity;
    boolean isFullscreen;
}
```

### CarDisplayBlendedUiConfig

For HUs that blend native UI elements over the projected display:
```
CarDisplayBlendedUiConfig — configures overlay regions where native HU UI
appears on top of the projected AA display
```

### CarDisplayCornerRadii

```
CarDisplayCornerRadii — corner radius values for rounded display corners
```

### Already Partially Implemented

We already set content insets and safe areas in the ServiceDiscoveryResponse.
These GMS-level APIs provide more fine-grained control for OEM-quality integration.

---

## 7. Projection Window Decorations

### ProjectionWindowDecorationParams

```
ProjectionWindowDecorationParams {
    DropShadowParams dropShadow;     // shadow behind projected window
    int cornerRadius;                 // rounded corners
    int backgroundColor;             // background behind window
}
```

### ProjectionWindowAnimationParams / Choreography

```
ProjectionWindowAnimationParams {
    int duration;                     // animation duration ms
    int interpolator;                 // animation curve
}

ProjectionWindowAnimationChoreography {
    List<AnimationParams> stages;     // multi-stage animation sequence
}
```

These control how the AA projection window animates when opening, closing, or resizing.

---

## 8. Undocumented Sensor Types (IMPLEMENTED)

See `docs/ev-energy-model-reverse-engineering.md` for full details.

| ID | Name | Status |
|----|------|--------|
| 23 | `SENSOR_VEHICLE_ENERGY_MODEL` | **IMPLEMENTED, WORKING** |
| 24 | `SENSOR_TRAILER_DATA` | Defined in proto, not implemented |
| 25 | `SENSOR_RAW_VEHICLE_ENERGY_MODEL` | Defined in proto, not implemented |
| 26 | `SENSOR_RAW_EV_TRIP_SETTINGS` | Defined in proto, not implemented |

---

## 9. Feature Flags (Server-Side Config)

The AA app uses Google's server-side feature flagging system. Interesting flags discovered:

| Flag | Default | Purpose |
|------|---------|---------|
| `EvOverride__ev_fuel_type_make_model_year_allowlist` | list(20) | OEM vehicle allowlist for EV features |
| `AudioBufferingFeature__default_minimum_audio_buffers_for_wifi` | 8 | Audio buffer count for WiFi AA |
| `AudioCodecPreferencesFeature__wifi_pcm_codec_exclusion_list` | list | Codecs excluded over WiFi |
| `BufferPoolsFeature__crash_on_buffer_leak_threshold_video` | 30 | Video buffer leak detection |
| `CradleFeature__max_parked_native_apps_forced_notifications` | 0 | Native app notifications while parked |
| `DpadAsTouchpadFeature__enable_dpad_as_touchpad` | false | D-pad emulates touchpad |
| `HeadUnitFeature__max_notifications_for_head_unit` | 5 | Max notification count |
| `ProjectedAppsFeature__enabled` | false | Third-party projected apps |
| `RhdFeature__rhd_default` | false | Right-hand drive default |
| `SynchronizedMediaFeature__audio_buffer_limit_ms` | 300 | A/V sync buffer limit |
| `SystemUi__thermal_battery_temperature_severe_threshold` | 420 | Phone thermal threshold (0.1°C) |

These are controlled by Google's servers and cannot be set from the HU side.
Listed here for reference when debugging AA behavior.

---

## Implementation Priority

### Phase 1 (Done)
- [x] Sensor types 23-26 in aasdk proto
- [x] VehicleEnergyModel protobuf schema
- [x] sendVehicleEnergyModel() C++ + JNI + Kotlin wiring
- [x] VHAL → VEM data flow
- [x] Verified: Maps shows battery-on-arrival %

### Phase 2 (Next)
- [ ] Receive `VehicleEnergyForecast` back from Maps (requires GMS Binder service)
- [ ] `DRIVER_POSITION_SETTING` capability flag
- [ ] `COOLWALK` capability flag investigation

### Phase 3 (Future)
- [ ] Car controls API (HVAC/climate in AA)
- [ ] Hero theming (OEM branding)
- [ ] Local media integration
- [ ] Multi-display support

### Phase 4 (Research)
- [ ] Capability flag injection mechanism (how to set the bitmask)
- [ ] GMS car service registration (needed for phase 2-3 features)
- [ ] Cielo rendering engine investigation

---

## Source File Reference

### HU Capabilities
- `p000/qmr.java` — Capability flag enum (53 flags)
- `p000/yhl.java` — Capability flag staleness tracking
- `p000/ihz.java` — UI element flag → bitmask mapping
- `p000/wys.java` — UI element enum

### Car Controls
- `com/google/android/gms/car/control/CarControl.java` — Control data model
- `com/google/android/gms/car/control/CarPropertyControl.java` — VHAL property binding
- `com/google/android/gms/car/control/CarControlGroup.java` — Control grouping
- `com/google/android/gms/car/control/CarPropertyConfig.java` — Property config (min/max/step)
- `com/google/android/gms/car/control/CarPropertyValue.java` — Property value
- `com/google/android/gms/car/control/CarAction.java` — Control actions

### Display/Window
- `com/google/android/gms/car/display/CarDisplayUiFeatures.java` — UI element bitmask
- `com/google/android/gms/car/display/CarDisplayBlendedUiConfig.java` — Blended UI overlay
- `com/google/android/gms/car/CarDisplayCornerRadii.java` — Corner radius
- `com/google/android/gms/car/CarDisplayLayoutConfig.java` — Display layout
- `com/google/android/gms/car/ProjectionWindowDecorationParams.java` — Window decorations
- `com/google/android/gms/car/ProjectionWindowAnimationParams.java` — Window animations

### Navigation
- `com/google/android/gms/car/navigation/VehicleEnergyForecast.java` — EV routing forecast
- `com/google/android/gms/car/navigation/EnergyAtDistance.java` — Battery at distance
- `com/google/android/gms/car/navigation/ChargingStationDetails.java` — Charging stop info
- `com/google/android/gms/car/navigation/StopDetails.java` — Per-stop details
- `com/google/android/gms/car/navigation/DestinationDetails.java` — Destination with charging
- `com/google/android/gms/car/navigation/NavigationState.java` — Full nav state
- `com/google/android/gms/car/navigation/NavigationCurrentPosition.java` — Current position
- `p000/lnj.java` — GhNavDataManager (energy forecast sender)
- `p000/lnl.java` — INavigationStateCallback implementation
- `p000/oib.java` — INavigationStateCallback Binder interface (transaction codes 1-4)

### Local Media
- `com/google/android/gms/car/carlocalmedia/` — Local media playback API
- `com/google/android/gms/car/CarLocalMediaPlaybackMetadata.java`
- `com/google/android/gms/car/CarLocalMediaPlaybackRequest.java`
- `com/google/android/gms/car/CarLocalMediaPlaybackStatus.java`

### Sensors (Implemented)
- `p000/wyb.java` — Sensor type enum (26 types)
- `p000/irc.java` — Proto field → sensor type mapping
- `p000/ioj.java` — Sensor data parser
- `p000/gdz.java` — CarSensorEvent builder
- `p000/wxu.java` — SensorBatch proto (26 repeated fields)

### Feature Flags
- `p000/abvs.java` through `p000/acff.java` — Server-side feature flag definitions
- `p000/pkc.java` — Client-side startup feature flags
