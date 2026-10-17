#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# BBeBee Audio Engine - iOS Real mpv Cross-Compilation Script
# Targets: device (arm64), simulator (arm64, x86_64)
# Toolchain: Apple Clang / Xcode SDK (iOS 15.0+)
# Audio Backends: AudioUnit / CoreAudio
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TARGET_DIR="${1:-${ROOT_DIR}/staging/mobile-mpv/ios/mpv.framework}"
PATCH_FILE="${ROOT_DIR}/patches/mpv-pcm-tap.patch"

MPV_VERSION="v0.41.0"
FFMPEG_VERSION="n6.1.1"
FRAMEWORK_VERSION="1.2.0"

echo "=========================================================="
echo " BBeBee iOS Real mpv Builder"
echo "=========================================================="
echo "Target Dir:      ${TARGET_DIR}"
echo "mpv Version:     ${MPV_VERSION}"
echo "FFmpeg Version:  ${FFMPEG_VERSION}"
echo "Version Tag:     ${FRAMEWORK_VERSION}"
echo "=========================================================="

if ! command -v xcrun >/dev/null 2>&1; then
  echo "ERROR: xcrun not found. Building iOS mpv requires macOS with Xcode command line tools." >&2
  exit 1
fi

IPHONEOS_SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
IPHONESIM_SDK="$(xcrun --sdk iphonesimulator --show-sdk-path)"

if [ -z "${IPHONEOS_SDK}" ] || [ -z "${IPHONESIM_SDK}" ]; then
  echo "ERROR: Unable to locate iOS SDKs via xcrun." >&2
  exit 1
fi

