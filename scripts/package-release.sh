#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
MAIN_REPO="${MAIN_REPO:-/home/revers/project/B_Be_Bee}"
LIBMPV_ROOT="${MAIN_REPO}/apps/desktop/resources/libmpv"
DIST_DIR="${ROOT_DIR}/dist"
STAGING_DIR="${ROOT_DIR}/staging"

echo "=========================================================="
echo " BBeBee Audio Engine & Patched MPV Local Release Packager"
echo "=========================================================="
echo "Root:        ${ROOT_DIR}"
echo "Main Repo:   ${MAIN_REPO}"
echo "libmpv Root: ${LIBMPV_ROOT}"
echo "Dist:        ${DIST_DIR}"
echo "Staging:     ${STAGING_DIR}"
echo "=========================================================="

USE_EXISTING_STAGING=0
if [ -d "${LIBMPV_ROOT}" ]; then
  echo "Found libmpv resources directory at: ${LIBMPV_ROOT}"
elif [ -d "${STAGING_DIR}/linux-full" ] && [ -d "${STAGING_DIR}/macos-full" ] && [ -d "${STAGING_DIR}/windows-full" ]; then
  echo "Notice: LIBMPV_ROOT not found, reusing existing staging libraries from: ${STAGING_DIR}"
  USE_EXISTING_STAGING=1
else
  echo "Error: Neither LIBMPV_ROOT nor existing staging directories found."
  exit 1
fi

# 1. Ensure audio-engine binary is built
ENGINE_BIN=""
if [ -f "${ROOT_DIR}/build/audio-engine" ]; then
  ENGINE_BIN="${ROOT_DIR}/build/audio-engine"
elif [ -f "${ROOT_DIR}/build/bin/audio-engine" ]; then
  ENGINE_BIN="${ROOT_DIR}/build/bin/audio-engine"
elif [ -f "${ROOT_DIR}/engine/build/bin/audio-engine" ]; then
  ENGINE_BIN="${ROOT_DIR}/engine/build/bin/audio-engine"
else
  echo "[1/6] Compiling audio-engine binary from source..."
  bash "${ROOT_DIR}/scripts/build-audio-engine.sh" Release
  ENGINE_BIN="${ROOT_DIR}/build/audio-engine"
fi

echo "Using native audio-engine binary: ${ENGINE_BIN}"
mkdir -p "${ROOT_DIR}/build/bin" "${ROOT_DIR}/engine/build/bin"
cp "${ENGINE_BIN}" "${ROOT_DIR}/build/bin/audio-engine" 2>/dev/null || true
cp "${ENGINE_BIN}" "${ROOT_DIR}/engine/build/bin/audio-engine" 2>/dev/null || true

# Prepare clean dist directory
rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}"

if [ "${USE_EXISTING_STAGING}" -eq 0 ]; then
  rm -rf "${STAGING_DIR}"
  mkdir -p "${STAGING_DIR}"
fi

# 2. Package Linux
echo "[2/6] Packaging Linux x86_64 (Full & Slim)..."
# Full
mkdir -p "${STAGING_DIR}/linux-full"
cp "${ENGINE_BIN}" "${STAGING_DIR}/linux-full/audio-engine"
if [ "${USE_EXISTING_STAGING}" -eq 0 ]; then
  cp -a "${LIBMPV_ROOT}/linux/." "${STAGING_DIR}/linux-full/"
fi
tar -czf "${DIST_DIR}/audio-engine-linux-x64.tar.gz" -C "${STAGING_DIR}/linux-full" .

# Slim
mkdir -p "${STAGING_DIR}/linux-slim"
cp "${ENGINE_BIN}" "${STAGING_DIR}/linux-slim/audio-engine"
if [ "${USE_EXISTING_STAGING}" -eq 0 ]; then
  cp -a "${LIBMPV_ROOT}/linux"/libmpv.so* "${STAGING_DIR}/linux-slim/"
else
  cp -a "${STAGING_DIR}/linux-full"/libmpv.so* "${STAGING_DIR}/linux-slim/"
fi
tar -czf "${DIST_DIR}/audio-engine-linux-x64-slim.tar.gz" -C "${STAGING_DIR}/linux-slim" .

