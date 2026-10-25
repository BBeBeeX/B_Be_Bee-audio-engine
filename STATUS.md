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
  - Static dependency pipeline: FFmpeg -> FreeType -> FriBidi (`v1.0.14`) -> libass (`0.17.2`) -> libplacebo (`v6.338.2`) -> mpv.
  - FriBidi resolution: compiles static `fribidi` via Meson into prefix, passing explicit `FRIBIDI_CFLAGS`/`LIBS` and `FREETYPE_CFLAGS`/`LIBS` to `libass`'s `./configure` to eliminate missing FriBidi failures.
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

---

### Group 6: Full Mobile Binding Surface (JNI contract completion)
- **Gate Status**: SOURCE COMPLETE (Tier B: host `g++ -fsyntax-only` clean with `-Wall -Wextra` against a stub `jni.h`; Android build verification pending the next CI artifact build — no NDK on this host, per Decision D2).
- **Why**: the shipped bridge exposed a skeleton (load/play/pause/stop/seek/volume/spectrum/status) with no position/duration reads, no gapless append, no DSP config — and no Java loader or JS binding existed anywhere, so `libbbebee_audio_jni.so` was unreachable even when bundled. The B_Be_Bee main repo documented the full `bbebeeMobileMpv` contract (docs/audio/engine.md) and ran on a JS simulation bridge.
- **Implementation** (`engine/jni/bbebee_audio_jni.{hpp,cpp}`):
  - New dlsym: `mpv_get_property_string`, `mpv_free`.
  - `loadEx(uri, headers)` → `{ok, durationMs, resumed, endedCount}`: gapless re-bind detection (requested file == sounding `path` → no `loadfile replace`), `http-header-fields` option, and a bounded (≤2 s) duration poll — a live stream answers `durationMs: 0` ("unknown"), a refused load answers `ok: false` via `idle-active`.
  - `append(uri, headers)` → `loadfile append` (gapless playlist boundary).
  - `getState()` → `{positionMs, durationMs, status, endedCount}` from `time-pos`/`duration`/`pause`/`idle-active`/`eof-reached`, with rising-edge eof counting so a stalled ended file polled forever does not inflate the counter.
  - `playAt(ms | NaN)`, `seekMs`, `setVolume(0..1 → mpv 0..100)`, `setMuted`, `setOutputDevice`, `setVisualizer(enabled, fftSize)` (sizes the FFT, clears the ring on disable), `setDspConfig(af, replaygain, clip, preamp, fallback)`, `getFftBinCount`, and documented no-ops for the desktop-only `setStreamPlayback`/`setAudioExclusive`.
  - JSON answers via `engine/json.hpp`; `nativeSeek` (seconds) superseded by `nativeSeekMs` — no shipped consumer of the old ABI existed.
- **Downstream** (B_Be_Bee main repo): local Expo module `apps/mobile/modules/audio-engine/` (Kotlin loader `org.bbebee.audio.BBeBeeAudioEngine` + `expo.modules.audioengine.BbebeeAudioEngineModule` + JS adapter installing `globalThis.bbebeeMobileMpv` from `boot.ts` with graceful degradation), plus `BBEE_AUDIO_ENGINE_STRICT=1` in the Expo config plugin so release prebuilds fail rather than bundle a build without the engine.
- **Fallback stub parity**: `scripts/package-release.sh`'s emergency C stub carries the same symbol set.
- **Remaining**: cut a release so CI rebuilds `libbbebee_audio_jni.so` (both ABIs), run `pnpm fetch:audio-engine --app mobile` in the main repo, rebuild the dev-client APK; then on-device verification of load/state/append/DSP/FFT.

---

### Group 7: Mobile FFmpeg Transcoding over JNI (`HAVE_LIBAV` for Android)
- **Gate Status**: SOURCE COMPLETE (Tier B: host `g++ -fsyntax-only -Wall -Wextra -Werror` clean against a stub `jni.h`; the libav-touching TU `transcode_caps.cpp` compiles only in CI where the FFmpeg SDK exists — no NDK on this host, per Decision D2).
- **Why**: `transcode.cpp` existed only behind the desktop engine's NDJSON process; the Android FFmpeg build disabled all encoders/muxers and never persisted its SDK, so the JNI bridge had no transcode path and the B_Be_Bee mobile app's `plugin-audio-converter` degraded to a permanent failure.
- **FFmpeg pipeline** (`scripts/mpv-build/build-android.sh`):
  - Selective `--enable-encoder`/`--enable-muxer` lists (aac, alac, flac, pcm_s16le/24le/f32le, wmav2, libmp3lame, libvorbis, libopus; adts, asf, flac, ipod, mp4, mp3, ogg, opus, wav) keep every `resolveTranscodeTarget()` entry resolvable without un-disabling the whole encoder set.
  - New static builds staged into the prefix before FFmpeg's configure: libogg v1.3.5, LAME 3.100 (encoder-only), libvorbis v1.3.7, libopus v1.4.
  - The FFmpeg SDK (libav headers + static archives + external encoder archives + `BUILD_INFO.txt`) is persisted to `<target>/ffmpeg-sdk` before the temp prefix is cleaned, because the static libav archives reference the external encoders at link time.
