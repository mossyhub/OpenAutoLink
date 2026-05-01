# Car Hotspot WiFi Auto-Connect — Implementation Plan

## Problem

When the phone is connected to home WiFi and the car is nearby (driveway), the phone won't auto-switch to the car's WiFi hotspot. Android doesn't allow non-system apps to programmatically switch WiFi. This blocks Car Hotspot mode from being zero-touch.

## Solution

Use `WifiNetworkSpecifier` + `ConnectivityManager.requestNetwork()` to create an app-managed WiFi connection to the car's hotspot. This API:

- Shows a one-time system dialog on first connection to a new SSID (user taps once)
- Auto-connects silently for all subsequent connections (approval persists across app kills)
- If the SSID is already a saved/known network, connects silently even the first time
- Works on non-rooted Android 10+ (API 29+)
- Creates a `Network` object that sockets can be explicitly bound to

### Validated on OnePlus 13 / Android 16 (2026-05-01)

| Test | Result |
|------|--------|
| First connection to unknown SSID | System dialog with filtered WiFi list, one tap to approve |
| Second connection (same session) | Silent, zero prompts |
| After app force-stop + relaunch | Silent, approval persists |
| Already-known SSID | Silent from the very first attempt, no dialog ever |
| Disconnect | Phone returns to previous WiFi |
| OS blocks manual connect while specifier active | Yes — Android prevents manual connect to claimed SSID |
| After clearing app data | Resets approval; system dialog appears again |
| After phone reboot | **UNTESTED** — verify when convenient |

---

## Architecture

### Design Principle: Additive Only

`CarWifiManager` runs **in parallel** with `TcpAdvertiser`, not as a prerequisite.
`TcpAdvertiser` starts immediately on `0.0.0.0` (all interfaces) — **no changes to TcpAdvertiser**.
This preserves the existing fast path when the phone has no WiFi or is already on the car WiFi.
`CarWifiManager` simply ensures the phone gets on the car WiFi in the background.

```
CompanionService starts:
  ├─ Start TcpAdvertiser on 0.0.0.0 IMMEDIATELY (existing behavior, zero changes)
  └─ Start CarWifiManager in parallel (new, additive)
       └─ Ensures phone gets on car WiFi even if home WiFi is active
```

### New Component: `CarWifiManager`

**File**: `companion/src/main/java/com/openautolink/companion/wifi/CarWifiManager.kt`

Single-purpose class that manages the `WifiNetworkSpecifier` lifecycle.

```
CarWifiManager
  ├── start(entries: List<CarWifiEntry>)  → begins retry loop
  ├── stop()                              → releases network, stops retries
  └── state: StateFlow<CarWifiState>
       ├── Idle
       ├── Scanning(attempt, maxAttempts)
       ├── Connected(ssid)
       └── Failed(reason)
```

**Retry strategy:**
- Each `requestNetwork()` call scans for ~30 seconds before calling `onUnavailable()`
- On `onUnavailable()`: wait 5 seconds, retry
- Max 12 attempts → ~7 minutes total coverage
- On `onLost()`: retry after 2-second debounce (car turned off and back on)
- Stop retrying when: `stop()` called, max retries exceeded

### NOT Modified: `TcpAdvertiser`

`TcpAdvertiser` listens on `0.0.0.0:5277` — all interfaces. When the phone joins car WiFi
(naturally or via specifier), port 5277 is reachable on the car WiFi interface. The car
initiates the TCP connection, so it arrives on the correct interface. No rebinding needed.

### Modified: `CompanionService`

**File**: `companion/src/main/java/com/openautolink/companion/service/CompanionService.kt`

**Changes:**
1. In `startNearby()`, after starting `TcpAdvertiser` (unchanged), also start `CarWifiManager`
2. Only start `CarWifiManager` when car WiFi entries are configured (`CAR_WIFI_SSIDS` non-empty)
3. Update notification text to reflect WiFi specifier state alongside TCP state
4. Stop `CarWifiManager` in `onDestroy()`

### Modified: `AutoStartReceiver`

**File**: `companion/src/main/java/com/openautolink/companion/autostart/AutoStartReceiver.kt`

**Changes:**
1. Currently only triggers when `AUTO_START_MODE == AUTO_START_BT`
2. Add: also trigger when `AUTO_START_MODE == AUTO_START_BT_AND_WIFI` (new mode, see below)
3. No other logic changes — just start the service, `CarWifiManager` handles WiFi

### Modified: `WifiJobService`

**File**: `companion/src/main/java/com/openautolink/companion/autostart/WifiJobService.kt`

