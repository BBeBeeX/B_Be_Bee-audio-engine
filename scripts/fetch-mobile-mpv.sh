#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

PLATFORM="${1:-all}"
OUTPUT_DIR="${2:-${ROOT_DIR}/staging/mobile-mpv}"
CACHE_DIR="${MOBILE_MPV_CACHE_DIR:-${ROOT_DIR}/.cache/mobile-mpv}"

echo "=========================================================="
echo " BBeBee Mobile libmpv Fetcher & Integrity Verifier"
echo "=========================================================="
echo "Platform target: ${PLATFORM}"
echo "Output dir:      ${OUTPUT_DIR}"
echo "Cache dir:       ${CACHE_DIR}"
echo "=========================================================="

mkdir -p "${CACHE_DIR}" "${OUTPUT_DIR}"

verify_real_android_so() {
  local so_file="$1"
  local abi="$2"

  if [ ! -f "${so_file}" ]; then
    return 1
  fi

  # Check that binary is authentic by verifying exported symbol count
  local sym_count=0
  if command -v llvm-nm >/dev/null 2>&1; then
    sym_count=$(llvm-nm -D --defined-only "${so_file}" 2>/dev/null | wc -l || echo 0)
  fi
  if [ "${sym_count}" -lt 50 ] && command -v readelf >/dev/null 2>&1; then
    sym_count=$(readelf -W -s "${so_file}" 2>/dev/null | grep -E "GLOBAL|WEAK" | wc -l || echo 0)
  fi
  if [ "${sym_count}" -lt 50 ] && command -v nm >/dev/null 2>&1; then
    sym_count=$(nm -D --defined-only "${so_file}" 2>/dev/null | wc -l || echo 0)
  fi

  if [ "${sym_count}" -lt 50 ]; then
    echo "ERROR: ${so_file} appears to be an invalid library (${sym_count} symbols)." >&2
    return 1
  fi

  # Verify critical core client API and PCM tap
  # NOTE: with `set -o pipefail`, `tool | grep -q` misreports "not found":
  # grep -q exits on the first match, the producer then dies with SIGPIPE (141)
  # and the whole pipeline fails. Capture the dump into a variable first and
  # grep that instead (a herestring reads from a temp file: no pipe, no SIGPIPE).
  local has_create=0
  local sym_dump=""
  if command -v llvm-nm >/dev/null 2>&1; then
    sym_dump="$(llvm-nm -D --defined-only "${so_file}" 2>/dev/null || true)"
  elif command -v readelf >/dev/null 2>&1; then
    sym_dump="$(readelf -W -s "${so_file}" 2>/dev/null || true)"
  elif command -v nm >/dev/null 2>&1; then
    sym_dump="$(nm -D --defined-only "${so_file}" 2>/dev/null || true)"
  fi
  if grep -q "mpv_create" <<< "${sym_dump}"; then
    has_create=1
  fi

  if [ "${has_create}" -eq 0 ]; then
    echo "ERROR: ${so_file} is missing mandatory mpv_create symbol." >&2
    return 1
  fi

  return 0
}

verify_real_ios_framework() {
  local fw_dir="$1"
  local bin_file="${fw_dir}/mpv"

  if [ ! -f "${bin_file}" ]; then
    return 1
  fi

  if command -v nm >/dev/null 2>&1; then
    local sym_count=$(nm -gU "${bin_file}" 2>/dev/null | wc -l || echo 0)
    if [ "${sym_count}" -lt 50 ]; then
      sym_count=$(nm --defined-only "${bin_file}" 2>/dev/null | wc -l || echo 0)
    fi
    if [ "${sym_count}" -lt 50 ]; then
      sym_count=$(nm "${bin_file}" 2>/dev/null | grep -E " [TtSs] " | wc -l || echo 0)
    fi
    if [ "${sym_count}" -lt 50 ]; then
      echo "ERROR: ${bin_file} appears to be an invalid framework (${sym_count} symbols)." >&2
      return 1
    fi

    # Verify critical core client API
    # NOTE: with `set -o pipefail`, `tool | grep -q` misreports "not found":
    # grep -q exits on the first match, the producer then dies with SIGPIPE (141)
    # and the whole pipeline fails. Capture the dumps into variables first and
    # grep those instead (a herestring reads from a temp file: no pipe, no SIGPIPE).
    local found_create=0
    local bin_syms="$(nm -gU "${bin_file}" 2>/dev/null || true)"
    if [ -z "${bin_syms}" ]; then
      bin_syms="$(nm "${bin_file}" 2>/dev/null || true)"
    fi
    local bin_strs="$(strings "${bin_file}" 2>/dev/null || true)"
    if grep -q "mpv_create" <<< "${bin_syms}" || grep -q "mpv_create" <<< "${bin_strs}"; then
      found_create=1
    fi

    if [ "${found_create}" -eq 0 ]; then
      echo "ERROR: ${bin_file} is missing mandatory mpv_create symbol." >&2
      return 1
    fi
  fi

  return 0
}

