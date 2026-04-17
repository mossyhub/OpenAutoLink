# EV Energy Model Reverse Engineering

## Status: WORKING

EV routing with battery-on-arrival estimates is fully functional via the AA protocol.
When connected to an EV with VHAL data, the app sends a VehicleEnergyModel protobuf
through undocumented sensor type 23. Google Maps on the phone processes it and shows
battery percentage remaining at each destination in search results and during navigation.

**Verified:** April 2026 on AAOS emulator with phone-side Google Maps showing battery
estimates (94%, 93%, 92%, etc.) that decrease correctly with destination distance.

### How It Works

1. Service discovery advertises sensor types 23, 25, 26 + FUEL_TYPE_ELECTRIC + EV connectors
2. Phone AA app requests sensor type 23 during session setup
3. App reads VHAL: `EV_BATTERY_LEVEL`, `INFO_EV_BATTERY_CAPACITY`, `RANGE_REMAINING`
4. Builds `VehicleEnergyModel` protobuf with battery capacity, current level, consumption rate
5. Sends as sensor type 23 in `SensorBatch` — phone passes through to GMS → Maps
6. Maps computes battery-on-arrival estimates using the energy model + route data

### Key Files

| File | Role |
|------|------|
| `external/opencardev-aasdk/protobuf/.../SensorType.proto` | Sensor type enum (23-26 added) |
| `external/opencardev-aasdk/protobuf/.../SensorBatch.proto` | SensorBatch with field 23-26 |
| `external/opencardev-aasdk/protobuf/.../VehicleEnergyModel.proto` | Reconstructed VEM proto |
| `app/src/main/cpp/aa_session.cpp` | `sendVehicleEnergyModel()` + service discovery |
| `app/src/main/cpp/jni_bridge.cpp` | JNI bridge for `sendVehicleEnergyModel` |
| `app/src/main/java/.../AasdkJni.kt` | Kotlin JNI declaration |
| `app/src/main/java/.../DirectAaTransport.kt` | VHAL → VEM send wiring (30s throttle) |
| `app/src/main/java/.../VehicleDataForwarderImpl.kt` | VHAL property reading (trackedIds fix) |

---

## Overview

Google Maps on AAOS and via Android Auto can show battery-on-arrival estimates for EV routing. This document captures the reverse-engineered protobuf schemas and data flow from decompiling both the AAOS Google Maps APK and the Android Auto phone APK.

## Data Flow

```
                    AA Protocol (sensor channel)
AAOS Head Unit  ───────────────────────────────────>  Phone AA App
  reads VHAL:                                           │
  - EV_BATTERY_LEVEL                                    │ pass-through
  - INFO_EV_BATTERY_CAPACITY                            │ (opaque bytes)
  - RANGE_REMAINING                                     ▼
  - EV_CHARGE_PORT_OPEN                              GMS Car Service
  - CarInfoManager                                      │
    (make/model/year/                                   │
     fuel types/connectors)                             ▼
                                                   Google Maps (phone)
                                                     sends back:
                                                   VehicleEnergyForecast
                                                     - energyAtNextStop
                                                     - distanceToEmpty
                                                     - chargingStops
```

## Undocumented AA Sensor Types

The AA protocol has sensor types beyond the publicly documented 1-22. Found in the decompiled AA APK (`wyb.java`):

| Sensor ID | Internal Name | Proto Wrapper | Purpose |
|-----------|--------------|---------------|---------|
| 23 | `SENSOR_VEHICLE_ENERGY_MODEL_DATA` | `wrc` (empty msg, pass-through) | Vehicle energy model protobuf |
| 24 | `SENSOR_TRAILER_DATA` | `wyq` | Trailer info |
| 25 | `SENSOR_RAW_VEHICLE_ENERGY_MODEL` | `wxn` (bytes field) | Same as 23, raw bytes shortcut |
| 26 | `SENSOR_RAW_EV_TRIP_SETTINGS` | `wxl` (bytes field) | EV trip configuration |

### Wire Format

Sensor 23 and 25 produce identical output. From `gdz.java`:
```java
case 23:
    if (aaxjVar instanceof wrc) {
        bytes = ((wrc) aaxjVar).serialize();     // unknown-field pass-through
    } else if (aaxjVar instanceof wxn) {
        bytes = ((wxn) aaxjVar).rawBytes;         // direct bytes
    }
    return new CarSensorEvent(23, timestamp, new float[0], bytes);
case 25:
    return new CarSensorEvent(25, timestamp, new float[0], ((wxl) aaxjVar).rawBytes);
```

### Phone-Side Processing

