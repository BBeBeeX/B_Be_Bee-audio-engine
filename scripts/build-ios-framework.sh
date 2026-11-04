#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/ios"
DIST_DIR="${ROOT_DIR}/dist"

echo "=== Building BBeBeeAudioBridge.xcframework for iOS ==="
mkdir -p "${BUILD_DIR}/headers" "${BUILD_DIR}/device" "${BUILD_DIR}/sim-arm64" "${BUILD_DIR}/sim-x86_64" "${BUILD_DIR}/sim" "${DIST_DIR}"

cp "${ROOT_DIR}/engine/ios/BBeBeeAudioBridge.h" "${BUILD_DIR}/headers/"
cat << 'EOF' > "${BUILD_DIR}/headers/module.modulemap"
module BBeBeeAudioBridge {
    header "BBeBeeAudioBridge.h"
    export *
}
EOF

if ! command -v xcrun >/dev/null 2>&1; then
  echo "Error: xcrun not found. iOS xcframework build requires macOS with Xcode command line tools."
  exit 1
fi

IPHONEOS_SDK=$(xcrun --sdk iphoneos --show-sdk-path)
IPHONESIM_SDK=$(xcrun --sdk iphonesimulator --show-sdk-path)

# iOS transcode: the bridge slices below link the FFmpeg SDKs staged
# by scripts/mpv-build/build-ios.sh. If any slice SDK is missing (e.g.
# mpv.framework was built by an older build-ios.sh), rebuild via
# build-ios.sh now — it stages the SDKs alongside mpv.framework —
# rather than silently building a bridge without transcode support.
for _sdk in device_arm64 sim_arm64 sim_x86_64; do
  if [ ! -f "${ROOT_DIR}/staging/mobile-mpv/ios/ffmpeg-sdk-${_sdk}/lib/libavcodec.a" ]; then
    echo "FFmpeg SDK for ${_sdk} missing; building via scripts/mpv-build/build-ios.sh..."
    bash "${ROOT_DIR}/scripts/mpv-build/build-ios.sh" "${ROOT_DIR}/staging/mobile-mpv/ios/mpv.framework"
    break
  fi
done
for _sdk in device_arm64 sim_arm64 sim_x86_64; do
  if [ ! -f "${ROOT_DIR}/staging/mobile-mpv/ios/ffmpeg-sdk-${_sdk}/lib/libavcodec.a" ]; then
    echo "ERROR: FFmpeg SDK for ${_sdk} not found and could not be built. Refusing to package a bridge without transcode support." >&2
    exit 1
  fi
done

echo "=== Compiling iOS Device (arm64) ==="
# iOS transcode: the bridge's transcode code links the FFmpeg SDK
# staged by scripts/mpv-build/build-ios.sh for this slice. The SDK
# archives are merged into the bridge archive below (a static library
# cannot "link" them — libtool -static concatenates their objects),
# so the consuming app needs no extra link flags for transcode.
DEVICE_SDK="${ROOT_DIR}/staging/mobile-mpv/ios/ffmpeg-sdk-device_arm64"
xcrun --sdk iphoneos clang++ -arch arm64 -isysroot "${IPHONEOS_SDK}" \
  -miphoneos-version-min=16.0 -std=c++17 -fobjc-arc \
  -I"${ROOT_DIR}/engine" \
  -c "${ROOT_DIR}/engine/pcm_ring_buffer.cpp" -o "${BUILD_DIR}/device/pcm_ring_buffer.o"
xcrun --sdk iphoneos clang++ -arch arm64 -isysroot "${IPHONEOS_SDK}" \
  -miphoneos-version-min=16.0 -std=c++17 -fobjc-arc \
  -I"${ROOT_DIR}/engine" \
  -c "${ROOT_DIR}/engine/ios/BBeBeeAudioBridge.mm" -o "${BUILD_DIR}/device/BBeBeeAudioBridge.o"
xcrun --sdk iphoneos clang++ -arch arm64 -isysroot "${IPHONEOS_SDK}" \
  -miphoneos-version-min=16.0 -std=c++17 -DHAVE_LIBAV \
  -I"${ROOT_DIR}/engine" -I"${DEVICE_SDK}/include" \
  -c "${ROOT_DIR}/engine/transcode.cpp" -o "${BUILD_DIR}/device/transcode.o"
xcrun --sdk iphoneos clang++ -arch arm64 -isysroot "${IPHONEOS_SDK}" \
  -miphoneos-version-min=16.0 -std=c++17 -DHAVE_LIBAV \
  -I"${ROOT_DIR}/engine" -I"${DEVICE_SDK}/include" \
  -c "${ROOT_DIR}/engine/jni/transcode_caps.cpp" -o "${BUILD_DIR}/device/transcode_caps.o"
