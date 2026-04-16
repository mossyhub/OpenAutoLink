# OpenAutoLink Bridge Components

## relay/

The `openautolink-relay` binary -- a thin TCP socket splicer. Accepts the car app's
outbound connection on port 5291, the phone's AA connection on port 5277, and splices
them together. Control channel on port 5288 handles signaling and diagnostics.

Zero external dependencies. ~340 lines of C++. 67KB stripped.

## scripts/

`aa_bt_all.py` -- BLE advertising, BT pairing (AA profiles), HSP audio gateway,
RFCOMM WiFi credential exchange. Runs as `openautolink-bt.service`.