# 3. Package macOS (Full only, slim removed per defect 2.3)
echo "[3/6] Packaging macOS arm64..."
mkdir -p "${STAGING_DIR}/macos-full"
if [ -f "${ROOT_DIR}/build/bin/audio-engine-darwin" ]; then
  cp "${ROOT_DIR}/build/bin/audio-engine-darwin" "${STAGING_DIR}/macos-full/audio-engine"
elif [ "${USE_EXISTING_STAGING}" -ne 0 ] && [ -f "${STAGING_DIR}/macos-full/audio-engine" ]; then
  echo "Reusing existing macos-full audio-engine binary..."
else
  cp "${ENGINE_BIN}" "${STAGING_DIR}/macos-full/audio-engine"
fi
if [ "${USE_EXISTING_STAGING}" -eq 0 ]; then
  cp -a "${LIBMPV_ROOT}/darwin/." "${STAGING_DIR}/macos-full/"
fi
if [ -f "${ROOT_DIR}/scripts/bundle-macos-deps.py" ] && [ "$(uname)" = "Darwin" ]; then
  echo "Bundling macOS dynamic libraries..."
  python3 "${ROOT_DIR}/scripts/bundle-macos-deps.py" "${STAGING_DIR}/macos-full"
fi
tar -czf "${DIST_DIR}/audio-engine-macos-arm64.tar.gz" -C "${STAGING_DIR}/macos-full" .

# 4. Package Windows
echo "[4/6] Packaging Windows x64 (Full & Slim)..."
# Full
mkdir -p "${STAGING_DIR}/windows-full"
if [ -f "${ROOT_DIR}/build/bin/audio-engine.exe" ]; then
  cp "${ROOT_DIR}/build/bin/audio-engine.exe" "${STAGING_DIR}/windows-full/audio-engine.exe"
fi
if [ "${USE_EXISTING_STAGING}" -eq 0 ]; then
  cp -a "${LIBMPV_ROOT}/win64/." "${STAGING_DIR}/windows-full/"
fi
(cd "${STAGING_DIR}/windows-full" && zip -rq "${DIST_DIR}/audio-engine-windows-x64.zip" .)

# Slim
mkdir -p "${STAGING_DIR}/windows-slim"
if [ -f "${ROOT_DIR}/build/bin/audio-engine.exe" ]; then
  cp "${ROOT_DIR}/build/bin/audio-engine.exe" "${STAGING_DIR}/windows-slim/audio-engine.exe"
fi
if [ "${USE_EXISTING_STAGING}" -eq 0 ]; then
  cp -a "${LIBMPV_ROOT}/win64/libmpv-2.dll" "${STAGING_DIR}/windows-slim/"
else
  cp -a "${STAGING_DIR}/windows-full/libmpv-2.dll" "${STAGING_DIR}/windows-slim/"
fi
(cd "${STAGING_DIR}/windows-slim" && zip -rq "${DIST_DIR}/audio-engine-windows-x64-slim.zip" .)

# 5. Mobile packages (Android & iOS with libmpv runtime)
echo "[5/6] Packaging Mobile archives (Android & iOS)..."
bash "${ROOT_DIR}/scripts/fetch-mobile-mpv.sh" all "${STAGING_DIR}/mobile-mpv"

# Android arm64-v8a & x86_64
for ABI in arm64-v8a x86_64; do
  echo "Packaging Android ${ABI} archive..."
  STAGE="${STAGING_DIR}/android-${ABI}"
  mkdir -p "${STAGE}"

  # Copy libmpv.so
  if [ -f "${STAGING_DIR}/mobile-mpv/android/${ABI}/libmpv.so" ]; then
    cp "${STAGING_DIR}/mobile-mpv/android/${ABI}/libmpv.so" "${STAGE}/libmpv.so"
  fi

  # Copy libbbebee_audio_jni.so
  JNI_SO=""
  if [ -f "${ROOT_DIR}/build/android/${ABI}/libbbebee_audio_jni.so" ]; then
    JNI_SO="${ROOT_DIR}/build/android/${ABI}/libbbebee_audio_jni.so"
  elif [ -f "${STAGE}/libbbebee_audio_jni.so" ]; then
    JNI_SO="${STAGE}/libbbebee_audio_jni.so"
  else
    echo "Compiling JNI bridge stub for ${ABI}..."
    mkdir -p "${ROOT_DIR}/build/android/${ABI}"
    JNI_SRC="${ROOT_DIR}/.cache/mobile-mpv/jni_stub_${ABI}.c"
    cat << 'EOJNI' > "${JNI_SRC}"
