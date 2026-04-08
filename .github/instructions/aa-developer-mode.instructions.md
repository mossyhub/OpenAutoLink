---
description: "Use when debugging AA protocol issues, testing sensor data, voice/mic, video resolution, or any phone-side AA behavior. Covers AA developer mode on the phone, the Desktop Head Unit (DHU) tool, and how they interact with our bridge."
---
# Android Auto Developer Mode & DHU Reference

## AA Developer Mode (on the Phone)

### How to Enable
1. Open **Settings → Apps → Android Auto → Additional settings in the app**
   - Alternative: open the Android Auto app directly
2. Scroll to **About** at the bottom, tap **Version**
3. Tap the **Version and permission info** section **10 times**
4. Tap **OK** on "Allow development settings?"
5. Developer options now appear in the **three-dot overflow menu** (top-right of AA app)

Only needs to be done once. To disable: overflow menu → "Quit developer mode."

### Developer Options Available
| Option | What It Does | When to Use |
|--------|-------------|------------|
| **Start head unit server** | Turns the phone into an AA server on port 5277 (for DHU testing) | Testing with Desktop Head Unit on PC |
| **Allow unknown sources** | Lets AA run sideloaded media/messaging apps (NOT needed for Car App Library apps) | Testing media apps not from Play Store |
| **Video resolution** | Unlock all resolution tiers the phone can encode (see below) | Testing 1440p/4K or validating HW decoder support |
| **Application mode** | Switch between developer/production behavior | General debugging |

### Video Resolution Override (Important for OpenAutoLink)
In AA developer settings on the phone, the **Video resolution** option lets you unlock ALL resolutions the phone's encoder supports. By default, the phone may restrict to standard tiers based on negotiation. With developer mode, you can force:

| Resolution | AA Tier | Our `aa_resolution` Setting | Pixel Dimensions |
|------------|---------|---------------------------|-----------------|
| 480p | 1 | `480p` | 800×480 |
| 720p | 2 | `720p` | 1280×720 |
| 1080p | 3 | `1080p` | 1920×1080 |
| 1440p | 4 | `1440p` | 2560×1440 |
| 4K | 5 | `4k` | 3840×2160 |

**How it interacts with our bridge**: The phone selects from resolutions offered in our ServiceDiscoveryResponse's VideoConfiguration list. With developer mode enabled, the phone will accept higher tiers even if it normally wouldn't. This is useful to test whether the GM Blazer EV's Snapdragon HW decoders handle 1440p/4K properly. Change the AA Resolution in our app Settings → Video tab to match.

### Relevance to OpenAutoLink
- **Not needed for normal operation** — our bridge connects to the phone via WiFi TCP, not through the DHU path
- **"Start head unit server"** could theoretically be used to connect our bridge directly to the phone via adb forward, bypassing BT/WiFi setup — useful for debugging
- **Video resolution override** — enables testing higher resolution tiers that the phone might not normally negotiate. Combined with our app's AA Resolution setting, lets you test 1440p/4K on real hardware

---

## Desktop Head Unit (DHU)

The DHU is a PC tool that emulates an AA head unit. It connects to the phone and provides a full AA session — useful for testing what the phone sends/receives without needing the real bridge+car.

### Installation
```bash
# Install via Android Studio → SDK Manager → SDK Tools → "Android Auto Desktop Head Unit Emulator"
# Binary location: $ANDROID_SDK/extras/google/auto/desktop-head-unit
```

### Running
```bash
# ADB tunneling mode (most common):
adb forward tcp:5277 tcp:5277
./desktop-head-unit

# USB AOA mode:
./desktop-head-unit --usb

# With custom config:
./desktop-head-unit -c config.ini
```

Prerequisites:
1. AA developer mode enabled on phone
2. "Start head unit server" activated from overflow menu
3. Phone connected via USB
4. `adb forward tcp:5277 tcp:5277` run first

### DHU Configuration (.ini file)

Default location: `~/.android/headunit.ini`

#### Video Settings
```ini
[general]
resolution = 1920x1080    # 800x480, 1280x720, or 1920x1080
dpi = 160
framerate = 30
marginwidth = 0
marginheight = 0
margins = 0,0,0,0         # top,bottom,left,right (overrides marginwidth/marginheight)
contentinsets = 0,0,0,0    # top,bottom,left,right
stablecontentinsets = 0,0,0,0
cropmargins = false
pixelaspectratio = 1.0
```

#### Input Settings
```ini
[general]
inputmode = touch          # touch, rotary, hybrid, or default
touch = true
touchpad = false
controller = false
```

#### Sensor Settings (MUST enable to send mock sensor data)
```ini
[sensors]
location = true
fuel = true
speed = true
compass = true
gyroscope = true
accelerometer = true
odometer = true
night_mode = true
driving_status = true
toll_card = true
gps_satellite = true
parking_brake = true
gear = true
```

