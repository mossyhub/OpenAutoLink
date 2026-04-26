#!/bin/bash
# Monitor the aasdk build until it succeeds or fails.
# Usage: bash scripts/monitor-aasdk-build.sh

WORK="/tmp/oal-aasdk-android-build"
NTFS_OUTPUT="/mnt/d/personal/openautolink/app/src/main/cpp/third_party/aasdk/arm64-v8a/lib/libaasdk.a"

while true; do
    PROCS=$(ps aux | grep -E 'cmake|make|clang|g\+\+' | grep -v grep | wc -l)
    HOST_A=$(find "$WORK/host-build" -name '*.a' 2>/dev/null | wc -l)
    HOST_AASDK=$(find "$WORK/host-build" -name 'libaasdk.a' 2>/dev/null)
    CROSS_A=$(find "$WORK/build" -name '*.a' 2>/dev/null | wc -l)
    CROSS_AASDK=$(find "$WORK/build" -name 'libaasdk.a' 2>/dev/null)

    echo "[$(date +%H:%M:%S)] procs=$PROCS host_libs=$HOST_A cross_libs=$CROSS_A host_aasdk=${HOST_AASDK:-(none)} cross_aasdk=${CROSS_AASDK:-(none)}"

    if [ -f "$NTFS_OUTPUT" ]; then
        echo "=== SUCCESS: libaasdk.a on NTFS ==="
        ls -la "$(dirname "$NTFS_OUTPUT")/"
        exit 0
    fi

    if [ "$PROCS" -eq 0 ]; then
        if [ "$HOST_A" -eq 0 ] && [ "$CROSS_A" -eq 0 ]; then
            echo "=== WAITING: build not started yet or still staging ==="
        else
            echo "=== BUILD ENDED (procs=0, host=$HOST_A, cross=$CROSS_A) ==="
            echo "Check build terminal for errors."
            exit 1
        fi
    fi

    sleep 10
done
