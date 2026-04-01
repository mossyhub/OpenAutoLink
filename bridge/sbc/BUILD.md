# OpenAutoLink Bridge — Build & Deploy

## SBC Filesystem Layout

All bridge files live under `/opt/` — **never** in user home directories.

```
/opt/openautolink-src/          # Source tree (git checkout)
├── aasdk/                      # aasdk library source
├── headless/                   # Bridge headless binary source
│   ├── include/openautolink/   # Headers (.hpp)
│   └── src/                    # Implementation (.cpp)
├── build/                      # CMake build output directory
│   └── openautolink-headless   # Unstripped binary (~43MB, debug)
└── CMakeLists.txt              # Top-level CMake

/opt/openautolink/              # Runtime deployment
├── bin/
│   └── openautolink-headless   # Stripped binary (~2.3MB, production)
├── run-openautolink.sh
├── scripts/
└── setup-*.sh

/etc/openautolink.env           # Runtime configuration
```

## Build Prerequisites

```bash
sudo apt install cmake build-essential libboost-all-dev libusb-1.0-0-dev \
    libssl-dev libprotobuf-dev protobuf-compiler
```

## Building on the SBC

```bash
# From the build directory:
cd /opt/openautolink-src/build

# Full rebuild:
cmake --build . --target openautolink-headless -j$(nproc)

# IMPORTANT: If you edited a source file via scp, CMake may not detect
# the timestamp change. Touch the file first to force recompilation:
touch /opt/openautolink-src/headless/src/cpc_session.cpp
cmake --build . --target openautolink-headless -j$(nproc)
```

## Deploying from Windows

### 1. Push modified source file(s)

```powershell
# Source files go to /opt/openautolink-src/headless/src/
scp bridge\openautolink\headless\src\cpc_session.cpp `
    khadas@192.168.137.197:/opt/openautolink-src/headless/src/cpc_session.cpp
```

### 2. Build on SBC via SSH

```powershell
ssh khadas@192.168.137.197 "touch /opt/openautolink-src/headless/src/cpc_session.cpp && cd /opt/openautolink-src/build && cmake --build . --target openautolink-headless -j4 2>&1 | tail -5"
```

### 3. Stop service, strip+copy binary, restart

```powershell
ssh khadas@192.168.137.197 "echo khadas | sudo -S bash -c 'systemctl stop openautolink.service && sleep 1 && strip -o /opt/openautolink/bin/openautolink-headless /opt/openautolink-src/build/openautolink-headless && systemctl start openautolink.service'"
```

### All-in-one (push + build + deploy)

```powershell
# Push source
scp bridge\openautolink\headless\src\cpc_session.cpp `
    khadas@192.168.137.197:/opt/openautolink-src/headless/src/cpc_session.cpp

# Build + deploy
ssh khadas@192.168.137.197 "touch /opt/openautolink-src/headless/src/cpc_session.cpp && cd /opt/openautolink-src/build && cmake --build . --target openautolink-headless -j4 2>&1 | tail -3 && echo khadas | sudo -S bash -c 'systemctl stop openautolink.service && sleep 1 && strip -o /opt/openautolink/bin/openautolink-headless /opt/openautolink-src/build/openautolink-headless && systemctl start openautolink.service' 2>/dev/null && echo DEPLOYED"
```

## Service Management

```bash
# Check status
systemctl status openautolink.service

# View logs
journalctl -u openautolink.service -f          # Follow live
journalctl -u openautolink.service -n 50       # Last 50 lines

# Restart
sudo systemctl restart openautolink.service
```

## Source ↔ Repo File Mapping

| Repo path (Windows)                                     | SBC path                                                  |
|----------------------------------------------------------|-----------------------------------------------------------|
| `bridge/openautolink/headless/src/cpc_session.cpp`       | `/opt/openautolink-src/headless/src/cpc_session.cpp`      |
| `bridge/openautolink/headless/src/live_session.cpp`      | `/opt/openautolink-src/headless/src/live_session.cpp`     |
| `bridge/openautolink/headless/include/openautolink/*.hpp` | `/opt/openautolink-src/headless/include/openautolink/*.hpp`|
| `external/aasdk/`                                        | `/opt/openautolink-src/aasdk/`                            |

## Common Pitfalls

- **NEVER put source/build in user home** (`/home/khadas/`). Use `/opt/openautolink-src/`.
- **Touch files after scp** — CMake may not detect timestamp changes from Windows.
- **Must stop service before copying binary** — `Text file busy` error if binary is running.
- **Strip the binary** — unstripped is ~43MB, stripped is ~2.3MB.
- **Phone reconnection after restart** — takes 30-60s for BT+WiFi to re-establish.
