# BBeBee Audio Engine — Android JNI Bridge Layer

## Overview

The `engine/jni/` module provides native JNI bindings for Android (`arm64-v8a` primary target, `x86_64` emulator debug target), bridging between Android / React Native application layers and the underlying C++ audio engine.

## Architecture

```
+-------------------------------------------------------------+
|               React Native / Kotlin Layer                   |
|           (org.bbebee.audio.BBeBeeAudioEngine)              |
+-------------------------------------------------------------+
                              |
                     JNI Method Invocations
                              |
+-------------------------------------------------------------+
|             BBeBee Audio Engine JNI Bridge (C++)            |
|       (bbebee_audio_jni.cpp / bbebee_audio_jni.hpp)         |
+-------------------------------------------------------------+
        |                    |                        |
    Audio Control &      Transcoder              Audio PCM
    Property Settings    (HAVE_LIBAV)            Tap Callback
        |                    |                        |
        v                    v                        v
+------------------+  +------------------+  +----------------------+
|  libmpv.so Core  |  |  transcode.cpp   |  | Lock-free RingBuffer |
| (OpenSL ES / AA) |  |  (static libav)  |  |  (PcmRingBuffer.cpp) |
+------------------+  +------------------+  +----------------------+
                                                         |
                                                Reader & FFT Spectrum
                                                         |
                                                         v
                                              +----------------------+
                                              |   FftProcessor.hpp   |
                                              |  (Radix-2 FFT 64bin) |
                                              +----------------------+
```

## Features

1. **Dual ABI Support**:
   - `arm64-v8a`: Production 64-bit ARM Android devices.
   - `x86_64`: Android Emulator development and automated CI environments.
2. **Dynamic `libmpv` Resolution via `dlopen`**:
   - Dynamically resolves `libmpv.so` at runtime using `dlopen("libmpv.so", RTLD_NOW)`.
   - Smoothly accommodates Android APK unpacked libraries and customized asset directories.
   - Non-fatal fallback if `libmpv` is missing or in mock mode.
3. **Lock-Free Concurrency for Audio & Visualizer**:
   - The libmpv PCM tap audio callback thread writes directly into `PcmRingBuffer` without mutex acquisition or memory allocation.
   - The React Native visualizer thread invokes `nativeGetSpectrum()` to consume buffered PCM frames, compute FFT spectrum magnitudes with temporal smoothing, and update UI state at 60 FPS.
4. **FFmpeg Transcoding (optional `HAVE_LIBAV`)**:
   - When CMake receives `BEE_FFMPEG_SDK_DIR` (the SDK `scripts/mpv-build/build-android.sh` stages next to `libmpv.so`), the bridge statically links `transcode.cpp` (libavformat/libavcodec/libswresample/libavutil plus libmp3lame/libvorbis/libopus/libogg) and exposes the transcoder to React Native.
   - Transcodes run on detached worker threads and never take the engine context mutex — playback control stays responsive while a job runs. Progress is delivered by polling `nativeGetTranscodeState`, the same pull model every other answer on this bridge uses; cancellation is the cooperative atomic flag the desktop engine's NDJSON `cancelTranscode` action uses.
   - Without the SDK the bridge builds unchanged and every transcode export answers honestly that transcoding is unsupported.
5. **Android Native Logging**:
   - Integrated with `<android/log.h>` under the `BBeBeeAudioJNI` tag.

## JNI Export API Reference

The authoritative declaration is `bbebee_audio_jni.hpp`; the Kotlin loader
`org.bbebee.audio.BBeBeeAudioEngine` mirrors it 1:1 with `external fun`s.
Playback exports map onto the `bbebeeMobileMpv` contract table
(B_Be_Bee/docs/audio/engine.md); the transcoder exports serve the
`bbebeeNativeCodec` seam instead.