**Changes:**
1. Currently only triggers when `AUTO_START_MODE == AUTO_START_WIFI`
2. Add: also trigger when `AUTO_START_MODE == AUTO_START_BT_AND_WIFI` (new mode)
3. For `AUTO_START_BT_AND_WIFI` mode, the WiFi scan uses the **car hotspot SSIDs** from the new pref (not the existing `AUTO_START_WIFI_SSIDS` which is for "phone is already on this WiFi")
4. This handles the case where the car's TCU boots WiFi before AAOS pairs BT

### Modified: `CompanionPrefs`

**File**: `companion/src/main/java/com/openautolink/companion/MainActivity.kt` (CompanionPrefs object)

**New constants:**
```kotlin
// Auto-start mode: both BT + WiFi scan (for Car Hotspot mode)
const val AUTO_START_BT_AND_WIFI = 4

// Car Hotspot WiFi credentials (for WifiNetworkSpecifier)
const val CAR_WIFI_SSIDS = "car_wifi_ssids"          // Map<String, String> serialized as Set<String> "ssid\tpassword"
```

**Multi-car storage format:**
```
Set<String> where each entry is "SSID\tPASSWORD"
```
Simple, no JSON dependency, easy to parse. Example:
```
setOf("OAL-Blazer\tMyPass123", "OAL-Tahoe\tOtherPass456")
```

### Modified: `MainScreen`

**File**: `companion/src/main/java/com/openautolink/companion/ui/MainScreen.kt`

**Changes to settings UI:**

1. **Connection Mode section** — when "Car Hotspot" is selected, show:
   - "Car WiFi Networks" section with add/remove list
   - Each entry: SSID + Password fields
   - "Add Car WiFi" button → dialog with SSID + Password inputs

2. **Auto-Start Mode section** — when connection mode is "Car Hotspot":
   - Show new option: "Bluetooth + WiFi Scan" (recommended for Car Hotspot)
   - When selected, show both BT device picker AND note that WiFi scan runs automatically using the configured Car WiFi SSIDs
   - Grey out / hide plain "WiFi" auto-start (doesn't make sense for Car Hotspot since the phone isn't on the car WiFi yet)

3. **Status card updates:**
   - Show car WiFi connection state: "Scanning for OAL-Blazer..." / "Connected to OAL-Blazer"
   - Show retry attempt count during scanning

---

## End-to-End Flow

### Happy Path: BT connects first (most common)

```
1. User gets in car, ignition ON
2. Phone BT auto-pairs with car head unit (2-5 seconds)
3. AutoStartReceiver fires → starts CompanionService
4. CompanionService.startNearby():
   a. Reads CONNECTION_MODE = car_hotspot
   b. Reads CAR_WIFI_SSIDS → finds "OAL-Blazer" + password
   c. Creates CarWifiManager("OAL-Blazer", password)
   d. CarWifiManager.start() → requestNetwork()
5. Car hotspot not yet up → requestNetwork scanning...
6. Car TCU boots, hotspot comes up (10-30 seconds)
7. CarWifiManager.onAvailable(network) fires
8. CompanionService creates TcpAdvertiser(network)
   a. Main TCP on 5277 (bound to car network)
   b. Identity probe on 5278 (bound to car network)
   c. UDP discovery on 5279 (bound to car network)
   d. mDNS registered
9. Car app wakes from suspend → starts mDNS/UDP scan
10. Car finds phone → TCP connect to 5277
11. AA session starts
```

### Alt Path: Car WiFi up first (TCU boots before AAOS BT)

```
1. User gets in car, ignition ON
2. Car TCU boots fast → car hotspot broadcasting (5-10 seconds)
3. WifiJobService periodic scan detects "OAL-Blazer" in range
4. WifiJobService → starts CompanionService
5. CompanionService.startNearby():
   a. Creates CarWifiManager → requestNetwork()
   b. Car hotspot is already up → onAvailable immediately
   c. TcpAdvertiser starts on car network
6. Car AAOS boots, BT pairs (15-30 seconds)
   a. AutoStartReceiver fires → service already running, no-op
7. Car app wakes → finds phone → AA session
```

### Reconnection: Car off → Car on

```
1. Car turns off → hotspot dies
2. CarWifiManager.onLost() fires
3. CompanionService tears down TcpAdvertiser
4. CarWifiManager re-enters retry loop
5. Car turns back on → hotspot comes up
6. CarWifiManager.onAvailable() → new Network
7. CompanionService recreates TcpAdvertiser bound to new Network
8. Car app wakes → finds phone → AA session resumes
```

### Edge: Phone rebooted while in car

```
1. Phone reboots
2. BT auto-pairs on boot
3. AutoStartReceiver (BOOT_COMPLETED is already registered) fires
4. Normal happy path from step 4 onwards
```

### Multi-car

