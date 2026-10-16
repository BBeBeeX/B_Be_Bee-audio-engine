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

echo "=== Compiling iOS Device (arm64) ==="
xcrun --sdk iphoneos clang++ -arch arm64 -isysroot "${IPHONEOS_SDK}" \
  -miphoneos-version-min=15.0 -std=c++17 -fobjc-arc \
  -I"${ROOT_DIR}/engine" \
  -c "${ROOT_DIR}/engine/pcm_ring_buffer.cpp" -o "${BUILD_DIR}/device/pcm_ring_buffer.o"
xcrun --sdk iphoneos clang++ -arch arm64 -isysroot "${IPHONEOS_SDK}" \
  -miphoneos-version-min=15.0 -std=c++17 -fobjc-arc \
  -I"${ROOT_DIR}/engine" \
  -c "${ROOT_DIR}/engine/ios/BBeBeeAudioBridge.mm" -o "${BUILD_DIR}/device/BBeBeeAudioBridge.o"
libtool -static -o "${BUILD_DIR}/device/libBBeBeeAudioBridge.a" \
  "${BUILD_DIR}/device/pcm_ring_buffer.o" "${BUILD_DIR}/device/BBeBeeAudioBridge.o"

echo "=== Compiling iOS Simulator (arm64) ==="
xcrun --sdk iphonesimulator clang++ -arch arm64 -isysroot "${IPHONESIM_SDK}" \
  -mios-simulator-version-min=15.0 -std=c++17 -fobjc-arc \
  -I"${ROOT_DIR}/engine" \
  -c "${ROOT_DIR}/engine/pcm_ring_buffer.cpp" -o "${BUILD_DIR}/sim-arm64/pcm_ring_buffer.o"
xcrun --sdk iphonesimulator clang++ -arch arm64 -isysroot "${IPHONESIM_SDK}" \
  -mios-simulator-version-min=15.0 -std=c++17 -fobjc-arc \
  -I"${ROOT_DIR}/engine" \
  -c "${ROOT_DIR}/engine/ios/BBeBeeAudioBridge.mm" -o "${BUILD_DIR}/sim-arm64/BBeBeeAudioBridge.o"
libtool -static -o "${BUILD_DIR}/sim-arm64/libBBeBeeAudioBridge.a" \
  "${BUILD_DIR}/sim-arm64/pcm_ring_buffer.o" "${BUILD_DIR}/sim-arm64/BBeBeeAudioBridge.o"

echo "=== Compiling iOS Simulator (x86_64) ==="
xcrun --sdk iphonesimulator clang++ -arch x86_64 -isysroot "${IPHONESIM_SDK}" \
  -mios-simulator-version-min=15.0 -std=c++17 -fobjc-arc \
  -I"${ROOT_DIR}/engine" \
  -c "${ROOT_DIR}/engine/pcm_ring_buffer.cpp" -o "${BUILD_DIR}/sim-x86_64/pcm_ring_buffer.o"
xcrun --sdk iphonesimulator clang++ -arch x86_64 -isysroot "${IPHONESIM_SDK}" \
  -mios-simulator-version-min=15.0 -std=c++17 -fobjc-arc \
  -I"${ROOT_DIR}/engine" \
  -c "${ROOT_DIR}/engine/ios/BBeBeeAudioBridge.mm" -o "${BUILD_DIR}/sim-x86_64/BBeBeeAudioBridge.o"
libtool -static -o "${BUILD_DIR}/sim-x86_64/libBBeBeeAudioBridge.a" \
  "${BUILD_DIR}/sim-x86_64/pcm_ring_buffer.o" "${BUILD_DIR}/sim-x86_64/BBeBeeAudioBridge.o"

echo "=== Creating Universal Simulator Library ==="
lipo -create "${BUILD_DIR}/sim-arm64/libBBeBeeAudioBridge.a" "${BUILD_DIR}/sim-x86_64/libBBeBeeAudioBridge.a" \
  -output "${BUILD_DIR}/sim/libBBeBeeAudioBridge.a"

echo "=== Creating XCFramework ==="
rm -rf "${BUILD_DIR}/BBeBeeAudioBridge.xcframework"
xcodebuild -create-xcframework \
  -library "${BUILD_DIR}/device/libBBeBeeAudioBridge.a" -headers "${BUILD_DIR}/headers" \
  -library "${BUILD_DIR}/sim/libBBeBeeAudioBridge.a" -headers "${BUILD_DIR}/headers" \
  -output "${BUILD_DIR}/BBeBeeAudioBridge.xcframework"

echo "=== Packaging iOS xcframework zip ==="
(cd "${BUILD_DIR}" && zip -rq "${DIST_DIR}/audio-engine-ios.xcframework.zip" BBeBeeAudioBridge.xcframework)
echo "=== Created ${DIST_DIR}/audio-engine-ios.xcframework.zip ==="
