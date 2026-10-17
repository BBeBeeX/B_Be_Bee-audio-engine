#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# BBeBee Audio Engine - Android Real libmpv Cross-Compilation Script
# Targets: arm64-v8a (aarch64), x86_64
# Toolchain: Android NDK r26b (API Level 26)
# ==============================================================================

ABI="${1:-arm64-v8a}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TARGET_DIR="${2:-${ROOT_DIR}/build/libmpv/android/${ABI}}"
PATCH_FILE="${ROOT_DIR}/patches/mpv-pcm-tap.patch"

# Pinned Dependency Versions & Sources
MPV_VERSION="v0.41.0"
FFMPEG_VERSION="n6.1.1"
LIBPLACEBO_VERSION="v6.338.2"
LIBASS_VERSION="0.17.2"
FREETYPE_VERSION="VER-2-13-2"
FRIBIDI_VERSION="v1.0.14"
HARFBUZZ_VERSION="8.3.0"

echo "=========================================================="
echo " BBeBee Android Real libmpv Builder"
echo "=========================================================="
echo "Target ABI:      ${ABI}"
echo "Target Dir:      ${TARGET_DIR}"
echo "mpv Version:     ${MPV_VERSION}"
echo "FFmpeg Version:  ${FFMPEG_VERSION}"
echo "=========================================================="

case "${ABI}" in
  arm64-v8a)
    ARCH="aarch64"
    CPU="armv8-a"
    CPU_FAMILY="aarch64"
    NDK_TRIPLE="aarch64-linux-android"
    ;;
  x86_64)
    ARCH="x86_64"
    CPU="generic"
    CPU_FAMILY="x86_64"
    NDK_TRIPLE="x86_64-linux-android"
    ;;
  *)
    echo "ERROR: Unsupported Android ABI '${ABI}'. Supported ABIs: arm64-v8a, x86_64" >&2
    exit 1
    ;;
esac

# 1. Locate Android NDK
NDK_PATH="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-${ANDROID_NDK:-}}}"
if [ -z "${NDK_PATH}" ] || [ ! -d "${NDK_PATH}" ]; then
  if [ -d "/usr/local/lib/android/sdk/ndk" ]; then
    NDK_PATH=$(find /usr/local/lib/android/sdk/ndk -maxdepth 1 -mindepth 1 | sort -V | tail -n 1)
  elif [ -d "/opt/android-sdk/ndk" ]; then
    NDK_PATH=$(find /opt/android-sdk/ndk -maxdepth 1 -mindepth 1 | sort -V | tail -n 1)
  elif [ -d "${HOME}/Android/Sdk/ndk" ]; then
    NDK_PATH=$(find "${HOME}/Android/Sdk/ndk" -maxdepth 1 -mindepth 1 | sort -V | tail -n 1)
  fi
fi

if [ -z "${NDK_PATH}" ] || [ ! -d "${NDK_PATH}" ]; then
  echo "ERROR: Android NDK not found. Please set ANDROID_NDK_HOME or ANDROID_NDK_ROOT." >&2
  exit 1
fi
echo "Using Android NDK: ${NDK_PATH}"

# 2. Locate LLVM toolchain within NDK
LLVM_DIR="$(find "${NDK_PATH}/toolchains/llvm/prebuilt" -maxdepth 2 -name bin -type d | head -n 1 || true)"
if [ -z "${LLVM_DIR}" ] || [ ! -d "${LLVM_DIR}" ]; then
  echo "ERROR: Prebuilt LLVM toolchain directory not found in ${NDK_PATH}." >&2
  exit 1
fi
echo "Using LLVM Toolchain: ${LLVM_DIR}"

API_LEVEL="26"
CC="${LLVM_DIR}/${NDK_TRIPLE}${API_LEVEL}-clang"
CXX="${LLVM_DIR}/${NDK_TRIPLE}${API_LEVEL}-clang++"
AR="${LLVM_DIR}/llvm-ar"
NM="${LLVM_DIR}/llvm-nm"
RANLIB="${LLVM_DIR}/llvm-ranlib"
STRIP="${LLVM_DIR}/llvm-strip"

