#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BUILD_TYPE="${1:-Release}"

echo "=== Building audio-engine from source (${BUILD_TYPE}) ==="
mkdir -p "${BUILD_DIR}"

if command -v cmake >/dev/null 2>&1; then
  echo "Using CMake to build audio-engine..."
  cmake -B "${BUILD_DIR}" -S "${ROOT_DIR}/engine" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
  cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  echo "=== Build finished successfully. Binaries located in ${BUILD_DIR} ==="
else
  echo "CMake not found, falling back to direct compiler invocation..."
  mkdir -p "${BUILD_DIR}/bin"
  CXX="${CXX:-g++}"
  OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
  RPATH_FLAG="-Wl,-rpath,\$ORIGIN"
  if [[ "${OS}" == *"darwin"* ]]; then
    RPATH_FLAG="-Wl,-rpath,@executable_path -Wl,-rpath,@loader_path"
  fi
  # FFmpeg (libav*) for the "transcode" action; optional — the engine builds
  # without it and the action reports "not supported" at runtime.
  FFMPEG_CFLAGS="$(pkg-config --cflags libavformat libavcodec libavutil libswresample 2>/dev/null || true)"
  FFMPEG_LIBS="$(pkg-config --libs libavformat libavcodec libavutil libswresample 2>/dev/null || true)"
  FFMPEG_DEFS=""
  if [[ -n "${FFMPEG_CFLAGS}" ]]; then
    FFMPEG_DEFS="-DHAVE_LIBAV"
    echo "FFmpeg found: transcode action enabled"
  else
    echo "FFMPEG not found: transcode action will report unsupported at runtime"
  fi
  ${CXX} -O2 -std=c++17 -I"${ROOT_DIR}/engine" \
    "${ROOT_DIR}/engine/main.cpp" \
    "${ROOT_DIR}/engine/pcm_ring_buffer.cpp" \
    "${ROOT_DIR}/engine/transcode.cpp" \
    ${FFMPEG_DEFS} ${FFMPEG_CFLAGS} \
    -o "${BUILD_DIR}/bin/audio-engine" \
    -pthread -ldl ${FFMPEG_LIBS} ${RPATH_FLAG}
  echo "=== Build finished: ${BUILD_DIR}/bin/audio-engine ==="
fi
