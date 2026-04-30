#!/usr/bin/env bash
# Linux equivalent of scripts/build-android.ps1
# Builds the AAOS app via Gradle. See scripts/linux/README.md.
set -euo pipefail

TASK="${1:-assembleDebug}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Detect JAVA_HOME if not set. Gradle 8.x Kotlin DSL fails on JDK 25 — must be 17 or 21.
if [ -z "${JAVA_HOME:-}" ]; then
    for candidate in \
        /usr/lib/jvm/java-21-openjdk \
        /usr/lib/jvm/java-21-openjdk-amd64 \
        /usr/lib/jvm/temurin-21-jdk \
        /usr/lib/jvm/java-17-openjdk \
        /usr/lib/jvm/java-17-openjdk-amd64 \
        /usr/lib/jvm/temurin-17-jdk; do
        if [ -d "$candidate" ]; then
            export JAVA_HOME="$candidate"
            break
        fi
    done
fi
if [ -z "${JAVA_HOME:-}" ]; then
    echo "ERROR: JAVA_HOME not set and no JDK 17/21 found in /usr/lib/jvm/." >&2
    echo "Install OpenJDK 21 (e.g. 'sudo pacman -S jdk21-openjdk' / 'apt install openjdk-21-jdk')." >&2
    exit 1
fi
echo "[build-android] JAVA_HOME=$JAVA_HOME"
echo "[build-android] Task: $TASK"

cd "$REPO_ROOT"
exec ./gradlew ":app:${TASK}" "${@:2}"