The AA app on the phone (`ioj.java`) deserializes sensor data, stores it, and forwards to GMS via `CarSensorEvent`. The energy model bytes are **opaque** — the AA app does not parse them, just passes through.

### Gating

From `lnj.java` (GhNavDataManager):
```
"Can't send vehicle energy forecast without enhanced nav metadata enabled."
```
The `enhanced nav metadata` feature flag must be enabled during session setup.

## Reconstructed Protobuf Schema

### VehicleEnergyModel (aeaf) — Sensor Type 23 Payload

This is the protobuf serialized into the bytes field of sensor type 23/25.

```protobuf
// Reconstructed from AAOS Google Maps decompile (aeaf and sub-messages)
// Field numbers and types decoded from protobuf-lite descriptor strings

message VehicleEnergyModel {
  BatteryConfig battery = 1;                    // aeah
  EnergyConsumption consumption = 2;            // aeaa
  // field 3 unused
  VehicleSpecs specs = 4;                       // adzv
  // field 5 unused
  repeated ChargingCurvePoint curves_ac = 6;    // adzh (AC charging curve)
  repeated ChargingCurvePoint curves_dc = 7;    // adzh (DC charging curve)
  EnergyEfficiency efficiency = 8;              // adze
  ThermalModel thermal = 9;                     // adzf
  ConnectorConfig connectors = 10;              // adzj
  WheelConfig wheels = 11;                      // adza
  ChargingPrefs charging_prefs = 12;            // adzw
  CalibrationData calibration = 13;             // aeae
}

message BatteryConfig {                         // aeah
  int64 config_id = 1;                          // identifier/version
  // field 2 unused
  EnergyValue min_usable_capacity = 3;          // e.g. {wh=64740}
  EnergyValue max_capacity = 4;                 // e.g. {wh=78000}
  EnergyDisplayUnit display_unit = 5;           // enum
  float charge_efficiency = 6;                  // 0.0-1.0
  float discharge_efficiency = 7;               // 0.0-1.0
  EnergyValue reserve_energy = 8;               // e.g. {wh=2154}
  int32 max_charge_power_w = 9;                 // e.g. 170000 (170kW)
  int32 max_discharge_power_w = 10;             // e.g. 155000 (155kW)
  bool regen_braking_capable = 11;
  float preconditioning_power_kw = 12;
  repeated BatterySegment segments = 13;        // aeag (SoC curve segments)
}

message EnergyConsumption {                     // aeaa
  EnergyRate driving = 1;                       // e.g. {rate=138.38} Wh/km
  EnergyRate auxiliary = 2;                     // e.g. {rate=2.18} Wh/km
  EnergyRate aerodynamic = 3;                   // e.g. {rate=0.3617} drag coeff
}

message EnergyValue {                           // adzs
  int32 watt_hours = 1;                         // energy in Wh
  float display_value = 2;                      // formatted for display
}

message EnergyRate {                            // adzr
  float rate = 1;                               // Wh/km or coefficient
  float uncertainty = 2;                        // confidence/error margin
}

message ChargingCurvePoint {                    // adzh
  float charge_rate_kw = 1;                     // power at this SoC
  EnergyValue soc_point_1 = 2;                  // SoC breakpoint
  EnergyValue soc_point_2 = 3;                  // SoC breakpoint
  EnergyValue soc_point_3 = 4;                  // SoC breakpoint
}

message ChargingPrefs {                         // adzw
  string provider_package = 1;                  // e.g. "com.google.android.apps.maps"
  bool user_has_modified = 2;
  ChargingMode mode = 3;                        // enum: 0=unknown, 1=standard
}

message ThermalModel {                          // adzf
  int32 thermal_zone = 1;
  EnergyValue thermal_capacity = 2;             // adzs
}

message ConnectorConfig {                       // adzj
  repeated ConnectorEntry entries = 7;          // adzu (starts at field 7)
  int32 default_connector = 8;
}

message VehicleSpecs {                          // adzv
  repeated FuelTypeEntry fuel_types = 1;        // adzy
  repeated ConnectorTypeEntry ev_connectors = 2;// adzz
}

message EnergyEfficiency {                      // adze
  repeated EfficiencyPoint normal = 4;          // adzc (starts at field 4)
  repeated EfficiencyPoint eco = 5;             // adzd
}

message WheelConfig {                           // adza
  repeated WheelEntry wheels = 4;               // adzt (starts at field 4)
}

message CalibrationData {                       // aeae
  repeated CalibrationPoint point_set_1 = 1;    // aead
  int32 calibration_version_1 = 2;
  repeated CalibrationPoint point_set_2 = 3;    // aeab
  int32 calibration_version_2 = 4;
  repeated CalibrationPoint point_set_3 = 5;    // aeac
}
```