stage_android() {
  local abi="$1"
  local target_so="${OUTPUT_DIR}/android/${abi}/libmpv.so"
  local cached_so="${CACHE_DIR}/android_${abi}_libmpv.so"
  local local_built_so="${ROOT_DIR}/build/libmpv/android/${abi}/libmpv.so"

  echo "--> Staging Android libmpv for ${abi}..."

  # 0. Check if target is already staged and authentic
  if [ -f "${target_so}" ] && verify_real_android_so "${target_so}" "${abi}"; then
    echo "Found already staged genuine Android ${abi} libmpv.so: ${target_so}"
    return 0
  fi

  # 1. Check local build output from scripts/mpv-build/build-android.sh
  if [ -f "${local_built_so}" ] && verify_real_android_so "${local_built_so}" "${abi}"; then
    mkdir -p "${OUTPUT_DIR}/android/${abi}"
    cp "${local_built_so}" "${target_so}"
    echo "Staged genuine locally built Android ${abi} libmpv.so: ${target_so}"
    return 0
  fi

  # 2. Check cached binary (must pass integrity check)
  if [ -f "${cached_so}" ]; then
    if verify_real_android_so "${cached_so}" "${abi}"; then
      mkdir -p "${OUTPUT_DIR}/android/${abi}"
      cp "${cached_so}" "${target_so}"
      echo "Staged genuine cached Android ${abi} libmpv.so: ${target_so}"
      return 0
    else
      echo "Discarding invalid cached binary: ${cached_so}"
      rm -f "${cached_so}"
    fi
  fi

  # 3. Download if custom URL provided
  local dl_url_var="MOBILE_MPV_ANDROID_${abi//-/_}_URL"
  local dl_url="${!dl_url_var:-${MOBILE_MPV_ANDROID_URL:-}}"
  if [ -n "${dl_url}" ]; then
    echo "Downloading ${abi} libmpv from ${dl_url}..."
    if curl -fSL -o "${cached_so}" "${dl_url}"; then
      if verify_real_android_so "${cached_so}" "${abi}"; then
        mkdir -p "${OUTPUT_DIR}/android/${abi}"
        cp "${cached_so}" "${target_so}"
        echo "Successfully downloaded and verified Android ${abi} libmpv.so: ${target_so}"
        return 0
      else
        rm -f "${cached_so}"
        echo "ERROR: Downloaded libmpv from ${dl_url} failed integrity verification." >&2
        exit 1
      fi
    else
      rm -f "${cached_so}"
      echo "ERROR: Failed to download libmpv from ${dl_url}." >&2
      exit 1
    fi
  fi

  # 4. Loud Fail - Zero tolerance for missing genuine binary
  echo "ERROR: Authentic libmpv.so not found for Android ${abi}!" >&2
  echo "To resolve this, either:" >&2
  echo "  1) Build genuine libmpv from source: bash scripts/mpv-build/build-android.sh ${abi}" >&2
  echo "  2) Set MOBILE_MPV_ANDROID_${abi//-/_}_URL or MOBILE_MPV_ANDROID_URL to a valid release asset." >&2
  exit 1
}

