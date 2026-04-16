---
description: "Use when building, deploying, or debugging the C++ bridge relay binary. Covers WSL cross-compilation, SBC deployment via SCP, and iterative dev workflow."
---
# Bridge Dev Workflow

## Overview

The bridge relay binary (`openautolink-relay`) is a ~340-line C++ file with zero external dependencies. Build is trivial -- single cmake target, no submodules, no aasdk.

## Build (WSL cross-compile)

```powershell
# From Windows -- builds in WSL, deploys to SBC via SCP
scripts\deploy-bridge.ps1           # Incremental build + deploy
scripts\deploy-bridge.ps1 -Clean    # Clean rebuild + deploy
```

### Manual WSL build
```bash
# In WSL:
cd /mnt/d/personal/openautolink
mkdir -p build-relay-arm64 && cd build-relay-arm64
cmake ../bridge/openautolink/relay \
    -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc-14 \
    -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++-14
make -j$(nproc)
aarch64-linux-gnu-strip -o openautolink-relay-stripped openautolink-relay
```

Output: `build-relay-arm64/openautolink-relay-stripped` (~67KB)

### WSL prerequisites
Only two packages needed:
```bash
sudo apt install g++-14-aarch64-linux-gnu cmake
```

## Deploy to SBC

```powershell
# deploy-bridge.ps1 does this automatically, but manual:
scp build-relay-arm64/openautolink-relay-stripped openautolink:/usr/local/bin/openautolink-relay
ssh openautolink "sudo systemctl restart openautolink"
```

## Iterative dev cycle

1. Edit `bridge/openautolink/relay/src/main.cpp`
2. Run `scripts\deploy-bridge.ps1` (builds + deploys in ~5 seconds)
3. Check logs: `ssh openautolink "journalctl -u openautolink -f"`
4. App auto-reconnects when bridge restarts

## Debugging

```bash
# On SBC -- check relay status
systemctl status openautolink
journalctl -u openautolink -f --no-pager

# Check if relay is listening
ss -tlnp | grep -E '5277|5288|5291'
```

## CI

`.github/workflows/release-bridge.yml` builds the relay on push to `main`. Only needs `g++-14-aarch64-linux-gnu` and `cmake` -- no submodules, no heavy deps.