libtool -static -o "${BUILD_DIR}/device/libBBeBeeAudioBridge.a" \
  "${BUILD_DIR}/device/pcm_ring_buffer.o" "${BUILD_DIR}/device/BBeBeeAudioBridge.o" \
  "${BUILD_DIR}/device/transcode.o" "${BUILD_DIR}/device/transcode_caps.o" \
  "${DEVICE_SDK}"/lib/libavcodec.a "${DEVICE_SDK}"/lib/libavformat.a \
  "${DEVICE_SDK}"/lib/libavutil.a "${DEVICE_SDK}"/lib/libswresample.a \
  "${DEVICE_SDK}"/lib/libmp3lame.a "${DEVICE_SDK}"/lib/libopus.a \
  "${DEVICE_SDK}"/lib/libvorbis.a "${DEVICE_SDK}"/lib/libvorbisenc.a \
  "${DEVICE_SDK}"/lib/libogg.a

echo "=== Compiling iOS Simulator (arm64) ==="
SIM_ARM64_SDK="${ROOT_DIR}/staging/mobile-mpv/ios/ffmpeg-sdk-sim_arm64"
xcrun --sdk iphonesimulator clang++ -arch arm64 -isysroot "${IPHONESIM_SDK}" \
  -mios-simulator-version-min=16.0 -std=c++17 -fobjc-arc \
  -I"${ROOT_DIR}/engine" \
  -c "${ROOT_DIR}/engine/pcm_ring_buffer.cpp" -o "${BUILD_DIR}/sim-arm64/pcm_ring_buffer.o"
xcrun --sdk iphonesimulator clang++ -arch arm64 -isysroot "${IPHONESIM_SDK}" \
  -mios-simulator-version-min=16.0 -std=c++17 -fobjc-arc \
  -I"${ROOT_DIR}/engine" \
  -c "${ROOT_DIR}/engine/ios/BBeBeeAudioBridge.mm" -o "${BUILD_DIR}/sim-arm64/BBeBeeAudioBridge.o"
xcrun --sdk iphonesimulator clang++ -arch arm64 -isysroot "${IPHONESIM_SDK}" \
  -mios-simulator-version-min=16.0 -std=c++17 -DHAVE_LIBAV \
  -I"${ROOT_DIR}/engine" -I"${SIM_ARM64_SDK}/include" \
  -c "${ROOT_DIR}/engine/transcode.cpp" -o "${BUILD_DIR}/sim-arm64/transcode.o"
xcrun --sdk iphonesimulator clang++ -arch arm64 -isysroot "${IPHONESIM_SDK}" \
  -mios-simulator-version-min=16.0 -std=c++17 -DHAVE_LIBAV \
  -I"${ROOT_DIR}/engine" -I"${SIM_ARM64_SDK}/include" \
  -c "${ROOT_DIR}/engine/jni/transcode_caps.cpp" -o "${BUILD_DIR}/sim-arm64/transcode_caps.o"
libtool -static -o "${BUILD_DIR}/sim-arm64/libBBeBeeAudioBridge.a" \
  "${BUILD_DIR}/sim-arm64/pcm_ring_buffer.o" "${BUILD_DIR}/sim-arm64/BBeBeeAudioBridge.o" \
  "${BUILD_DIR}/sim-arm64/transcode.o" "${BUILD_DIR}/sim-arm64/transcode_caps.o" \
  "${SIM_ARM64_SDK}"/lib/libavcodec.a "${SIM_ARM64_SDK}"/lib/libavformat.a \
  "${SIM_ARM64_SDK}"/lib/libavutil.a "${SIM_ARM64_SDK}"/lib/libswresample.a \
  "${SIM_ARM64_SDK}"/lib/libmp3lame.a "${SIM_ARM64_SDK}"/lib/libopus.a \
  "${SIM_ARM64_SDK}"/lib/libvorbis.a "${SIM_ARM64_SDK}"/lib/libvorbisenc.a \
  "${SIM_ARM64_SDK}"/lib/libogg.a

echo "=== Compiling iOS Simulator (x86_64) ==="
SIM_X86_64_SDK="${ROOT_DIR}/staging/mobile-mpv/ios/ffmpeg-sdk-sim_x86_64"
xcrun --sdk iphonesimulator clang++ -arch x86_64 -isysroot "${IPHONESIM_SDK}" \
  -mios-simulator-version-min=16.0 -std=c++17 -fobjc-arc \
  -I"${ROOT_DIR}/engine" \
  -c "${ROOT_DIR}/engine/pcm_ring_buffer.cpp" -o "${BUILD_DIR}/sim-x86_64/pcm_ring_buffer.o"
