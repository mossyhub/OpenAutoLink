---
description: "Use when writing or modifying the C++ bridge relay binary. Covers the relay architecture, control channel signaling, socket splice loop, and phone management via bluetoothctl."
applyTo: "bridge/openautolink/relay/**"
---
# Bridge Relay C++ Conventions

## Architecture

The bridge relay (`openautolink-relay`) is a ~340-line C++ binary with **zero external dependencies**. It does NOT process AA protocol -- just raw TCP bytes between two sockets.

```
Phone:5277 <--raw bytes--> App:5291 (via poll() splice loop)
App:5288 <--JSON lines--> relay control channel
```

### Three TCP servers

| Port | Purpose |
|------|---------|
| 5288 | **Control** -- JSON lines signaling + diagnostics forwarding |
| 5291 | **Relay** -- accepts app's outbound connection, holds socket for splice |
| 5277 | **Phone** -- accepts phone's AA TCP connection after BT/WiFi pairing |

### Splice loop
- `poll()` on both relay and phone sockets with 100ms timeout
- Non-blocking `recv()`/`send()` with 64KB buffer
- On either socket close/error: notify app via control channel, clean up, wait for next connection
- Zero-copy where possible -- no parsing, no buffering, no AA protocol knowledge

## Control Channel Protocol

Newline-delimited JSON, one object per line.

### Relay -> App
- `{"type":"hello","name":"openautolink-relay","version":1,"relay_port":5291}`
- `{"type":"relay_ready"}` -- phone connected, sockets spliced, app can start aasdk
- `{"type":"relay_disconnected","reason":"..."}` -- phone TCP dropped
- `{"type":"phone_bt_connected","phone_name":"..."}` -- BT paired, WiFi pending
- `{"type":"paired_phones","phones":[{"mac":"...","name":"...","connected":true}]}`
- `{"type":"error","message":"..."}`

### App -> Relay
- `{"type":"hello","name":"...","version":1}`
- `{"type":"list_paired_phones"}`
- `{"type":"switch_phone","mac":"AA:BB:CC:DD:EE:FF"}`
- `{"type":"forget_phone","mac":"AA:BB:CC:DD:EE:FF"}`
- `{"type":"app_log",...}` / `{"type":"app_telemetry",...}` -- forwarded to stderr for journalctl

## Security

- MAC address fields are validated with regex `^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$` before passing to shell commands
- bluetoothctl commands use validated MAC only -- no shell injection possible
- No authentication on TCP (same trust model as before -- car network is isolated)

## Build

Single-file build, no dependencies beyond standard C++ library:
```bash
aarch64-linux-gnu-g++-14 -O2 -std=c++17 -o openautolink-relay src/main.cpp
```

## Thread model
- Single-threaded, event-driven via `poll()`
- Control channel and splice loop share the same thread
- Phone management (bluetoothctl) runs via `popen()` -- blocks briefly but acceptable for BT operations
