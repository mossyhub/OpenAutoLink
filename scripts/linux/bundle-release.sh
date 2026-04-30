#!/usr/bin/env bash
# Linux equivalent of scripts/bundle-release.ps1
# Builds a signed release AAB with version auto-increment.
#
# Credentials: pass via env vars (no Linux equivalent of Windows DPAPI here).
#   export OAL_KEYSTORE_PASS='...'
#   export OAL_KEY_PASS='...'   # optional; defaults to OAL_KEYSTORE_PASS
#   export OAL_KEY_ALIAS='upload'         # optional
#   export OAL_KEYSTORE_PATH='secrets/upload-key.jks'  # optional
#
# Usage:
#   scripts/linux/bundle-release.sh
#   scripts/linux/bundle-release.sh --no-increment
set -euo pipefail

NO_INCREMENT=0
for arg in "$@"; do
    case "$arg" in
        --no-increment) NO_INCREMENT=1 ;;
        *) echo "Unknown arg: $arg" >&2; exit 2 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
VERSION_FILE="$REPO_ROOT/secrets/version.properties"
KEYSTORE_PATH="${OAL_KEYSTORE_PATH:-$REPO_ROOT/secrets/upload-key.jks}"
KEY_ALIAS="${OAL_KEY_ALIAS:-upload}"

if [ -z "${OAL_KEYSTORE_PASS:-}" ]; then
    echo "ERROR: OAL_KEYSTORE_PASS not set." >&2
    echo "  export OAL_KEYSTORE_PASS='...'  # keystore password" >&2
    echo "  export OAL_KEY_PASS='...'       # key password (optional, defaults to keystore pass)" >&2
    exit 1
fi
KEY_PASS="${OAL_KEY_PASS:-$OAL_KEYSTORE_PASS}"

if [ ! -f "$KEYSTORE_PATH" ]; then
    echo "ERROR: Keystore not found: $KEYSTORE_PATH" >&2
    exit 1
fi

# --- Version management (mirror of bundle-release.ps1) ---
mkdir -p "$(dirname "$VERSION_FILE")"
VERSION_CODE="$(grep -E '^\s*versionCode\s*=' "$VERSION_FILE" 2>/dev/null | tail -1 | sed -E 's/^[^=]*=\s*//; s/\s*$//')"
VERSION_NAME="$(grep -E '^\s*versionName\s*=' "$VERSION_FILE" 2>/dev/null | tail -1 | sed -E 's/^[^=]*=\s*//; s/\s*$//')"
VERSION_CODE="${VERSION_CODE:-1}"
VERSION_NAME="${VERSION_NAME:-0.1.0}"

if [ "$NO_INCREMENT" -eq 0 ]; then
    VERSION_CODE=$((VERSION_CODE + 1))
    # Bump patch (third dotted component)
    IFS='.' read -r MAJOR MINOR PATCH <<< "$VERSION_NAME"
    PATCH=$((PATCH + 1))
    VERSION_NAME="${MAJOR}.${MINOR}.${PATCH}"
    cat > "$VERSION_FILE" <<EOF
# Auto-managed by bundle-release.sh - do not edit during builds
# This file is per-clone (gitignored via secrets/) so each contributor
# tracks their own version independently.
versionCode=${VERSION_CODE}
versionName=${VERSION_NAME}
EOF
    echo "[bundle] Version incremented: versionCode=${VERSION_CODE}, versionName=${VERSION_NAME}"
else
    echo "[bundle] Version (no increment): versionCode=${VERSION_CODE}, versionName=${VERSION_NAME}"
fi

# Detect JAVA_HOME if not set
if [ -z "${JAVA_HOME:-}" ]; then
    for candidate in /usr/lib/jvm/java-21-openjdk /usr/lib/jvm/java-21-openjdk-amd64 \
                     /usr/lib/jvm/temurin-21-jdk /usr/lib/jvm/java-17-openjdk \
                     /usr/lib/jvm/java-17-openjdk-amd64 /usr/lib/jvm/temurin-17-jdk; do
        [ -d "$candidate" ] && export JAVA_HOME="$candidate" && break
    done
fi
if [ -z "${JAVA_HOME:-}" ]; then
    echo "ERROR: JAVA_HOME not set and no JDK 17/21 found." >&2
    exit 1
fi
echo "[bundle] JAVA_HOME=$JAVA_HOME"

cd "$REPO_ROOT"
exec ./gradlew :app:bundleRelease \
    "-PoalVersionCode=${VERSION_CODE}" \
    "-PoalVersionName=${VERSION_NAME}" \
    "-Pandroid.injected.signing.store.file=${KEYSTORE_PATH}" \
    "-Pandroid.injected.signing.store.password=${OAL_KEYSTORE_PASS}" \
    "-Pandroid.injected.signing.key.alias=${KEY_ALIAS}" \
    "-Pandroid.injected.signing.key.password=${KEY_PASS}"