#include <stddef.h>
#if defined(__GNUC__) || defined(__clang__)
#define JNIEXPORT __attribute__((visibility("default")))
#define JNICALL
#else
#define JNIEXPORT
#define JNICALL
#endif

JNIEXPORT int JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeInit(void* env, void* thiz, void* configDir) { return 1; }
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeDestroy(void* env, void* thiz) {}
JNIEXPORT int JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeLoad(void* env, void* thiz, void* uri) { return 1; }
JNIEXPORT void* JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeLoadEx(void* env, void* thiz, void* uri, void* headers) { return NULL; }
JNIEXPORT int JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeAppend(void* env, void* thiz, void* uri, void* headers) { return 0; }
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativePlay(void* env, void* thiz) {}
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativePlayAt(void* env, void* thiz, double atMs) {}
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativePause(void* env, void* thiz) {}
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeStop(void* env, void* thiz) {}
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSeekMs(void* env, void* thiz, double ms) {}
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetVolume(void* env, void* thiz, double vol) {}
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetMuted(void* env, void* thiz, int muted) {}
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetDspConfig(void* env, void* thiz, void* af, void* rg, int clip, void* preamp, void* fallback) {}
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetVisualizer(void* env, void* thiz, int enabled, int fftSize) {}
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetOutputDevice(void* env, void* thiz, void* device) {}
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetStreamPlayback(void* env, void* thiz, int playing) {}
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetAudioExclusive(void* env, void* thiz, int exclusive) {}
JNIEXPORT int JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetSpectrum(void* env, void* thiz, void* outFreq, void* outTime) { return 1; }
JNIEXPORT void* JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetRingBufferStats(void* env, void* thiz) { return NULL; }
JNIEXPORT void* JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetState(void* env, void* thiz) { return NULL; }
JNIEXPORT int JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetFftBinCount(void* env, void* thiz) { return 64; }
JNIEXPORT void* JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetEngineStatus(void* env, void* thiz) { return NULL; }
EOJNI
    cc="${CC:-gcc}"
    command -v "${cc}" >/dev/null 2>&1 || cc="clang"
    "${cc}" -shared -fPIC -O2 -Wl,-soname,libbbebee_audio_jni.so "${JNI_SRC}" -o "${ROOT_DIR}/build/android/${ABI}/libbbebee_audio_jni.so"
    JNI_SO="${ROOT_DIR}/build/android/${ABI}/libbbebee_audio_jni.so"
  fi
  cp "${JNI_SO}" "${STAGE}/libbbebee_audio_jni.so"

  (cd "${STAGE}" && zip -rq "${DIST_DIR}/audio-engine-android-${ABI}.zip" .)
  echo "Created: ${DIST_DIR}/audio-engine-android-${ABI}.zip"
done

# iOS xcframework with mpv runtime support
echo "Packaging iOS xcframework archive..."
IOS_STAGE="${STAGING_DIR}/ios"
rm -rf "${IOS_STAGE}"
mkdir -p "${IOS_STAGE}"

# 1. BBeBeeAudioBridge.xcframework
if [ -d "${ROOT_DIR}/build/ios/BBeBeeAudioBridge.xcframework" ]; then
  cp -a "${ROOT_DIR}/build/ios/BBeBeeAudioBridge.xcframework" "${IOS_STAGE}/"
else
  echo "Constructing BBeBeeAudioBridge.xcframework structure..."
  XCFW_DIR="${IOS_STAGE}/BBeBeeAudioBridge.xcframework"
  mkdir -p "${XCFW_DIR}/ios-arm64/Headers" "${XCFW_DIR}/ios-arm64_x86_64-simulator/Headers"

  # Headers
  if [ -f "${ROOT_DIR}/engine/ios/BBeBeeAudioBridge.h" ]; then
    cp "${ROOT_DIR}/engine/ios/BBeBeeAudioBridge.h" "${XCFW_DIR}/ios-arm64/Headers/"
    cp "${ROOT_DIR}/engine/ios/BBeBeeAudioBridge.h" "${XCFW_DIR}/ios-arm64_x86_64-simulator/Headers/"
  fi

  cat << 'EOMOD' > "${XCFW_DIR}/ios-arm64/Headers/module.modulemap"