if [ ! -x "${CC}" ]; then
  echo "ERROR: Compiler binary not executable: ${CC}" >&2
  exit 1
fi

WORK_DIR="${TMPDIR:-/tmp}/mpv-build-android-${ABI}-$$"
PREFIX_DIR="${WORK_DIR}/prefix"
mkdir -p "${PREFIX_DIR}/lib/pkgconfig" "${PREFIX_DIR}/include" "${TARGET_DIR}"

cleanup() {
  echo "Cleaning up temporary build directory: ${WORK_DIR}"
  rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

CORES=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
export PKG_CONFIG_LIBDIR="${PREFIX_DIR}/lib/pkgconfig:${PREFIX_DIR}/lib64/pkgconfig:${PREFIX_DIR}/share/pkgconfig"
export PKG_CONFIG_PATH="${PREFIX_DIR}/lib/pkgconfig:${PREFIX_DIR}/lib64/pkgconfig:${PREFIX_DIR}/share/pkgconfig"

CROSS_FILE="${WORK_DIR}/crossfile.txt"
cat << CROSSFILE > "${CROSS_FILE}"
[built-in options]
buildtype = 'release'
default_library = 'static'
c_args = ['-I${PREFIX_DIR}/include', '-O3', '-fPIC']
cpp_args = ['-I${PREFIX_DIR}/include', '-O3', '-fPIC']
c_link_args = ['-L${PREFIX_DIR}/lib', '-Wl,-z,max-page-size=16384', '-static-libstdc++']
cpp_link_args = ['-L${PREFIX_DIR}/lib', '-Wl,-z,max-page-size=16384', '-static-libstdc++']

[binaries]
c = '${CC}'
cpp = '${CXX}'
ar = '${AR}'
nm = '${NM}'
strip = '${STRIP}'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = ['${PREFIX_DIR}/lib/pkgconfig', '${PREFIX_DIR}/lib64/pkgconfig', '${PREFIX_DIR}/share/pkgconfig']

[host_machine]
system = 'android'
cpu_family = '${CPU_FAMILY}'
cpu = '${CPU}'
endian = 'little'
CROSSFILE

# 3. Build FFmpeg (Static, Audio/MediaCodec Tailored)
echo "=== [1/6] Building FFmpeg (${FFMPEG_VERSION}) for Android ${ABI} ==="
mkdir -p "${WORK_DIR}/ffmpeg-src"
git clone --depth 1 --branch "${FFMPEG_VERSION}" https://github.com/FFmpeg/FFmpeg.git "${WORK_DIR}/ffmpeg-src"

(
  cd "${WORK_DIR}/ffmpeg-src"
  ./configure \
    --prefix="${PREFIX_DIR}" \
    --libdir="${PREFIX_DIR}/lib" \
    --enable-cross-compile \
    --target-os=android \
    --arch="${ARCH}" \
    --cpu="${CPU}" \
    --cross-prefix="${LLVM_DIR}/llvm-" \
    --cc="${CC}" \
    --cxx="${CXX}" \
    --ar="${AR}" \
    --nm="${NM}" \
    --ranlib="${RANLIB}" \
    --strip="${STRIP}" \
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
  make -j"${CORES}"
  make install
)

# 4. Build FreeType (Static)
echo "=== [2/6] Building FreeType (${FREETYPE_VERSION}) for Android ${ABI} ==="
mkdir -p "${WORK_DIR}/freetype-src"
git clone --depth 1 --branch "${FREETYPE_VERSION}" https://gitlab.freedesktop.org/freetype/freetype.git "${WORK_DIR}/freetype-src"
(
  cd "${WORK_DIR}/freetype-src"
  meson setup build \
    --cross-file "${CROSS_FILE}" \
    --prefix="${PREFIX_DIR}" \
    --libdir=lib \
    --default-library=static \
    -Dbrotli=disabled \
    -Dbzip2=disabled \
    -Dharfbuzz=disabled \
    -Dpng=disabled \
    -Dzlib=disabled \
    -Dtests=disabled
  ninja -C build install
)

# 5. Build FriBidi (Static)
echo "=== [3/6] Building FriBidi (${FRIBIDI_VERSION}) for Android ${ABI} ==="
mkdir -p "${WORK_DIR}/fribidi-src"
git clone --depth 1 --branch "${FRIBIDI_VERSION}" https://github.com/fribidi/fribidi.git "${WORK_DIR}/fribidi-src"

(
  cd "${WORK_DIR}/fribidi-src"
  meson setup build \
    --cross-file "${CROSS_FILE}" \
    --prefix="${PREFIX_DIR}" \
    --libdir=lib \
    --default-library=static \
    -Dbin=false \
    -Ddocs=false \
    -Dtests=false
  ninja -C build install
)

# 6. Build HarfBuzz (Static, Minimal without glib/cairo/icu)
echo "=== [4/6] Building HarfBuzz (${HARFBUZZ_VERSION}) for Android ${ABI} ==="
mkdir -p "${WORK_DIR}/harfbuzz-src"
git clone --depth 1 --branch "${HARFBUZZ_VERSION}" https://github.com/harfbuzz/harfbuzz.git "${WORK_DIR}/harfbuzz-src"

(
  cd "${WORK_DIR}/harfbuzz-src"
  meson setup build \
    --cross-file "${CROSS_FILE}" \
    --prefix="${PREFIX_DIR}" \
    --libdir=lib \
    --default-library=static \
    -Dglib=disabled \
    -Dgobject=disabled \
    -Dcairo=disabled \
    -Dchafa=disabled \
    -Dicu=disabled \
    -Dgraphite=disabled \
    -Dgraphite2=disabled \
    -Dfreetype=disabled \
    -Dutilities=disabled \
    -Dtests=disabled \
    -Ddocs=disabled \
    -Dbenchmark=disabled \
    -Dintrospection=disabled
  ninja -C build install
)

# 7. Build libass (Static, minimal OSD requirement for mpv)
echo "=== [5/6] Building libass (${LIBASS_VERSION}) for Android ${ABI} ==="
mkdir -p "${WORK_DIR}/libass-src"
git clone --depth 1 --branch "${LIBASS_VERSION}" https://github.com/libass/libass.git "${WORK_DIR}/libass-src"
(
  cd "${WORK_DIR}/libass-src"
  meson setup build \
    --cross-file "${CROSS_FILE}" \
    --prefix="${PREFIX_DIR}" \
    --libdir=lib \
    --default-library=static \
    -Dfontconfig=disabled \
    -Ddirectwrite=disabled \
    -Dcoretext=disabled \
    -Dasm=disabled \
    -Dlibunibreak=disabled \
    -Drequire-system-font-provider=false \
    -Dtest=false \
    -Dprofile=false
  ninja -C build install
)

# 8. Build libplacebo (Static, minimal without vulkan/demos/shaderc)
echo "=== [6/6] Building libplacebo (${LIBPLACEBO_VERSION}) for Android ${ABI} ==="
mkdir -p "${WORK_DIR}/libplacebo-src"
git clone --depth 1 --branch "${LIBPLACEBO_VERSION}" https://github.com/haasn/libplacebo.git "${WORK_DIR}/libplacebo-src"

(
  cd "${WORK_DIR}/libplacebo-src"
  meson setup build \
    --cross-file "${CROSS_FILE}" \
    --prefix="${PREFIX_DIR}" \
    --libdir=lib \
    --default-library=static \
    -Dvulkan=disabled \
    -Dopengl=disabled \
    -Dshaderc=disabled \
    -Dglslang=disabled \
    -Dlcms=disabled \
    -Ddovi=disabled \
    -Dlibdovi=disabled \
    -Dunwind=disabled \
    -Dxxhash=disabled \
    -Ddemos=false \
    -Dtests=false \
    -Dbench=false \
    -Dfuzz=false
  ninja -C build install
)

# 9. Build Patched mpv (libmpv.so with PCM Tap and Android Audio Backends)
echo "=== Building Patched libmpv (${MPV_VERSION}) for Android ${ABI} ==="
mkdir -p "${WORK_DIR}/mpv-src"
git clone --depth 1 --branch "${MPV_VERSION}" https://github.com/mpv-player/mpv.git "${WORK_DIR}/mpv-src"

(
  cd "${WORK_DIR}/mpv-src"
  echo "Applying PCM tap patch..."
  if git apply --ignore-whitespace --ignore-space-change "${PATCH_FILE}" 2>/dev/null; then
    echo "PCM tap patch applied cleanly."
  else
    tr -d '\r' < "${PATCH_FILE}" | git apply --ignore-whitespace --ignore-space-change -
    echo "PCM tap patch applied with CRLF normalization."
  fi

  # Crossfile for shared libmpv.so output
  cat << CROSSFILE > "${CROSS_FILE}"
[built-in options]
buildtype = 'release'
default_library = 'shared'
c_args = ['-I${PREFIX_DIR}/include', '-O3', '-fPIC']
cpp_args = ['-I${PREFIX_DIR}/include', '-O3', '-fPIC']
c_link_args = ['-L${PREFIX_DIR}/lib', '-Wl,-z,max-page-size=16384', '-Wl,-soname,libmpv.so', '-static-libstdc++']
cpp_link_args = ['-L${PREFIX_DIR}/lib', '-Wl,-z,max-page-size=16384', '-Wl,-soname,libmpv.so', '-static-libstdc++']

[binaries]
c = '${CC}'
cpp = '${CXX}'
ar = '${AR}'
nm = '${NM}'
strip = '${STRIP}'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = ['${PREFIX_DIR}/lib/pkgconfig', '${PREFIX_DIR}/lib64/pkgconfig', '${PREFIX_DIR}/share/pkgconfig']

[host_machine]
system = 'android'
cpu_family = '${CPU_FAMILY}'
cpu = '${CPU}'
endian = 'little'
CROSSFILE

  meson setup build \
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
    -Dlua=disabled \
    -Djavascript=disabled \
    -Duchardet=disabled \
    -Drubberband=disabled \
    -Dlcms2=disabled \
    -Dlibarchive=disabled \
    -Dcdda=disabled \
    -Ddvdnav=disabled \
    -Diconv=disabled \
    -Dmanpage-build=disabled \
    -Dhtml-build=disabled \
    -Dpdf-build=disabled \
    -Dlibavdevice=disabled \
    -Dlibbluray=disabled \
    -Dvapoursynth=disabled \
    -Dzimg=disabled \
    -Dcaca=disabled \
    -Ddrm=disabled \
    -Dgbm=disabled \
    -Djpeg=disabled \
    -Dshaderc=disabled \
    -Dsixel=disabled \
    -Dspirv-cross=disabled \
    -Dvaapi=disabled \
    -Dvdpau=disabled \
    -Ddvbin=disabled \
    -Degl=disabled \
    -Degl-android=disabled \
    -Dc_args='-Wno-deprecated-declarations'

  ninja -C build
)

# 10. Stage and verify output libmpv.so
OUTPUT_SO="${TARGET_DIR}/libmpv.so"
cp -L "${WORK_DIR}/mpv-src/build/libmpv.so" "${OUTPUT_SO}"
chmod 0755 "${OUTPUT_SO}"

echo "=== Build Complete: ${OUTPUT_SO} ==="
file "${OUTPUT_SO}"

# Integrity checks (hundreds of symbols, client symbols, pcm tap callback)
SYMBOL_COUNT=$(nm -D --defined-only "${OUTPUT_SO}" | wc -l)
echo "Exported dynamic symbols count: ${SYMBOL_COUNT}"
if [ "${SYMBOL_COUNT}" -lt 50 ]; then
  echo "ERROR: Symbol count (${SYMBOL_COUNT}) is too low! Authentic libmpv must have hundreds of symbols." >&2
  exit 1
fi

REQUIRED_SYMBOLS=("mpv_create" "mpv_initialize" "mpv_destroy" "mpv_command" "mpv_set_pcm_callback")
for sym in "${REQUIRED_SYMBOLS[@]}"; do
  if ! nm -D --defined-only "${OUTPUT_SO}" | grep -q "[[:space:]]${sym}$"; then
    echo "ERROR: Required symbol '${sym}' missing from ${OUTPUT_SO}!" >&2
    exit 1
  fi
  echo "  [OK] Verified symbol: ${sym}"
done

echo "=== Android ${ABI} genuine libmpv.so successfully built and verified! ==="
