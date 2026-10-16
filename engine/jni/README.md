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
        |                                       |
    Audio Control &                          Audio PCM
    Property Settings                      Tap Callback
        |                                       |
        v                                       v
+------------------+                 +----------------------+
|  libmpv.so Core  |                 | Lock-free RingBuffer |
| (OpenSL ES / AA) |                 |  (PcmRingBuffer.cpp) |
+------------------+                 +----------------------+
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
4. **Android Native Logging**:
   - Integrated with `<android/log.h>` under the `BBeBeeAudioJNI` tag.

## JNI Export API Reference

| JNI Export | Arguments | Return | Description |
|---|---|---|---|
| `nativeInit` | `(jstring configDir)` | `jboolean` | Initializes the native engine context and binds libmpv |
| `nativeDestroy` | `()` | `void` | Shuts down engine, clears ring buffers, and frees resources |
| `nativeLoad` | `(jstring uri)` | `jboolean` | Loads audio track or network stream |
| `nativePlay` | `()` | `void` | Starts or resumes playback |
| `nativePause` | `()` | `void` | Pauses playback |
| `nativeStop` | `()` | `void` | Stops playback and resets buffers |
| `nativeSeek` | `(jdouble seconds)` | `void` | Seeks to absolute timestamp |
| `nativeSetVolume` | `(jdouble volume)` | `void` | Sets output volume `[0.0, 100.0]` |
| `nativeGetSpectrum` | `(jbyteArray outFreq, jbyteArray outTime)` | `jboolean` | Populates FFT frequency spectrum and time-domain waveform |
| `nativeGetRingBufferStats` | `()` | `jlongArray` | Returns written, read, dropped, overflow, underrun frame stats |
| `nativeGetEngineStatus` | `()` | `jstring` | Returns JSON status `{ mpvAvailable, pcmTapAvailable, platform }` |

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