| JNI Export | Arguments | Return | Description |
|---|---|---|---|
| `nativeInit` | `(jstring configDir)` | `jboolean` | Initializes the native engine context and binds libmpv |
| `nativeDestroy` | `()` | `void` | Shuts down engine, clears ring buffers, cancels transcode jobs, and frees resources |
| `nativeLoad` | `(jstring uri)` | `jboolean` | Loads audio track or network stream (`loadfile replace`) |
| `nativeLoadEx` | `(jstring uri, jstring headers)` | `jstring` | Gapless-aware load: `{ok, durationMs, resumed, endedCount, sampleRate?, channels?, bitDepth?}` as JSON; optional fields absent while unknown |
| `nativeAppend` | `(jstring uri, jstring headers)` | `jboolean` | Gapless playlist append (`loadfile append`) |
| `nativePlay` | `()` | `void` | Starts or resumes playback |
| `nativePlayAt` | `(jdouble atMs)` | `void` | Resumes, seeking to `atMs` first (NaN = keep the engine position) |
| `nativePause` | `()` | `void` | Pauses playback |
| `nativeStop` | `()` | `void` | Stops playback and resets buffers |
| `nativeSeekMs` | `(jdouble ms)` | `void` | Seeks to absolute timestamp in milliseconds |
| `nativeSetVolume` | `(jdouble volume)` | `void` | Sets output volume `[0.0, 1.0]` (mpv's 0..100 scale is internal) |
| `nativeSetMuted` | `(jboolean muted)` | `void` | Mutes or unmutes output |
| `nativeSetDspConfig` | `(jstring af, jstring replaygain, jboolean clip, jstring preamp, jstring fallback)` | `void` | Applies the lavfi `af` fragment and ReplayGain knobs |
| `nativeSetVisualizer` | `(jboolean enabled, jint fftSize)` | `void` | Enables the PCM tap consumer and sizes its FFT (0 keeps current size) |
| `nativeSetOutputDevice` | `(jstring device)` | `void` | Selects an mpv `audio-device` |
| `nativeSetStreamPlayback` | `(jboolean playing)` | `void` | No-op on Android (desktop degraded-path concept) |
| `nativeSetAudioExclusive` | `(jboolean exclusive)` | `void` | No-op on Android (WASAPI exclusive is a desktop concept) |
| `nativeGetSpectrum` | `(jbyteArray outFreq, jbyteArray outTime)` | `jboolean` | Populates FFT frequency spectrum and time-domain waveform (bins are unsigned 0..255, transported in signed bytes) |
| `nativeGetRingBufferStats` | `()` | `jlongArray` | Returns written, read, dropped, overflow, underrun frame stats |
| `nativeGetState` | `()` | `jstring` | `{positionMs, durationMs, status, endedCount}` as JSON; `endedCount` is a rising-edge eof counter |
| `nativeGetFftBinCount` | `()` | `jint` | Frequency bins per spectrum frame (fftSize / 2) |
| `nativeGetEngineStatus` | `()` | `jstring` | `{running, mpvAvailable, pcmTapAvailable, transcodeAvailable, platform}` as JSON |
| `nativeTranscode` | `(jstring jobId, jstring input, jstring output, jstring format, jstring bitrate, jint sampleRate, jint channels, jdouble startSec, jdouble endSec)` | `jstring` | Starts a detached transcode worker; `{ok, jobId}` or `{ok: false, error}` as JSON. Paths are filesystem paths (`file://` tolerated), URIs resolved by the Kotlin layer |
| `nativeCancelTranscode` | `(jstring jobId)` | `jboolean` | Sets the cooperative cancel flag; `true` when a running job was found |
| `nativeGetTranscodeState` | `(jstring jobId)` | `jstring` | `{jobId, found, running, done, ok, cancelled, progress, currentTimeSec, durationSec, error?}` as JSON |
| `nativeGetTranscodeCapabilities` | `()` | `jstring` | `{supported, encoders: {<id>: <bool>}}` — encoders actually present in the linked libavcodec, keyed by the desktop capability ids |

## Building with Android NDK

Add to your Android app's `build.gradle` (or `CMakeLists.txt`):

```groovy
android {
    defaultConfig {
        ndk {
            abiFilters 'arm64-v8a', 'x86_64'
        }
    }
    externalNativeBuild {
        cmake {
            path file("${projectDir}/../../../../B_Be_Bee-audio-engine/engine/jni/CMakeLists.txt")
            version "3.22.1"
        }
    }
}
```

### Transcode support

Build the mpv + FFmpeg pipeline first, then point CMake at the staged SDK:

```bash
bash scripts/mpv-build/build-android.sh arm64-v8a build/libmpv/android/arm64-v8a
cmake -B build/android/arm64-v8a -S engine/jni \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
  -DBEE_FFMPEG_SDK_DIR="$(pwd)/build/libmpv/android/arm64-v8a/ffmpeg-sdk"
```

Omitting `BEE_FFMPEG_SDK_DIR` builds the bridge without `HAVE_LIBAV`; the
transcoder exports then report "not supported" at runtime instead of failing
the build — the same degradation the desktop engine ships.