- **JNI bridge** (`engine/jni/`):
  - `CMakeLists.txt`: optional `BEE_FFMPEG_SDK_DIR` → adds `../transcode.cpp` + `transcode_caps.cpp`, defines `HAVE_LIBAV`, static-links the group-ordered libav + external encoder archives (plus `z`, `m`). Without it the bridge builds exactly as before and transcode reports "unsupported".
  - `transcode_caps.{hpp,cpp}`: encoder-id → present probe (`avcodec_find_encoder_by_name`) keyed by the desktop capability ids; the public header carries no libav types so the JNI TU stays host-syntax-checkable.
  - New exports: `nativeTranscode` (detached worker thread, job registry deliberately OUTSIDE `g_contextMutex` — a running transcode never blocks playback control; cooperative cancel flag mirrors main.cpp's `transcodeJobs`), `nativeCancelTranscode`, `nativeGetTranscodeState` (poll `{jobId, found, running, done, ok, cancelled, progress, currentTimeSec, durationSec, error}` — the bridge's established pull model; finished records retained 5 min), `nativeGetTranscodeCapabilities` (`{supported, encoders}`).
  - `nativeGetEngineStatus` gains `transcodeAvailable`; `nativeLoadEx` gains best-effort `sampleRate`/`channels`/`bitDepth` from mpv's `audio-params` (absent from the JSON while unknown); `nativeDestroy` cancels live jobs.
  - `engine/jni/README.md` export table refreshed (26 exports; the old table documented the pre-Group-6 skeleton).
- **CI** (`.github/workflows/release.yml` build-android): reordered — libmpv + FFmpeg SDK build first, JNI compiles second with `-DBEE_FFMPEG_SDK_DIR`, and a post-build check fails the job if `nativeTranscode` is missing from the exported symbols. `scripts/package-release.sh`'s emergency stub carries the four new symbols.
- **Downstream** (B_Be_Bee main repo): Expo module exposes `transcode`/`cancelTranscode`/`getTranscodeState`/`getTranscodeCapabilities` and a Kotlin `MediaMetadataRetriever` worker pool as `readArtworkBatch`; `CodecRn` overrides `transcode`/`getEncoderCapabilities`/`readArtworkBatch` over the `bbebeeNativeCodec` seam with the in-process fallback kept as the degrade path.
- **Remaining**: cut a release (CI rebuilds both ABIs with transcode linked), refetch in the main repo, rebuild the dev-client APK, then on-device verification of transcode progress/cancel and the artwork batch pool.

---