### RawVehicleEnergyModel Wrapper (wxn) — Sensor Type 25

```protobuf
message RawVehicleEnergyModel {
  bytes energy_model_payload = 1;   // serialized VehicleEnergyModel
}
```

### RawEvTripSettings (wxl) — Sensor Type 26

```protobuf
message RawEvTripSettings {
  bytes trip_settings_payload = 1;  // serialized trip settings proto
}
```

## VehicleEnergyForecast — What Maps Sends Back

After receiving the energy model and computing routes, Maps sends `VehicleEnergyForecast` back through the `INavigationStateCallback` Binder interface (transaction code 4).

```
// From com.google.android.apps.auto.sdk.nav.state

VehicleEnergyForecast {
  EnergyAtDistance energyAtNextStop;     // {distanceMeters, arrivalBatteryEnergyWh, timeToArrivalSeconds}
  EnergyAtDistance distanceToEmpty;      // when battery reaches 0
  int forecastQuality;                  // confidence level
  ChargingStationDetails nextChargingStop; // {minDepartureEnergyWh, maxRatedPowerWatts, estimatedChargingTimeSeconds}
  List<StopDetails> stopDetails;        // per-stop {expectedArrivalEnergy, chargingInfo}
  List<DataAuthorization> dataAuthorizations; // consent tracking
}
```

## Known Values from AAOS Maps (gzd.java)

These were found hardcoded in the AAOS Maps decompile for demo/testing:

| Field | Value | Unit |
|-------|-------|------|
| min_usable_capacity | 64,740 | Wh |
| max_capacity | 78,000 | Wh |
| reserve_energy | 2,154 | Wh |
| max_charge_power | 170,000 | W (170 kW) |
| max_discharge_power | 155,000 | W (155 kW) |
| driving_consumption | 138.38 | Wh/km |
| auxiliary_consumption | 2.18 | Wh/km |
| aerodynamic_coefficient | 0.3617 | - |

## VHAL Properties We Already Read

Our app (`VehicleDataForwarderImpl.kt`) already subscribes to:

| VHAL Property | ID | Permission | Maps To |
|---------------|-----|-----------|---------|
| `EV_BATTERY_LEVEL` | 0x11600309 | CAR_ENERGY | Current battery Wh |
| `INFO_EV_BATTERY_CAPACITY` | 0x11600106 | CAR_INFO | Max capacity Wh |
| `RANGE_REMAINING` | 0x11600308 | CAR_ENERGY | Range in meters |
| `EV_BATTERY_INSTANTANEOUS_CHARGE_RATE` | 0x1160030C | CAR_ENERGY | Charge rate W |
| `EV_BATTERY_AVERAGE_TEMPERATURE` | 0x1160030E | CAR_ENERGY | Pack temp °C |
| `INFO_FUEL_TYPE` | (runtime) | CAR_INFO | Fuel type list |
| `INFO_EV_CONNECTOR_TYPE` | (runtime) | CAR_INFO | Connector type list |

## Implementation Plan

### Minimum Viable Approach

1. **Extend aasdk** to support sensor type 23 in `SensorType.proto` and `SensorSourceService`
2. **Build VehicleEnergyModel protobuf** from VHAL data:
   - `battery.max_capacity` = `INFO_EV_BATTERY_CAPACITY`
   - `battery.min_usable_capacity` = 95% of max (reasonable estimate)
   - `battery.reserve_energy` = 5% of max
   - `consumption.driving.rate` = derive from `EV_BATTERY_LEVEL / RANGE_REMAINING * 1000` (Wh/km)
3. **Send as sensor 23** in the SensorBatch during AA session
4. **Observe** whether Maps on the phone starts showing battery-on-arrival estimates

### Feature Negotiation

The phone's AA app gates this behind "enhanced nav metadata". This likely requires:
- Advertising sensor type 23 in the SensorSourceService configuration
- Possibly a specific head unit capability flag during AA handshake

### Fallback: Local Overlay

If the protocol approach is blocked by feature negotiation we can't satisfy, we already have all the data for a local overlay:
- Nav distance from AA (`NavigationCurrentPosition.destination_distances[0].distance.meters`)
- Live battery from VHAL (`EV_BATTERY_LEVEL`, `INFO_EV_BATTERY_CAPACITY`, `RANGE_REMAINING`)
- Simple linear estimate: `arrival% = (currentWh - distanceM * whPerM) / capacityWh * 100`

## Source Files Referenced

