# BBeBee Audio Engine & Patched MPV Runtime

[![License](https://img.shields.io/badge/license-LGPL%202.1%2B%20%2F%20GPL%202.0%2B-blue.svg)](LICENSE)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-17-green.svg)](https://en.wikipedia.org/wiki/C%2B%2B17)
[![Platform Matrix](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20(arm64)%20%7C%20Windows%20%7C%20Android%20%7C%20iOS-lightgrey.svg)](https://github.com/BBeBeeX/B_Be_Bee-audio-engine)

High-performance native audio playback engine and patched `libmpv` runtime for [BBeBee](https://github.com/BBeBeeX/B_Be_Bee_X). Provides low-latency audio decoding, real-time lock-free PCM audio tapping, and hardware-efficient 64-band FFT spectrum analysis for audio visualization.

---

## 1. Upstream Provenance & Historical Declaration

This repository was decoupled and migrated from the primary BBeBee monorepo to resolve repository bloat (eliminating 400MB+ precompiled binaries from Git history) and establish an independent CI/CD lifecycle.

The source code and dependencies in this repository are derived and audited from the authoritative commits of the primary monorepo:
- **`BBeBeeX/B_Be_Bee_X@3422224`**: Tauri v2 desktop migration baseline, establishing the multi-platform `libmpv` dynamic library dependency closures and initial Rust/Node audio engine supervisor.
- **`BBeBeeX/B_Be_Bee_X@8728515`**: Current production baseline incorporating native OS Keychain integration and enhanced audio supervisor error boundaries.

All multi-platform dynamic libraries (`libmpv.2.dylib`, `libmpv.so.2`, `libmpv-2.dll`, `libasound.so.2`, `zlib1.dll`) have been cryptographically audited via cross-platform MD5 checksums and verified 100% byte-for-byte identical to upstream baselines (see [third-party/README.md](third-party/README.md)).

---

## 2. Architecture Overview

```
+-------------------------------------------------------------+
|             BBeBee Host (Tauri v2 / Electron)               |
|            Node.js Sidecar / Audio Supervisor               |
+-------------------------------------------------------------+
                              |
                     stdio NDJSON RPC 2.0
                              |
+-------------------------------------------------------------+
|              audio-engine C++ Native Process                |
|                    (engine/main.cpp)                        |
+-------------------------------------------------------------+
       |                                       |
  libmpv Core                             Realtime Audio
  Playback Controller                     PCM Tap Hook
       |                                       |
       v                                       v
+------------------+                 +----------------------+
|    libmpv.so     |                 | Lock-free RingBuffer |
| (Pulse/ALSA/WAS) |                 | (pcm_ring_buffer.cpp)|
+------------------+                 +----------------------+
                                               |
                                      60 FPS Reader Thread
                                               |
                                               v
                                     +----------------------+
                                     |   FftProcessor.hpp   |
                                     |  (Radix-2 FFT 64bin) |
                                     +----------------------+
                                               |
                                        NDJSON Spectrum
                                         Broadcast Frame
```

### Core Components

1. **`engine/`**: Standalone C++17 audio engine.
   - `main.cpp`: Manages child process lifecycle, listens for NDJSON RPC commands (`play`, `pause`, `stop`, `seek`, `dsp_chain`, `volume`, `transcode`, `cancelTranscode`) on `stdin`, dispatches events to `stdout`, and redirects all diagnostics to `stderr`.
   - `transcode.hpp` / `transcode.cpp`: File-to-file audio transcoding (`transcode` action) using the FFmpeg libraries bundled next to libmpv (libavformat/libavcodec/libswresample): demux -> decode -> resample -> encode -> mux, with progress events, trim and cooperative cancellation. Built when FFmpeg dev files are present (`HAVE_LIBAV`); otherwise the action reports "not supported".
   - `pcm_ring_buffer.hpp` / `pcm_ring_buffer.cpp`: Single-Producer Single-Consumer (SPSC) lock-free ring buffer with 64-byte cache-line separation on write/read heads, guaranteeing zero locks and zero allocations in the real-time audio callback thread.
   - `fft.hpp`: Header-only Cooley-Tukey Radix-2 FFT implementation with Hann windowing and exponential temporal smoothing (`opts.smoothingTimeConstant = 0.8f`), converting PCM audio frames into 64 frequency bins (`[0..255]`) and waveform time-domain data.
2. **`patches/mpv-pcm-tap.patch`**:
   - Modifies mpv's `audio/out/ao.c` to inject an `ao_pcm_tap` callback hook.
   - Exposes `mpv_set_pcm_callback(mpv_handle*, mpv_pcm_callback_fn, void*)` in `mpv_client.h`, allowing zero-copy tapping of decoded PCM audio directly before hardware delivery.
3. **`engine/jni/`**: JNI bridge implementation for Android (`arm64-v8a` and `x86_64`), bridging `libmpv` and spectrum frames for React Native.
4. **`engine/ios/`**: Objective-C++ bridge (`BBeBeeAudioBridge`) for iOS (`xcframework` target) CoreAudio output.

---

## 3. The `dlopen` Runtime Loading & Dynamic Closure Mechanism

The native engine does **not** link directly against `libmpv` at link time. Instead, it resolves `libmpv` dynamically at startup using `dlopen("libmpv.so.2", RTLD_NOW)` on Linux, `dlopen("libmpv.2.dylib", RTLD_NOW)` on macOS, and `LoadLibraryW(L"libmpv-2.dll")` on Windows.

### Rationale
- **Graceful Degradation**: If `libmpv` is missing or fails to load, the engine safely flags `mpvAvailable: false`, allowing the host application to fall back gracefully to HTML5/Chromium Web Audio.
- **Strict Dependency Resolution (`RTLD_NOW`)**: Resolving symbols immediately surface any missing shared libraries up-front rather than crashing mid-playback.
- **Embedded RPATH ($ORIGIN / @loader_path)**: Binaries are compiled with `-Wl,-rpath,$ORIGIN` (Linux) and `-Wl,-rpath,@executable_path -Wl,-rpath,@loader_path` (macOS), ensuring all sibling libraries bundled alongside `audio-engine` are resolved without modifying system `/usr/lib`.
- **Glibc Core Denylist**: On Linux, the transitive dependency bundler (`scripts/bundle-mpv-deps.js`) uses a strict denylist for glibc core components (`libc.so`, `libm.so`, `libpthread.so`, `libdl.so`, `libgcc_s.so`, `libstdc++.so`), bundling all multimedia dependencies (FFmpeg, PulseAudio, ALSA, PipeWire, FreeType) while relying on the host distribution's C runtime.

---

## 4. Versioning Strategy & Anchor File

This repository strictly follows [Semantic Versioning 2.0.0](https://semver.org/).

### Root Anchor: `audio-engine.version.json`
The primary BBeBee monorepo pins the exact version and artifact hashes through a single root configuration file:

```json
{
  "version": "1.0.0",
  "gitTag": "v1.0.0",
  "repository": "https://github.com/BBeBeeX/B_Be_Bee-audio-engine",
  "artifacts": {
    "darwin-arm64": {
      "url": "https://github.com/BBeBeeX/B_Be_Bee-audio-engine/releases/download/v1.0.0/audio-engine-darwin-arm64-full.tar.gz",
      "sha256": "..."
    },
    "linux-x64": {
      "url": "https://github.com/BBeBeeX/B_Be_Bee-audio-engine/releases/download/v1.0.0/audio-engine-linux-x64-full.tar.gz",
      "sha256": "..."
    },
    "win32-x64": {
      "url": "https://github.com/BBeBeeX/B_Be_Bee-audio-engine/releases/download/v1.0.0/audio-engine-win32-x64-full.zip",
      "sha256": "..."
    }
  }
}
```

---

## 5. Dual-Package Release Specification (Decision D2)

Each desktop platform release provides two packaging tiers:

| Tier | Asset Pattern | Contents | Intended Audience |
|---|---|---|---|
| **Full** | `audio-engine-<platform>-<arch>-full.<ext>` | `audio-engine` executable + patched `libmpv` + full transitive dependency closure | Official production application installers (Debian `.deb`, AppImage, Windows NSIS, macOS DMG) |
| **Slim** | `audio-engine-<platform>-<arch>-slim.<ext>` | `audio-engine` executable + patched `libmpv` only | System package managers (Arch AUR, Fedora RPM), Linux power users with system FFmpeg |

### Target Platforms
- **macOS**: `arm64` (Apple Silicon M-series) exclusively supported. Intel `x64` is permanently dropped per Architectural Decision D1.
- **Linux**: `x86_64` (glibc 2.35+).
- **Windows**: `win32-x64` (Windows 10/11 64-bit).
- **Android**: `arm64-v8a` (production devices) and `x86_64` (emulator).
- **iOS**: `arm64` + `x86_64` (xcframework).

---

## 6. Local Build Guide (Escape Hatch D3)

When developing offline, debugging C++ internals, or testing on unsupported platforms, you can build directly from source using the `--from-source` workflow.

### Prerequisites

- **CMake**: >= 3.16
- **C++ Compiler**: GCC >= 10, Clang >= 12, or MSVC >= 2019 (C++17 support required)
- **libmpv Development Headers**: `libmpv-dev` (Linux), `mpv` via Homebrew (macOS)

### Building the Audio Engine

```bash
# Clone the repository
git clone https://github.com/BBeBeeX/B_Be_Bee-audio-engine.git
cd B_Be_Bee-audio-engine

# Build audio-engine using CMake
mkdir -p build && cd build
cmake ../engine -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

# Run the 13-gate PCM Tap Concurrency Test Suite
ctest --output-on-failure
# Or run the test executable directly:
./audio-engine-tests
```

### Compiling Patched libmpv from Source

```bash
# Builds mpv from source, applies patches/mpv-pcm-tap.patch, and outputs to build/libmpv/
bash scripts/build-patched-mpv.sh
```

---

## 7. License

- The `audio-engine` C++ source code, JNI layer, and iOS bridges are licensed under the **MIT License**.
- `libmpv` and patched components are licensed under the **GNU Lesser General Public License (LGPL) v2.1+** or **GNU General Public License (GPL) v2+** depending on configured build flags.