xcrun --sdk iphonesimulator clang++ -arch x86_64 -isysroot "${IPHONESIM_SDK}" \
  -mios-simulator-version-min=16.0 -std=c++17 -fobjc-arc \
  -I"${ROOT_DIR}/engine" \
  -c "${ROOT_DIR}/engine/ios/BBeBeeAudioBridge.mm" -o "${BUILD_DIR}/sim-x86_64/BBeBeeAudioBridge.o"
xcrun --sdk iphonesimulator clang++ -arch x86_64 -isysroot "${IPHONESIM_SDK}" \
  -mios-simulator-version-min=16.0 -std=c++17 -DHAVE_LIBAV \
  -I"${ROOT_DIR}/engine" -I"${SIM_X86_64_SDK}/include" \
  -c "${ROOT_DIR}/engine/transcode.cpp" -o "${BUILD_DIR}/sim-x86_64/transcode.o"
xcrun --sdk iphonesimulator clang++ -arch x86_64 -isysroot "${IPHONESIM_SDK}" \
  -mios-simulator-version-min=16.0 -std=c++17 -DHAVE_LIBAV \
  -I"${ROOT_DIR}/engine" -I"${SIM_X86_64_SDK}/include" \
  -c "${ROOT_DIR}/engine/jni/transcode_caps.cpp" -o "${BUILD_DIR}/sim-x86_64/transcode_caps.o"
libtool -static -o "${BUILD_DIR}/sim-x86_64/libBBeBeeAudioBridge.a" \
  "${BUILD_DIR}/sim-x86_64/pcm_ring_buffer.o" "${BUILD_DIR}/sim-x86_64/BBeBeeAudioBridge.o" \
  "${BUILD_DIR}/sim-x86_64/transcode.o" "${BUILD_DIR}/sim-x86_64/transcode_caps.o" \
  "${SIM_X86_64_SDK}"/lib/libavcodec.a "${SIM_X86_64_SDK}"/lib/libavformat.a \
  "${SIM_X86_64_SDK}"/lib/libavutil.a "${SIM_X86_64_SDK}"/lib/libswresample.a \
  "${SIM_X86_64_SDK}"/lib/libmp3lame.a "${SIM_X86_64_SDK}"/lib/libopus.a \
  "${SIM_X86_64_SDK}"/lib/libvorbis.a "${SIM_X86_64_SDK}"/lib/libvorbisenc.a \
  "${SIM_X86_64_SDK}"/lib/libogg.a

echo "=== Creating Universal Simulator Library ==="
lipo -create "${BUILD_DIR}/sim-arm64/libBBeBeeAudioBridge.a" "${BUILD_DIR}/sim-x86_64/libBBeBeeAudioBridge.a" \
  -output "${BUILD_DIR}/sim/libBBeBeeAudioBridge.a"

echo "=== Creating XCFramework ==="
rm -rf "${BUILD_DIR}/BBeBeeAudioBridge.xcframework"
xcodebuild -create-xcframework \
  -library "${BUILD_DIR}/device/libBBeBeeAudioBridge.a" -headers "${BUILD_DIR}/headers" \
  -library "${BUILD_DIR}/sim/libBBeBeeAudioBridge.a" -headers "${BUILD_DIR}/headers" \
  -output "${BUILD_DIR}/BBeBeeAudioBridge.xcframework"

echo "=== Staging mpv framework support ==="
if [ ! -d "${ROOT_DIR}/staging/mobile-mpv/ios/mpv.framework" ]; then
  echo "mpv.framework not pre-staged; building via scripts/mpv-build/build-ios.sh..."
  bash "${ROOT_DIR}/scripts/mpv-build/build-ios.sh" "${ROOT_DIR}/staging/mobile-mpv/ios/mpv.framework"
fi

if [ ! -d "${ROOT_DIR}/staging/mobile-mpv/ios/mpv.framework" ]; then
  echo "ERROR: Genuine iOS mpv.framework not found and could not be built. Refusing to package incomplete archive." >&2
  exit 1
fi

cp -a "${ROOT_DIR}/staging/mobile-mpv/ios/mpv.framework" "${BUILD_DIR}/"

echo "=== Packaging iOS xcframework zip ==="
(cd "${BUILD_DIR}" && zip -rq "${DIST_DIR}/audio-engine-ios.xcframework.zip" BBeBeeAudioBridge.xcframework mpv.framework)
echo "=== Created ${DIST_DIR}/audio-engine-ios.xcframework.zip ==="