```
1. CarWifiManager receives list of SSIDs + passwords
2. On start: scan available networks via WifiManager.startScan()
3. Match against stored SSIDs
4. requestNetwork() for the matched SSID
5. If no match → retry scan every 35 seconds (piggyback on requestNetwork timeout)
```

---

## New Preferences Schema

```
CAR_WIFI_SSIDS: Set<String>
  Format: "ssid\tpassword" per entry
  Example: {"OAL-Blazer\tMyPass123", "OAL-Tahoe\tOtherPass456"}
  Used by: CarWifiManager (for WifiNetworkSpecifier)
           WifiJobService (for WiFi scan trigger in BT+WiFi mode)

AUTO_START_BT_AND_WIFI: Int = 4
  New auto-start mode value
  Triggers: AutoStartReceiver (BT ACL) + WifiJobService (WiFi scan)
  Recommended for: Car Hotspot connection mode
```

---

## Files to Create

| File | Purpose |
|------|---------|
| `companion/src/main/java/com/openautolink/companion/wifi/CarWifiManager.kt` | WifiNetworkSpecifier retry manager |

## Files to Modify

| File | Change |
|------|--------|
| `companion/.../MainActivity.kt` (CompanionPrefs) | Add `AUTO_START_BT_AND_WIFI`, `CAR_WIFI_SSIDS` constants |
| `companion/.../service/CompanionService.kt` | Start CarWifiManager alongside (not instead of) TcpAdvertiser |
| `companion/.../autostart/AutoStartReceiver.kt` | Also trigger on `AUTO_START_BT_AND_WIFI` |
| `companion/.../autostart/WifiJobService.kt` | Also trigger on `AUTO_START_BT_AND_WIFI` with car SSIDs |
| `companion/.../ui/MainScreen.kt` | Car WiFi SSID/password settings, new auto-start option |
| `companion/src/main/AndroidManifest.xml` | Verify `CHANGE_NETWORK_STATE` present (already is) |

## Files NOT Modified

| File | Why |
|------|-----|
| `TcpAdvertiser.kt` | Listens on 0.0.0.0 — already reachable on any interface. No changes needed. |
| `AaProxy.kt` | Localhost bridge, doesn't touch WiFi layer |
| `NearbyAdvertiser.kt` | Disabled, unrelated |
| `NearbySocket.kt` | Disabled, unrelated |
| Any car-side (`app/`) code | Car app just scans and connects — no changes needed |

---

## Testing Plan

### Unit Tests
1. `CarWifiManager` retry logic with mock `ConnectivityManager`
2. `CompanionPrefs` parsing of `CAR_WIFI_SSIDS` format
3. `TcpAdvertiser` with mock `Network.socketFactory`

### Manual Tests (OnePlus 13 → Blazer EV)
1. **Cold start**: Phone on home WiFi, car off → start car → verify auto-connect flow
2. **Hot reconnect**: Car off → car on → verify `onLost` → retry → `onAvailable`
3. **BT first**: Verify service starts on BT, WiFi specifier retries until hotspot up
4. **WiFi first**: Verify WifiJobService detects car SSID in scan results, starts service
5. **Dual-STA**: Check if home WiFi stays connected while car WiFi is active (informational)
6. **Socket binding**: Verify car app can reach phone's TCP port 5277 through bound network
7. **Identity probe**: Verify car can reach port 5278 and get `OAL!` response
8. **mDNS**: Verify car discovers phone via `_openautolink._tcp` on car network
9. **Multi-car**: Configure two SSIDs, verify correct one is selected when in range
10. **Reboot persist**: Reboot phone, verify auto-connect still works without dialog

---

## Risk & Fallback

| Risk | Mitigation |
|------|------------|
| `WifiNetworkSpecifier` approval doesn't persist after reboot | Companion detects dialog shown → notification "Tap to connect to car WiFi" |
| Some OEMs don't support dual-STA | Fine — specifier replaces primary WiFi, TcpAdvertiser still reachable on 0.0.0.0 |
| `requestNetwork()` 30s timeout too short for slow car boots | 12 retries × 35s = 7 min coverage |
| Car WiFi password changes | User updates in companion settings, approval resets, one-time dialog |
| `onLost` spam during WiFi flaps | Debounce: 2-second delay before retry to avoid thrashing |
| Phone already on car WiFi (no home WiFi) | CarWifiManager runs harmlessly — specifier finds SSID immediately or is redundant. TcpAdvertiser already listening. Zero impact on existing fast path. |

---

## Open Questions (resolve during implementation)

1. **WiFi scan in background**: Does `WifiManager.startScan()` work from `WifiJobService` on Android 16 with throttling? May need to use scan results passively via `getScanResults()`.
2. **mDNS network registration**: When phone joins car WiFi via specifier, does NsdManager auto-register on the new interface, or do we need to re-register?