### AAOS Google Maps APK (Gmaps_teardown/java_src/)
- `p000/ghs.java` — EmbeddedCarInfo (reads CarPropertyManager)
- `p000/gzd.java` — VehicleEnergyModel builder (hardcoded demo values)
- `p000/aeaf.java` — VehicleEnergyModel protobuf
- `p000/aeah.java` — BatteryConfig protobuf
- `p000/aeaa.java` — EnergyConsumption protobuf
- `p000/adzs.java` — EnergyValue protobuf
- `p000/adzr.java` — EnergyRate protobuf
- `p000/adzw.java` — ChargingPrefs protobuf
- `p000/adzh.java` — ChargingCurvePoint protobuf
- `p000/fgn.java` — Powertrain detection (BEV/PHEV/ICE)

### Android Auto Phone APK (Gmaps_teardown/aa_apk_src/)
- `p000/wyb.java` — Sensor type enum (23-26 undocumented)
- `p000/irc.java` — Sensor type→proto class mapping
- `p000/ioj.java` — Sensor data parser (deserialize + forward)
- `p000/gdz.java` — CarSensorEvent builder (bytes extraction)
- `p000/wrc.java` — VEHICLE_ENERGY_MODEL_DATA proto (empty pass-through)
- `p000/wxn.java` — RAW_VEHICLE_ENERGY_MODEL proto (bytes wrapper)
- `p000/wxl.java` — RAW_EV_TRIP_SETTINGS proto (bytes wrapper)
- `p000/lnj.java` — GhNavDataManager (sends VehicleEnergyForecast)
- `p000/lnl.java` — INavigationStateCallback impl (setVehicleEnergyForecast)
- `com/google/android/apps/auto/sdk/nav/state/VehicleEnergyForecast.java`
- `com/google/android/apps/auto/sdk/nav/state/EnergyAtDistance.java`
- `com/google/android/apps/auto/sdk/nav/state/ChargingStationDetails.java`
- `com/google/android/apps/auto/sdk/nav/state/StopDetails.java`

### Protobuf-lite Type Encoding (for decoding descriptors)

Type IDs in info string (GROUP is skipped):
```
0=double, 1=float, 2=int64, 3=uint64, 4=int32,
5=fixed64, 6=fixed32, 7=bool, 8=string,
9=message, 10=bytes, 11=uint32, 12=enum
18+ = repeated versions (type + 18)
```

High byte of type_info char: `0x10` = has-bit presence word 1, `0x14/0x15` etc = different has-bit offsets. `0x08` flag in high byte = closed enum (needs default value in Objects[]).

## Implementation Details

### VEM Construction (aa_session.cpp)

The `sendVehicleEnergyModel(capacityWh, currentWh, rangeM)` method builds the protobuf:

```
battery.max_capacity.watt_hours = capacityWh          // from INFO_EV_BATTERY_CAPACITY
battery.min_usable_capacity.watt_hours = capacityWh * 0.95
battery.reserve_energy.watt_hours = capacityWh * 0.05
battery.regen_braking_capable = true
consumption.driving.rate = (currentWh / rangeM) * 1000  // Wh/km from car's own range estimate
consumption.auxiliary.rate = 2.0                         // typical aux consumption
charging_prefs.mode = 1                                  // standard
```

The energy consumption rate is derived from the car's own range estimate rather than hardcoded,
so it automatically reflects the car's driving conditions, temperature, and driving style.

### Throttling

VEM is sent at most once every 30 seconds via `DirectAaTransport.kt`. The energy model
doesn't change rapidly — battery % and range update slowly during driving.

### Permission Requirements

On the real car (GM AAOS), `android.car.permission.CAR_ENERGY` is pre-granted to system apps.
On the AAOS emulator, this permission must be manually granted via `pm grant` (signature-level).

### Bug Fix: trackedPropertyIds ordering

`VehicleDataForwarderImpl.kt` had a bug where initial VHAL property reads were silently
dropped because `trackedPropertyIds.add(propId)` happened AFTER the initial `handleChangeEvent()`
call, which checks `if (propertyId !in trackedPropertyIds) return`. This was invisible for
subscribable properties (callbacks re-populate values) but caused STATIC properties like
`INFO_EV_BATTERY_CAPACITY` to never appear in `currentValues`. Fixed by moving
`trackedPropertyIds.add(propId)` before the initial read.

### Future Work

- Add charging curve data from VHAL (if exposed by HAL) for more accurate charging stop estimates
- Investigate sensor type 26 (EV_TRIP_SETTINGS) for trip-specific battery targets
- Periodic VEM updates during driving to reflect changing consumption patterns
- Consider sending fuel data (sensor 6) in parallel for hybrid vehicles