WORK_DIR="${TMPDIR:-/tmp}/mpv-build-ios-$$"
mkdir -p "${WORK_DIR}"
cleanup() {
  rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

CORES=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

build_slice() {
  local slice_name="$1"
  local arch="$2"
  local sdk_name="$3"
  local sdk_path="$4"
  local min_ver_flag="$5"

  echo "=== Building slice: ${slice_name} (${arch} for ${sdk_name}) ==="
  local slice_prefix="${WORK_DIR}/prefix_${slice_name}"
  mkdir -p "${slice_prefix}/lib/pkgconfig" "${slice_prefix}/include"

  local clang_bin="$(xcrun --sdk "${sdk_name}" -f clang)"
  local clangxx_bin="$(xcrun --sdk "${sdk_name}" -f clang++)"
  local libtool_bin="$(xcrun --sdk "${sdk_name}" -f libtool)"

  # 1. Build FFmpeg for this iOS slice
  echo "--- Compiling FFmpeg for ${slice_name} ---"
  local ffmpeg_build="${WORK_DIR}/ffmpeg_${slice_name}"
  mkdir -p "${ffmpeg_build}"
  if [ ! -d "${WORK_DIR}/ffmpeg-src" ]; then
    git clone --depth 1 --branch "${FFMPEG_VERSION}" https://github.com/FFmpeg/FFmpeg.git "${WORK_DIR}/ffmpeg-src"
  fi
  cp -r "${WORK_DIR}/ffmpeg-src"/* "${ffmpeg_build}/"

  (
    cd "${ffmpeg_build}"
    ./configure \
      --prefix="${slice_prefix}" \
      --enable-cross-compile \
      --target-os=darwin \
      --arch="${arch}" \
      --cc="${clang_bin}" \
      --extra-cflags="-arch ${arch} -isysroot ${sdk_path} ${min_ver_flag} -fPIC -O3" \
      --extra-ldflags="-arch ${arch} -isysroot ${sdk_path} ${min_ver_flag}" \
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
      --enable-audiotoolbox \
      --enable-videotoolbox
    make -j"${CORES}"
    make install
  )

  # 2. Build mpv for this iOS slice
  echo "--- Compiling Patched mpv for ${slice_name} ---"
  local mpv_build="${WORK_DIR}/mpv_${slice_name}"
  mkdir -p "${mpv_build}"
  if [ ! -d "${WORK_DIR}/mpv-src" ]; then
    git clone --depth 1 --branch "${MPV_VERSION}" https://github.com/mpv-player/mpv.git "${WORK_DIR}/mpv-src"
    (
      cd "${WORK_DIR}/mpv-src"
      if git apply --ignore-whitespace --ignore-space-change "${PATCH_FILE}" 2>/dev/null; then
        echo "PCM tap patch applied cleanly."
      else
        tr -d '\r' < "${PATCH_FILE}" | git apply --ignore-whitespace --ignore-space-change -
      fi
    )
  fi
  cp -r "${WORK_DIR}/mpv-src"/* "${mpv_build}/"

  local cpu_family="${arch}"
  [ "${arch}" = "arm64" ] && cpu_family="aarch64"

  local cross_file="${WORK_DIR}/crossfile_${slice_name}.txt"
  cat << CROSSFILE > "${cross_file}"
[built-in options]
buildtype = 'release'
default_library = 'shared'
c_args = ['-arch', '${arch}', '-isysroot', '${sdk_path}', '${min_ver_flag}', '-I${slice_prefix}/include', '-O3', '-fPIC']
c_link_args = ['-arch', '${arch}', '-isysroot', '${sdk_path}', '${min_ver_flag}', '-L${slice_prefix}/lib', '-install_name', '@rpath/mpv.framework/mpv']

[binaries]
c = '${clang_bin}'
cpp = '${clangxx_bin}'
ar = 'ar'
nm = 'nm'
strip = 'strip'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = ['${slice_prefix}/lib/pkgconfig']

[host_machine]
system = 'darwin'
cpu_family = '${cpu_family}'
cpu = '${arch}'
endian = 'little'
CROSSFILE

  (
    cd "${mpv_build}"
    export PKG_CONFIG_SYSROOT_DIR="${slice_prefix}"
    export PKG_CONFIG_LIBDIR="${slice_prefix}/lib/pkgconfig"
    export PKG_CONFIG_PATH="${slice_prefix}/lib/pkgconfig"

    meson setup build \
      --cross-file "${cross_file}" \
      --default-library shared \
      -Dlibmpv=true \
      -Dcplayer=false \
      -Dcoreaudio=enabled \
      -Daudiounit=enabled \
      -Dalsa=disabled \
      -Dpulse=disabled \
      -Dpipewire=disabled \
      -Djack=disabled \
      -Dsndio=disabled \
      -Doss-audio=disabled \
      -Dwasapi=disabled \
      -Daudiotrack=disabled \
      -Daaudio=disabled \
      -Dopensles=disabled \
      -Dgl=disabled \
      -Dvulkan=disabled \
      -Dwayland=disabled \
      -Dx11=disabled \
      -Dsdl2-video=disabled \
      -Dsdl2-audio=disabled \
      -Dmanpage-build=disabled \
      -Dhtml-build=disabled \
      -Dpdf-build=disabled \
      -Dc_args='-Wno-deprecated-declarations'

    ninja -C build
    mkdir -p "${WORK_DIR}/binaries"
    cp build/libmpv*.dylib "${WORK_DIR}/binaries/mpv_${slice_name}"
  )
}

# Build the 3 standard iOS slices
build_slice "device_arm64" "arm64" "iphoneos" "${IPHONEOS_SDK}" "-miphoneos-version-min=15.0"
build_slice "sim_arm64" "arm64" "iphonesimulator" "${IPHONESIM_SDK}" "-mios-simulator-version-min=15.0"
build_slice "sim_x86_64" "x86_64" "iphonesimulator" "${IPHONESIM_SDK}" "-mios-simulator-version-min=15.0"

# Combine simulator slices into universal binary
echo "=== Creating Universal Simulator mpv Binary ==="
lipo -create \
  "${WORK_DIR}/binaries/mpv_sim_arm64" \
  "${WORK_DIR}/binaries/mpv_sim_x86_64" \
  -output "${WORK_DIR}/binaries/mpv_simulator"

# Assemble target mpv.framework
echo "=== Assembling Authentic mpv.framework ==="
rm -rf "${TARGET_DIR}"
mkdir -p "${TARGET_DIR}/Headers"

# Copy device binary as primary framework executable (or universal if needed)
cp "${WORK_DIR}/binaries/mpv_device_arm64" "${TARGET_DIR}/mpv"
chmod 0755 "${TARGET_DIR}/mpv"

# Copy verified client header
if [ -f "${ROOT_DIR}/engine/mpv_client.h" ]; then
  cp "${ROOT_DIR}/engine/mpv_client.h" "${TARGET_DIR}/Headers/client.h"
fi

# Generate valid Info.plist with pinned D1 version
cat << PLIST > "${TARGET_DIR}/Info.plist"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>mpv</string>
    <key>CFBundleIdentifier</key>
    <string>io.mpv.framework</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>mpv</string>
    <key>CFBundlePackageType</key>
    <string>FMWK</string>
    <key>CFBundleShortVersionString</key>
    <string>${FRAMEWORK_VERSION}</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>MinimumOSVersion</key>
    <string>15.0</string>
</dict>
</plist>
PLIST

echo "=== iOS mpv.framework Build Verification ==="
lipo -info "${TARGET_DIR}/mpv"
nm -defined-only "${TARGET_DIR}/mpv" | grep -E "mpv_create|mpv_set_pcm_callback"
echo "=== iOS Genuine mpv.framework successfully staged at ${TARGET_DIR} ==="