stage_ios() {
  local target_fw="${OUTPUT_DIR}/ios/mpv.framework"
  local cached_fw="${CACHE_DIR}/mpv.framework"
  local local_built_fw="${ROOT_DIR}/build/libmpv/ios/mpv.framework"

  echo "--> Staging iOS mpv.framework..."

  # 0. Check if target is already staged and authentic
  if [ -d "${target_fw}" ] && verify_real_ios_framework "${target_fw}"; then
    echo "Found already staged genuine iOS mpv.framework: ${target_fw}"
    return 0
  fi

  # 1. Check local build output from scripts/mpv-build/build-ios.sh
  if [ -d "${local_built_fw}" ] && verify_real_ios_framework "${local_built_fw}"; then
    mkdir -p "${OUTPUT_DIR}/ios"
    rm -rf "${target_fw}"
    cp -a "${local_built_fw}" "${OUTPUT_DIR}/ios/"
    echo "Staged genuine locally built iOS mpv.framework: ${target_fw}"
    return 0
  fi

  # 2. Check cached framework
  if [ -d "${cached_fw}" ]; then
    if verify_real_ios_framework "${cached_fw}"; then
      mkdir -p "${OUTPUT_DIR}/ios"
      rm -rf "${target_fw}"
      cp -a "${cached_fw}" "${OUTPUT_DIR}/ios/"
      echo "Staged genuine cached iOS mpv.framework: ${target_fw}"
      return 0
    else
      echo "Discarding invalid cached framework: ${cached_fw}"
      rm -rf "${cached_fw}"
    fi
  fi

  # 3. Download if custom URL provided
  local dl_url="${MOBILE_MPV_IOS_URL:-}"
  if [ -n "${dl_url}" ]; then
    echo "Downloading iOS mpv.framework from ${dl_url}..."
    mkdir -p "${CACHE_DIR}/ios_dl"
    if curl -fSL -o "${CACHE_DIR}/ios_dl/mpv.framework.zip" "${dl_url}"; then
      rm -rf "${CACHE_DIR}/ios_dl/extracted"
      mkdir -p "${CACHE_DIR}/ios_dl/extracted"
      unzip -q -o "${CACHE_DIR}/ios_dl/mpv.framework.zip" -d "${CACHE_DIR}/ios_dl/extracted/"
      local extracted_fw
      extracted_fw=$(find "${CACHE_DIR}/ios_dl/extracted" -maxdepth 2 -type d -name "mpv.framework" | head -n 1)
      if [ -n "${extracted_fw}" ] && verify_real_ios_framework "${extracted_fw}"; then
        rm -rf "${cached_fw}" "${target_fw}"
        cp -a "${extracted_fw}" "${cached_fw}"
        mkdir -p "${OUTPUT_DIR}/ios"
        cp -a "${cached_fw}" "${OUTPUT_DIR}/ios/"
        rm -rf "${CACHE_DIR}/ios_dl"
        echo "Successfully downloaded and verified iOS mpv.framework: ${target_fw}"
        return 0
      else
        rm -rf "${CACHE_DIR}/ios_dl"
        echo "ERROR: Downloaded iOS mpv.framework failed integrity verification." >&2
        exit 1
      fi
    else
      rm -rf "${CACHE_DIR}/ios_dl"
      echo "ERROR: Failed to download iOS mpv.framework from ${dl_url}." >&2
      exit 1
    fi
  fi

  # 4. Loud Fail - Zero tolerance for missing genuine binary
  echo "ERROR: Authentic iOS mpv.framework not found!" >&2
  echo "To resolve this, either:" >&2
  echo "  1) Build genuine mpv.framework on macOS: bash scripts/mpv-build/build-ios.sh" >&2
  echo "  2) Set MOBILE_MPV_IOS_URL to a valid release asset URL." >&2
  exit 1
}

case "${PLATFORM}" in
  android)
    stage_android "arm64-v8a"
    stage_android "x86_64"
    ;;
  ios)
    stage_ios
    ;;
  all)
    stage_android "arm64-v8a"
    stage_android "x86_64"
    stage_ios
    ;;
  *)
    echo "Unknown platform: ${PLATFORM}. Valid options: all, android, ios" >&2
    exit 1
    ;;
esac

# Generate SHA256SUMS for staged mobile artifacts
echo "Calculating SHA256 integrity checksums..."
CHECKSUMS_FILE="${OUTPUT_DIR}/SHA256SUMS.mobile"

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

(
  cd "${OUTPUT_DIR}"
  find . -type f ! -name "SHA256SUMS*" -exec "${SHA256_CMD[@]}" {} + | sort -k 2 > "${CHECKSUMS_FILE}" || true
)

if [ -s "${CHECKSUMS_FILE}" ]; then
  echo "--- Mobile mpv Checksums ---"
  cat "${CHECKSUMS_FILE}"
  echo "----------------------------"

  echo "Validating mobile checksum integrity..."
  (cd "${OUTPUT_DIR}" && "${SHA256_CHECK[@]}" "${CHECKSUMS_FILE}")
  echo "Mobile mpv artifacts successfully staged and verified at ${OUTPUT_DIR}!"
else
  rm -f "${CHECKSUMS_FILE}"
  echo "ERROR: No mobile libmpv runtime binaries staged. Failing build." >&2
  exit 1
fi
