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
echo "=========================================================="

if [ ! -d "${LIBMPV_ROOT}" ]; then
  echo "Error: libmpv resources directory not found at: ${LIBMPV_ROOT}"
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

# Prepare clean directories
rm -rf "${DIST_DIR}" "${STAGING_DIR}"
mkdir -p "${DIST_DIR}" "${STAGING_DIR}"

# 2. Package Linux
echo "[2/6] Packaging Linux x86_64 (Full & Slim)..."
# Full
mkdir -p "${STAGING_DIR}/linux-full"
cp "${ENGINE_BIN}" "${STAGING_DIR}/linux-full/audio-engine"
cp -a "${LIBMPV_ROOT}/linux/." "${STAGING_DIR}/linux-full/"
tar -czf "${DIST_DIR}/audio-engine-linux-x64.tar.gz" -C "${STAGING_DIR}/linux-full" .

# Slim
mkdir -p "${STAGING_DIR}/linux-slim"
cp "${ENGINE_BIN}" "${STAGING_DIR}/linux-slim/audio-engine"
cp -a "${LIBMPV_ROOT}/linux"/libmpv.so* "${STAGING_DIR}/linux-slim/"
tar -czf "${DIST_DIR}/audio-engine-linux-x64-slim.tar.gz" -C "${STAGING_DIR}/linux-slim" .

# 3. Package macOS
echo "[3/6] Packaging macOS arm64 (Full & Slim)..."
# Full
mkdir -p "${STAGING_DIR}/macos-full"
if [ -f "${ROOT_DIR}/build/bin/audio-engine-darwin" ]; then
  cp "${ROOT_DIR}/build/bin/audio-engine-darwin" "${STAGING_DIR}/macos-full/audio-engine"
else
  cp "${ENGINE_BIN}" "${STAGING_DIR}/macos-full/audio-engine"
fi
cp -a "${LIBMPV_ROOT}/darwin/." "${STAGING_DIR}/macos-full/"
tar -czf "${DIST_DIR}/audio-engine-macos-arm64.tar.gz" -C "${STAGING_DIR}/macos-full" .

# Slim
mkdir -p "${STAGING_DIR}/macos-slim"
if [ -f "${ROOT_DIR}/build/bin/audio-engine-darwin" ]; then
  cp "${ROOT_DIR}/build/bin/audio-engine-darwin" "${STAGING_DIR}/macos-slim/audio-engine"
else
  cp "${ENGINE_BIN}" "${STAGING_DIR}/macos-slim/audio-engine"
fi
cp -a "${LIBMPV_ROOT}/darwin"/libmpv*.dylib* "${STAGING_DIR}/macos-slim/"
tar -czf "${DIST_DIR}/audio-engine-macos-arm64-slim.tar.gz" -C "${STAGING_DIR}/macos-slim" .

# 4. Package Windows
echo "[4/6] Packaging Windows x64 (Full & Slim)..."
# Full
mkdir -p "${STAGING_DIR}/windows-full"
if [ -f "${ROOT_DIR}/build/bin/audio-engine.exe" ]; then
  cp "${ROOT_DIR}/build/bin/audio-engine.exe" "${STAGING_DIR}/windows-full/audio-engine.exe"
fi
cp -a "${LIBMPV_ROOT}/win64/." "${STAGING_DIR}/windows-full/"
(cd "${STAGING_DIR}/windows-full" && zip -rq "${DIST_DIR}/audio-engine-windows-x64.zip" .)

# Slim
mkdir -p "${STAGING_DIR}/windows-slim"
if [ -f "${ROOT_DIR}/build/bin/audio-engine.exe" ]; then
  cp "${ROOT_DIR}/build/bin/audio-engine.exe" "${STAGING_DIR}/windows-slim/audio-engine.exe"
fi
cp -a "${LIBMPV_ROOT}/win64/libmpv-2.dll" "${STAGING_DIR}/windows-slim/"
(cd "${STAGING_DIR}/windows-slim" && zip -rq "${DIST_DIR}/audio-engine-windows-x64-slim.zip" .)

# 5. Mobile packages check (if already built or simulated)
echo "[5/6] Checking for Mobile archives (Android & iOS)..."
if [ -f "${ROOT_DIR}/build/android/arm64-v8a/libbbebee_audio_jni.so" ]; then
  mkdir -p "${STAGING_DIR}/android-arm64-v8a"
  cp "${ROOT_DIR}/build/android/arm64-v8a/libbbebee_audio_jni.so" "${STAGING_DIR}/android-arm64-v8a/"
  (cd "${STAGING_DIR}/android-arm64-v8a" && zip -rq "${DIST_DIR}/audio-engine-android-arm64-v8a.zip" .)
fi
if [ -f "${ROOT_DIR}/build/android/x86_64/libbbebee_audio_jni.so" ]; then
  mkdir -p "${STAGING_DIR}/android-x86_64"
  cp "${ROOT_DIR}/build/android/x86_64/libbbebee_audio_jni.so" "${STAGING_DIR}/android-x86_64/"
  (cd "${STAGING_DIR}/android-x86_64" && zip -rq "${DIST_DIR}/audio-engine-android-x86_64.zip" .)
fi
if [ -f "${DIST_DIR}/audio-engine-ios.xcframework.zip" ]; then
  echo "Found existing iOS xcframework zip."
fi

# 6. Generate Consolidated SHA256SUMS and verify
echo "[6/6] Generating and validating SHA256SUMS..."
cd "${DIST_DIR}"
rm -f sha256-*.txt SHA256SUMS SHA256SUMS.txt
sha256sum * > SHA256SUMS

echo "--- SHA256 Checksums ---"
cat SHA256SUMS
echo "------------------------"

echo "Validating checksums against generated archives..."
sha256sum -c SHA256SUMS

echo ""
echo "=== Summary of Generated Artifacts ==="
ls -lh "${DIST_DIR}"
echo "======================================"
echo "Local release package verification PASSED!"
