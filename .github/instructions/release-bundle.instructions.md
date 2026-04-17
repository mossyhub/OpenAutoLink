---
description: "Use when the user asks to bundle, build, sign, or release the AAB/APK. Covers the signing workflow, version management, and deployment scripts."
---
# Release Bundle Workflow

## When the User Says "Bundle the AAB" (or similar)

Run the bundle-release script. This handles signing, version increment, and AAB output.

### Script Location
```
scripts/bundle-release.ps1
```

### How to Run

```powershell
# From the repo root:
.\scripts\bundle-release.ps1
```

This script:
1. Reads version from `secrets/version.properties` (per-clone, gitignored)
2. Auto-increments `versionCode` and patches `versionName`
3. Uses saved DPAPI credentials from `secrets/signing-credentials.xml` if available
4. Falls back to interactive password prompts if no saved credentials
5. Builds a signed release AAB via Gradle `bundleRelease`
   - The AAB includes `liboal_jni.so` for arm64-v8a (full aasdk) and x86_64 (stub mode)
   - NDK build runs automatically via `externalNativeBuild` in `app/build.gradle.kts`

### Flags
- `-NoIncrement` — skip version bump (rebuild same version)
- `-KeystorePath <path>` — override keystore location (default: `secrets/upload-key.jks`)
- `-Alias <name>` — override key alias (default: `upload`)

### First-Time Setup
If the user hasn't saved credentials yet:
```powershell
.\scripts\save-signing-credentials.ps1
```
This saves keystore/key passwords encrypted with Windows DPAPI so future builds don't prompt.

### Output
The signed AAB lands at:
```
app/build/outputs/bundle/release/app-release.aab
```

### Related Scripts
- `scripts/save-signing-credentials.ps1` — save signing passwords (DPAPI, one-time)
- `scripts/create-upload-keystore.ps1` — create a new upload keystore
- `scripts/build-android.ps1` — lower-level build helper

### Common Phrases That Mean "Bundle the AAB"
- "bundle the aab"
- "build the release"
- "sign the release"
- "build aab"
- "make a release build"
- "package for play store"
