#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

PLATFORM="${1:-auto}"
MPV_VERSION="${2:-v0.41.0}"
TARGET_DIR="${3:-}"

if [ -n "${TARGET_DIR}" ]; then
  mkdir -p "${TARGET_DIR}"
  TARGET_DIR="$(cd "${TARGET_DIR}" && pwd)"
fi

if [ "${PLATFORM}" = "auto" ]; then
  OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
  case "${OS}" in
    linux*) PLATFORM="linux" ;;
    darwin*) PLATFORM="macos" ;;
    msys*|mingw*|cygwin*) PLATFORM="windows" ;;
    *) echo "Unknown operating system: ${OS}" >&2; exit 1 ;;
  esac
fi

echo "=== Building Patched libmpv for ${PLATFORM} (${MPV_VERSION}) ==="

case "${PLATFORM}" in
  linux)
    if [ -n "${TARGET_DIR}" ]; then
      bash "${ROOT_DIR}/scripts/mpv-build/build-linux.sh" "${MPV_VERSION}" "${TARGET_DIR}"
    else
      bash "${ROOT_DIR}/scripts/mpv-build/build-linux.sh" "${MPV_VERSION}"
    fi
    ;;
  macos|darwin)
    if [ -n "${TARGET_DIR}" ]; then
      bash "${ROOT_DIR}/scripts/mpv-build/build-macos.sh" "${MPV_VERSION}" "${TARGET_DIR}"
    else
      bash "${ROOT_DIR}/scripts/mpv-build/build-macos.sh" "${MPV_VERSION}"
    fi
    ;;
  windows|win64)
    if [ -n "${TARGET_DIR}" ]; then
      bash "${ROOT_DIR}/scripts/mpv-build/build-windows.sh" "${MPV_VERSION}" "${TARGET_DIR}"
    else
      bash "${ROOT_DIR}/scripts/mpv-build/build-windows.sh" "${MPV_VERSION}"
    fi
    ;;
  *)
    echo "Error: Unsupported platform '${PLATFORM}'. Supported: linux, macos, windows." >&2
    exit 1
    ;;
esac