#### EV Configuration
```ini
[general]
fueltypes = electric       # unleaded,leaded,diesel-1,diesel-2,biodiesel,e85,lpg,cng,lng,hydrogen,electric,other,unknown
evconnectors = combo-1     # j1772,mennekes,chademo,combo-1,combo-2,roadster,hpwc,gbt,supercharger,other,unknown
```

#### Other Settings
```ini
[general]
instrumentcluster = true   # Show instrument cluster window
playbackstatus = true      # Show media playback status window
driverposition = left      # left, center, or right
```

### DHU Console Commands

#### Sensor Commands (for testing what AA does with our data)
| Command | Description |
|---------|------------|
| `fuel [percentage]` | Set fuel/battery level (0-100). No arg = deactivate |
| `range [km]` | Set range in km. No arg = deactivate |
| `lowfuel {on\|off}` | Set low fuel warning |
| `speed [m/s]` | Set vehicle speed |
| `location lat long [accuracy] [altitude] [speed] [bearing]` | Set GPS location |
| `compass bearing [pitch] [roll]` | Set compass (degrees) |
| `accel [x] [y] [z]` | Set accelerometer (m/s²) |
| `gyro [x] [y] [z]` | Set gyroscope (rad/s) |
| `odometer km [trip_km]` | Set odometer |
| `parking_brake {true\|false}` | Set parking brake |
| `gear value` | Set gear (0=neutral, 100=drive, 101=park, 102=reverse) |
| `gps_satellite num_in_use [az] [el] [prn] [snr] [used]` | Set GPS satellite data |
| `tollcard {insert\|remove}` | Insert/remove toll card |

#### Mic Commands (for debugging voice issues)
| Command | Description |
|---------|------------|
| `mic begin` | Activate mic (simulates steering wheel button), listen from PC mic |
| `mic play file.wav` | Activate mic and play a WAV file as input |
| `mic repeat` | Repeat last WAV file |
| `mic reject {on\|off}` | Reject all mic requests (test error handling) |

Pre-made voice WAV files in `$SDK/extras/google/auto/voice/`:
- `navhome.wav` — "Navigate to home"
- `navwork.wav` — "Navigate to work"
- `pause.wav` — "Pause music"
- `nextturn.wav` — "When is my next turn?"

#### Display Commands
| Command | Description |
|---------|------------|
| `day` / `night` / `daynight` | Switch day/night mode |
| `focus video {on\|off}` | Toggle video focus (simulates HU hiding AA) |
| `focus audio {on\|off}` | Toggle audio focus (simulates HU playing own audio) |
| `focus nav {on\|off}` | Toggle nav focus (simulates HU running own nav) |
| `restrict none` / `restrict all` | Disable/enable driving restrictions |

#### System Commands
| Command | Description |
|---------|------------|
| `screenshot file.png` | Save screenshot |
| `keycode home\|back\|search\|media\|navigation\|tel` | Send keycode |
| `quit` | Exit DHU |

---

## How DHU Relates to OpenAutoLink

### What DHU Tests That Our Bridge Also Does
The DHU is essentially a reference implementation of what our bridge does. Comparing behavior:

| Feature | DHU | Our Bridge |
|---------|-----|-----------|
| Service Discovery | Config .ini → SDR | headless_config → SDR in live_session.cpp |
| Fuel/EV type | `fueltypes = electric` in .ini | `FUEL_TYPE_ELECTRIC` in SDR builder |
| EV connectors | `evconnectors = combo-1` in .ini | `EV_CONNECTOR_TYPE_COMBO_1` in SDR |
| Sensor data | Console commands (`fuel 80`, `speed 30`) | Real VHAL data via app → bridge → aasdk |
| Video config | .ini resolution/dpi/margins | Config from app hello message |
| Touch input | Mouse clicks | Touch events from AAOS SurfaceView |
| Mic handling | `mic begin` / WAV playback | Silence pump + car AudioRecord forwarding |

### Debugging Strategy
1. **Reproduce issue on DHU first** — if the same problem happens on DHU, it's a phone-side issue, not our bridge
2. **Compare DHU sensor config with our SDR** — if DHU's `fuel` command makes Maps show battery % but our bridge doesn't, we're missing something in our SensorSourceService setup
3. **Use DHU mic commands** — `mic reject on` can test how the phone handles mic failures; `mic play` can test if voice works with known-good audio input
4. **Use DHU to test EV features** — set `fueltypes = electric` + `fuel 43` + `range 215` in DHU and see if Maps shows arrival battery estimates. If yes, our bridge should too

### Testing EV Battery Display on DHU
To check if AA Maps shows battery arrival estimates:
```ini
# ~/.android/headunit.ini
[general]
resolution = 1920x1080
dpi = 160
fueltypes = electric
evconnectors = combo-1

[sensors]
fuel = true
speed = true
location = true
```
Then in DHU console:
```
fuel 43
range 215
```
If Maps shows "X% on arrival" with this setup, our bridge should produce identical behavior since we send the same fuel/range/EV type data.