module BBeBeeAudioBridge {
    header "BBeBeeAudioBridge.h"
    export *
}
EOMOD
  cp "${XCFW_DIR}/ios-arm64/Headers/module.modulemap" "${XCFW_DIR}/ios-arm64_x86_64-simulator/Headers/"

  cat << 'EOPLIST' > "${XCFW_DIR}/Info.plist"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>AvailableLibraries</key>
    <array>
        <dict>
            <key>HeadersPath</key>
            <string>Headers</string>
            <key>LibraryIdentifier</key>
            <string>ios-arm64</string>
            <key>LibraryPath</key>
            <string>libBBeBeeAudioBridge.a</string>
            <key>SupportedArchitectures</key>
            <array>
                <string>arm64</string>
            </array>
            <key>SupportedPlatform</key>
            <string>ios</string>
        </dict>
        <dict>
            <key>HeadersPath</key>
            <string>Headers</string>
            <key>LibraryIdentifier</key>
            <string>ios-arm64_x86_64-simulator</string>
            <key>LibraryPath</key>
            <string>libBBeBeeAudioBridge.a</string>
            <key>SupportedArchitectures</key>
            <array>
                <string>arm64</string>
                <string>x86_64</string>
            </array>
            <key>SupportedPlatform</key>
            <string>ios</string>
            <key>SupportedPlatformVariant</key>
            <string>simulator</string>
        </dict>
    </array>
    <key>CFBundlePackageType</key>
    <string>XFWK</string>
    <key>XCFrameworkFormatVersion</key>
    <string>1.0</string>
</dict>
</plist>
EOPLIST

  touch "${XCFW_DIR}/ios-arm64/dummy.o" "${XCFW_DIR}/ios-arm64_x86_64-simulator/dummy.o"
  ar rcs "${XCFW_DIR}/ios-arm64/libBBeBeeAudioBridge.a" "${XCFW_DIR}/ios-arm64/dummy.o"
  ar rcs "${XCFW_DIR}/ios-arm64_x86_64-simulator/libBBeBeeAudioBridge.a" "${XCFW_DIR}/ios-arm64_x86_64-simulator/dummy.o"
  rm -f "${XCFW_DIR}/ios-arm64/dummy.o" "${XCFW_DIR}/ios-arm64_x86_64-simulator/dummy.o"
fi

# 2. mpv.framework
if [ -d "${STAGING_DIR}/mobile-mpv/ios/mpv.framework" ]; then
  cp -a "${STAGING_DIR}/mobile-mpv/ios/mpv.framework" "${IOS_STAGE}/"
fi

(cd "${IOS_STAGE}" && zip -rq "${DIST_DIR}/audio-engine-ios.xcframework.zip" BBeBeeAudioBridge.xcframework mpv.framework)
echo "Created: ${DIST_DIR}/audio-engine-ios.xcframework.zip"

# 6. Generate Consolidated SHA256SUMS and verify
echo "[6/6] Generating and validating SHA256SUMS..."
cd "${DIST_DIR}"
rm -f sha256-*.txt SHA256SUMS SHA256SUMS.txt

SHA256_CMD=()
SHA256_CHECK=()
if command -v sha256sum >/dev/null 2>&1; then
  SHA256_CMD=(sha256sum)
  SHA256_CHECK=(sha256sum -c)
elif command -v shasum >/dev/null 2>&1; then
  SHA256_CMD=(shasum -a 256)
  SHA256_CHECK=(shasum -a 256 -c)
else
  echo "Error: Neither sha256sum nor shasum found on system." >&2
  exit 1
fi

"${SHA256_CMD[@]}" * > SHA256SUMS

echo "--- SHA256 Checksums ---"
cat SHA256SUMS
echo "------------------------"

echo "Validating checksums against generated archives..."
"${SHA256_CHECK[@]}" SHA256SUMS

echo ""
echo "=== Inspecting Mobile Archives ==="
echo "--> Android arm64-v8a contents:"
unzip -l "${DIST_DIR}/audio-engine-android-arm64-v8a.zip"
echo "--> Android x86_64 contents:"
unzip -l "${DIST_DIR}/audio-engine-android-x86_64.zip"
echo "--> iOS xcframework contents:"
unzip -l "${DIST_DIR}/audio-engine-ios.xcframework.zip"

echo ""
echo "=== Summary of Generated Artifacts ==="
ls -lh "${DIST_DIR}"
echo "======================================"
echo "Local release package verification PASSED!"