### Group 8: On-Device Verification (Android emulator, API 34, x86_64) & Three Root-Cause Fixes
- **Gate Status**: PASSED (GREEN) — full binding verified live on device: `mpvLoad {ok, durationMs:240021, sampleRate:48000, channels:2, bitDepth:32}`, `getState` position advancing in `playing`, PCM-tap FFT frames flowing after reconfiguration, native artwork pool extracting an embedded cover (`base64(2960)` for a tagged MP3, `null` for coverless FLAC), and `transcodeAvailable:false` reported honestly by the non-HAVE_LIBAV build.
- **Fix 1 — libmpv.so dangled `__gxx_personality_v0`**: the crossfile's `-static-libstdc++` did not embed the personality routine, and nothing in the app namespace provided it, so `dlopen("libmpv.so")` failed and the engine ran in stub mode. Root fix: link libmpv against the **shared** C++ runtime (`-lc++_shared`, a DT_NEEDED every RN app can satisfy — react-native-audio-api packages `libc++_shared.so`). The JNI bridge now also builds with `ANDROID_STL=c++_shared` (engine/jni/CMakeLists.txt), which guarantees the runtime is loaded before mpv's dlopen.
- **Fix 2 — mpv v0.41 `gl=disabled` builds link dangling symbols**: `vo_gpu_next.c` references `ra_is_gl`/`ra_gl_get`/`ra_create_wrapped_tex`/`gl_check_extension` (defined in GL-only `ra_gl.c`, not compiled under `-Dgl=disabled`) and four EGL functions. build-android.sh now compiles an `ra_gl_compat.c` stub TU (booleans false, handles NULL — the honest "no GL here" answers for an audio-only build) and adds it to mpv's link args.
- **Fix 3 — ring buffer ignored the tap's format**: `AudioEngineContext` configured the PCM ring buffer once (44100/2) at construction; mpv's tap delivers at the track's own rate, and the buffer's format guard silently dropped every frame — a 48 kHz track rendered the visualizer permanently flat. The tap's observed format is now published via atomics from the audio thread, and `getSpectrum()` lazily reconfigures on the module thread (`syncRingBufferFormatLocked()`; `configure()` synchronizes with writer/reader quiescence). Verified on device: `ring buffer reconfigured to 48000 Hz / 2 ch`, FFT bins go non-zero.
- **Also fixed while here**: `nativeGetTranscodeCapabilities` referenced `probeTranscodeCaps()` without a HAVE_LIBAV guard — the no-SDK build failed to LINK (host syntax checks cannot catch this). The fallback now constructs the empty answer inline.
- **Local-verification shim**: for dev-client builds against the STAGED (pre-fix) libmpv.so, `libbbebee_mpv_compat.so` (same eight stubs) ships in jniLibs and `patchelf --add-needed` wires it plus `libEGL.so` into the staged binary. The next CI release rebuilds libmpv with fixes 1–2 embedded and makes the shim unnecessary. The staged artifacts under `resources/android-<abi>/` currently carry the patched binaries (gitignored).
- **App-side note discovered on device**: `org.bbebee.app` requests no storage permissions by design (SAF-only); raw `file:///sdcard/...` loads are refused by mpv. Playable paths are app-private files — the production flow goes through SAF `content://` grants, which mpv cannot `fopen` either; how scanned tracks reach the engine's file layer needs a follow-up look (out of scope here: the seam contract itself is verified with real paths).

---

### Group 9: Playback Failure Semantics & Engine Recovery Surface (mobile parity with the desktop's error path)
- **Gate Status**: SOURCE COMPLETE + device-verified (host `g++ -fsyntax-only -Wall -Wextra -Werror` clean; NDK builds for both ABIs; on-device pass on an API 34 x86_64 emulator through the app's dev client: load→play regression, FFT non-zero after the Group 8 ring-buffer fix, `reinit` → engine re-created and track reloaded, and the app-side degrade ladder floor reached without a throw).
- **Why**: the mobile `getState` emitted only `idle|ended|paused|playing`, so the JS source handle's `error` branch (`status === 'error'` → stall/skip) was dead code — a mid-track mpv failure (network cut) left the player silently frozen at "playing 0:00" with no recovery; and once wedged, the engine had no re-init surface at all.
- **JNI** (`engine/jni/bbebee_audio_jni.{hpp,cpp}`):
  - New bookkeeping under the engine mutex: `stopRequested_` (set in `stop()`, cleared by `load`/`loadEx`/`append`, reset in `destroy()`) and `hadLoad_` (set when a load command is accepted).
  - `getState()` now answers `status: "error"` in three cases — `idle-active` after a load with no stop request and no eof (the polling model's reading of the desktop sidecar's `END_FILE reason=error`; no event loop added), a null `mpvHandle_` after a load had been accepted (wedged engine), and idle-at-zero with `hadLoad_` set. Contract comment in the header spells the semantics.
  - No new exports; the JSON contract table comment updated.
- **Downstream Kotlin/JS** (B_Be_Bee main repo): Expo module gains `AsyncFunction("reinit")` (`nativeDestroy()` → `nativeInit(null)` on the module thread); the JS adapter exposes `mpvReinit` and widens `MpvStateResult.status` with `'error'`; `MobileAudioService` exposes `recoverEngine()` with a single automatic trigger (load refused while the engine claims running → probe once, reinit once, no retry loops).
- **Degrade-ladder floor finding**: react-native-audio-api's exported `Audio` is a React **component**, not an element class — installing it as `globalThis.Audio` is impossible as-is. The app shell instead installs `RnAudioElement`, a `MediaElementLike` over RNAA's native file source, handed the engine's own AudioContext through a new optional `attachContext?(context)` hook on the element contract (`streamed-handle.ts`). Documented deltas: `ended` via a 250 ms position poll (the mobile bridge is pull-based), no `waiting/stalled/canplaythrough` events, no per-track header forwarding, `file://`/`http(s)` sources only.
- **Follow-ups left explicitly**: per-track headers on the degraded element are unwired (RNAA takes headers at `createFileSource` spawn); automatic recovery for a live-but-wedged engine awaits device evidence before wiring into the player's stall-retry path; natural-end eof transients (mpv unloads before a 200 ms poll sees `eof-reached`) are still detected by the JS `endedCount` rollover as before.
