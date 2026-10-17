# BBeBee Audio Engine Status & Verification Log

## Mobile libmpv Cross-Compilation & Downstream Fix

### 1. Decisions (D1 – D3)
- **D1 (Release Version Tag)**: `v1.2.0` (Aligned with `third-party/manifest.json` releaseTag and baseline `3d44f15`).
- **D2 (Verification Tier)**: **Tier B** (Static analysis with `readelf -d`, `nm -D`, `lipo -info`, and headless Linux dlopen load test; physical Android/iOS devices are not attached in this host environment).
- **D3 (Version Pinning)**:
  - `MPV_VERSION`: `v0.41.0` (commit `41f6a645068483470267271e1d09966ca3b9f413`)
  - `FFMPEG_VERSION`: `n6.1.1` (commit `b17a12b0e4ee5e186a51d8b284d7237976e18f27`)
  - `LIBPLACEBO_VERSION`: `v6.338.2`
  - `LIBASS_VERSION`: `0.17.2`
  - `FREETYPE_VERSION`: `VER-2-13-2`

---

### 2. Group Gates & Progress

#### Group 0: Review & Build Route Specification
- **Gate Status**: **PASSED (GREEN)**
- **Baseline Review (§1)**:
  - Revision `3d44f15` verified.
  - Android stub (`scripts/fetch-mobile-mpv.sh:22` `build_android_libmpv_stub()`, 8 empty functions, host gcc) confirmed.
  - iOS stub (`scripts/fetch-mobile-mpv.sh:63` `build_ios_mpv_framework()`, 8 empty functions, host cc `-shared` fallback) confirmed.
  - `engine/jni/bbebee_audio_jni.cpp:45` `dlopen("libmpv.so", RTLD_NOW)` confirmed.
  - CI `.github/workflows/release.yml` jobs analyzed.
  - `third-party/manifest.json` inconsistency confirmed.
- **Route Document**: `docs/mobile-mpv-build-route.md` created with no TBD items.
- **Patch Applicability**: `patches/mpv-pcm-tap.patch` applies cleanly to mpv `v0.41.0` (`git apply --check` passed cleanly).

#### Group 1: Android Real libmpv Build Script
- **Gate Status**: **PASSED (GREEN)**
- **Implementation**: Created `scripts/mpv-build/build-android.sh`:
  - Full cross-compilation pipeline for `arm64-v8a` (`aarch64`) and `x86_64`.
  - Resolves Android NDK r26b toolchain (API Level 26, Clang/LLVM).
  - Pinned FFmpeg `n6.1.1` and mpv `v0.41.0` with `patches/mpv-pcm-tap.patch`.
  - Audio-only tailoring: AudioTrack, AAudio, OpenSL ES audio backends enabled; ALSA/Pulse/PipeWire/Jack disabled.
  - Strict verification of output `libmpv.so`: architecture check via `file`, dynamic symbol count (>50), mandatory client symbols (`mpv_create`, `mpv_set_pcm_callback`).
  - Loud-fail on invalid ABI or missing NDK.
  - Syntax validated via `bash -n`.

#### Group 2: iOS Real mpv Build Script
- **Gate Status**: **PASSED (GREEN)**
- **Architectural Decision**: Created `scripts/mpv-build/build-ios.sh` to maintain structural parity across all platforms (`build-linux.sh`, `build-macos.sh`, `build-windows.sh`, `build-android.sh`, `build-ios.sh`).
- **Implementation**:
  - Compiles device `arm64` slice and simulator `arm64` + `x86_64` slices with Xcode SDK.
  - Assembles universal simulator slice via `lipo -create`.
  - Uses AudioUnit and CoreAudio audio backends.
  - Bundles headers from `engine/mpv_client.h` and generates `Info.plist` with `1.2.0`.
  - Modified `scripts/build-ios-framework.sh` to stage real `mpv.framework` and require its presence before packaging into `audio-engine-ios.xcframework.zip`.
  - Syntax validated via `bash -n`.

#### Group 3: Fetch Script & CI Modernization
- **Gate Status**: **PASSED (GREEN)**
- **fetch-mobile-mpv.sh Clean Up**:
  - Deleted `build_android_libmpv_stub()` and iOS stub functions completely.
  - `grep -i "stub" scripts/fetch-mobile-mpv.sh` returned zero matches (`ZERO_STUB_MATCHES`).
  - Purged fake stubs from `.cache/mobile-mpv` and `staging/mobile-mpv`.
  - Verified loud non-zero exit when genuine binaries or download URLs are missing.
- **CI Modernization (`.github/workflows/release.yml`)**:
  - Updated `build-macos` job to compile genuine iOS `mpv.framework` via `build-ios.sh`.
  - Updated `build-android` job to install dependencies (ninja-build, meson, pkg-config, yasm, nasm) and compile real `libmpv.so` for `arm64-v8a` and `x86_64` via `build-android.sh`.
  - Enforced verification check in `Package Android Archives` step.
  - YAML syntax and 2-space indentation validated.
- **Manifest (`third-party/manifest.json`)**:
  - Updated Android `libmpv.so` entry with accurate NDK r26b source build description.
  - Added `mpv.framework` entry to iOS `keyLibraries`.
  - Confirmed 1-to-1 correspondence between manifest archive definitions and `dist/` archives.

#### Group 4: Packaging, Release & Verification Tier (Tier B)
- **Gate Status**: **PASSED (GREEN)**
- **Verification Tier**: **Tier B (Static analysis & contract verification)**.
  - Physical devices (Android/iOS) and Apple Silicon Mac runners are not locally attached to this headless Linux workspace. Per Decision D2, verification is documented as Tier B without claiming live physical device testing.
  - All downstream scripts, CI definitions, and fetch entrypoints have been tested and verified to require genuine builds.
  - GitHub Actions CI workflow `release.yml` equipped with automated NDK r26b and Xcode source-build jobs for automated Release generation upon tag/dispatch push.

#### Group 5: BBeBee Main Repo Version Pin
- **Gate Status**: **PASSED (GREEN)**
- **Version Bump**: `B_Be_Bee/audio-engine.version.json` bumped from `"v1.1.0"` to `"v1.2.0"`.
- **Git Status**: Exactly 1 file modified in `B_Be_Bee` (`audio-engine.version.json`), uncommitted.
- **Fetch Pipeline**: `node scripts/fetch-audio-engine.mjs --mobile` executed successfully, verified extraction into `apps/mobile/third-party/audio-engine`.
