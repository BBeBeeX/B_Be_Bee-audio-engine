# Mobile libmpv Cross-Compilation Architecture & Route Specification

This document details the authentic cross-compilation pipeline for building `libmpv` on Android (arm64-v8a, x86_64) and iOS (device arm64, simulator arm64/x86_64) for `BBeBee-audio-engine`.

---

## 1. Toolchain & Dependency Baseline

| Component | Pinned Version / Revision | Source URL / Repository | Integrity / Applicability |
| :--- | :--- | :--- | :--- |
| **Android NDK** | `r26b` (API Level 26) | Android NDK Official Distribution | LLVM Clang toolchain |
| **mpv** | `v0.41.0` | `https://github.com/mpv-player/mpv.git` | Commit `41f6a645068483470267271e1d09966ca3b9f413` |
| **FFmpeg** | `n6.1.1` | `https://github.com/FFmpeg/FFmpeg.git` | Pinned release tag `n6.1.1` |
| **PCM Tap Patch**| `patches/mpv-pcm-tap.patch` | In-tree patch (276 lines) | **Applies cleanly** to mpv `v0.41.0` (`git apply --check` verified) |
| **FreeType** | `VER-2-13-2` | `https://gitlab.freedesktop.org/freetype/freetype.git` | Static font rasterizer for libass |
| **FriBidi** | `v1.0.14` | `https://github.com/fribidi/fribidi.git` | Static bidirectional text core required by libass |
| **libass** | `0.17.2` | `https://github.com/libass/libass.git` | Static OSD dependency required by mpv core |
| **libplacebo** | `v6.338.2` | `https://github.com/haasn/libplacebo.git` | Static rendering helper required by mpv core |

---

## 2. Architecture & Audio-Only Strategy

The audio engine strictly requires high-fidelity audio playback, DSP filters, and real-time post-volume PCM tap callback (`mpv_set_pcm_callback`) to feed visualization and spectrum analysis. It operates in headless audio-only mode.

### Audio Output (AO) Backends
- **Android**:
  - `opensles`: OpenSL ES audio output (universal Android compatibility)
  - `aaudio`: Low-latency modern Android AAudio backend
  - `audiotrack`: Android Java AudioTrack bridge
  - **Pruned**: `alsa`, `pulse`, `pipewire`, `jack`, `sndio`, `oss-audio` explicitly disabled
- **iOS**:
  - `audiounit`: Apple AudioUnit low-latency output
  - `coreaudio`: CoreAudio hardware output
  - **Pruned**: All Linux/Windows specific audio backends

### Video Pruning
- Meson flags: `-Dgl=disabled -Dvulkan=disabled -Dsdl2-video=disabled -Dwayland=disabled -Dx11=disabled -Dgbm=disabled -Ddrm=disabled -Dcplayer=false -Dmanpage-build=disabled -Dhtml-build=disabled -Dpdf-build=disabled`
- At runtime: Native bridge initializes mpv in headless mode (`--vo=null --no-video`).

---

## 3. Android Cross-Compilation Route (per ABI)

### 3.1 ABI Mapping
- `arm64-v8a`:
  - `ndk_triple`: `aarch64-linux-android`
  - `cpu`: `armv8-a`
  - `cpu_family`: `aarch64`
  - `CC`: `aarch64-linux-android26-clang`
  - `CXX`: `aarch64-linux-android26-clang++`
- `x86_64`:
  - `ndk_triple`: `x86_64-linux-android`
  - `cpu`: `generic`
  - `cpu_family`: `x86_64`
  - `CC`: `x86_64-linux-android26-clang`
  - `CXX`: `x86_64-linux-android26-clang++`

