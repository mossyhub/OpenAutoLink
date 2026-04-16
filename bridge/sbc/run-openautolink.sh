#!/bin/bash
# run-openautolink.sh — Launch script for the relay binary.
# The relay is a thin TCP splice — aasdk runs inside the car app via NDK/JNI.
# No AA config flags needed — the app owns all AA settings.
set -u

echo "[run] OpenAutoLink Relay"
echo "[run] control:5288 relay:5291 phone:5277"

exec /opt/openautolink/bin/openautolink-relay