### 3.2 FFmpeg Configuration Draft
```bash
./configure \
  --prefix="${PREFIX_DIR}" \
  --enable-cross-compile \
  --target-os=android \
  --arch="${ARCH}" \
  --cpu="${CPU}" \
  --cross-prefix="${TOOLCHAIN}/bin/llvm-" \
  --cc="${TOOLCHAIN}/bin/${CC_NAME}" \
  --cxx="${TOOLCHAIN}/bin/${CXX_NAME}" \
  --ar="${TOOLCHAIN}/bin/llvm-ar" \
  --nm="${TOOLCHAIN}/bin/llvm-nm" \
  --ranlib="${TOOLCHAIN}/bin/llvm-ranlib" \
  --strip="${TOOLCHAIN}/bin/llvm-strip" \
  --extra-cflags="-O3 -fPIC -I${PREFIX_DIR}/include" \
  --extra-ldflags="-L${PREFIX_DIR}/lib -Wl,-z,max-page-size=16384" \
  --disable-programs \
  --disable-doc \
  --disable-avdevice \
  --disable-postproc \
  --disable-encoders \
  --disable-muxers \
  --enable-static \
  --disable-shared \
  --enable-pic \
  --enable-gpl \
  --enable-version3 \
  --enable-jni \
  --enable-mediacodec
```

### 3.3 mpv Meson Cross-File Draft
```ini
[built-in options]
buildtype = 'release'
default_library = 'shared'
c_args = ['-I@PREFIX_DIR@/include', '-O3', '-fPIC']
c_link_args = ['-L@PREFIX_DIR@/lib', '-Wl,-z,max-page-size=16384', '-Wl,-soname,libmpv.so']

[binaries]
c = '@TOOLCHAIN@/bin/@CC_NAME@'
cpp = '@TOOLCHAIN@/bin/@CXX_NAME@'
ar = '@TOOLCHAIN@/bin/llvm-ar'
nm = '@TOOLCHAIN@/bin/llvm-nm'
strip = '@TOOLCHAIN@/bin/llvm-strip'
pkg-config = 'pkg-config'

[host_machine]
system = 'android'
cpu_family = '@CPU_FAMILY@'
cpu = '@CPU@'
endian = 'little'
```

### 3.4 mpv Meson Setup Options
```bash
meson setup "${BUILD_DIR}" \
  --cross-file "${CROSS_FILE}" \
  --default-library shared \
  -Dlibmpv=true \
  -Dcplayer=false \
  -Daudiotrack=enabled \
  -Daaudio=enabled \
  -Dopensles=enabled \
  -Dalsa=disabled \
  -Dpulse=disabled \
  -Dpipewire=disabled \
  -Djack=disabled \
  -Dsndio=disabled \
  -Doss-audio=disabled \
  -Dwasapi=disabled \
  -Dcoreaudio=disabled \
  -Daudiounit=disabled \
  -Dgl=disabled \
  -Dvulkan=disabled \
  -Dwayland=disabled \
  -Dx11=disabled \
  -Dsdl2-video=disabled \
  -Dsdl2-audio=disabled \
  -Dmanpage-build=disabled \
  -Dhtml-build=disabled \
  -Dpdf-build=disabled
```

---

## 4. iOS Cross-Compilation Route

1. **Toolchain**: Apple clang via `xcrun --sdk iphoneos` and `xcrun --sdk iphonesimulator`.
2. **Slices**:
   - Device: `arm64` (`-miphoneos-version-min=15.0`)
   - Simulator: `arm64` + `x86_64` (`-mios-simulator-version-min=15.0`)
3. **Audio Backend**: CoreAudio / AudioUnit enabled; Video outputs disabled.
4. **Packaging**: Assembled into dynamic `mpv.framework` with headers from `engine/mpv_client.h` and version from D1. Bundled into `audio-engine-ios.xcframework.zip`.

---

## 5. PCM Tap Patch Applicability Conclusion

- **Tested Version**: mpv `v0.41.0` (commit `41f6a645068483470267271e1d09966ca3b9f413`).
- **Command Run**: `git apply --check patches/mpv-pcm-tap.patch`
- **Result**: **Clean application with zero conflicts (PATCH_APPLIES_CLEANLY)**.
- **Conclusion**: Patch is directly applicable without any semantic or syntactic modification.
