#!/usr/bin/env bash
set -euo pipefail

MPV_VERSION="${1:-v0.41.0}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PATCH_FILE="${ROOT_DIR}/patches/mpv-pcm-tap.patch"
TARGET_DIR="${2:-${ROOT_DIR}/build/libmpv/linux}"
WORK_DIR="/tmp/mpv-build-linux-$$"

echo "=== Building Patched libmpv for Linux (${MPV_VERSION}) ==="
echo "Work dir: ${WORK_DIR}"
echo "Target dir: ${TARGET_DIR}"

mkdir -p "${TARGET_DIR}"
TARGET_DIR="$(cd "${TARGET_DIR}" && pwd)"
mkdir -p "${WORK_DIR}"

cleanup() {
    rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

echo "Cloning mpv repository..."
git clone --depth 1 --branch "${MPV_VERSION}" https://github.com/mpv-player/mpv.git "${WORK_DIR}/mpv-src"

cd "${WORK_DIR}/mpv-src"

echo "Applying PCM tap patch..."
if git apply --ignore-whitespace --ignore-space-change "${PATCH_FILE}" 2>/dev/null; then
    echo "Patch applied cleanly."
else
    echo "Applying patch with CRLF conversion..."
    tr -d '\r' < "${PATCH_FILE}" | git apply --ignore-whitespace --ignore-space-change -
fi

echo "Configuring build with Meson..."
meson setup build \
    --buildtype=release \
    -Ddefault_library=shared \
    -Dlibmpv=true \
    -Dcplayer=false \
    -Dpulse=enabled \
    -Dalsa=enabled \
    -Dpipewire=enabled \
    -Dc_args='-Wno-deprecated-declarations'

echo "Compiling libmpv with Ninja..."
ninja -C build

echo "Copying compiled libraries to ${TARGET_DIR}..."
mkdir -p "${TARGET_DIR}"
for f in build/libmpv.so*; do
    if [ -f "$f" ] || [ -L "$f" ]; then
        cp -P "$f" "${TARGET_DIR}/"
    fi
done

# Bundle transitive dependencies for standalone distribution across Linux distros
BUNDLE_SCRIPT="${ROOT_DIR}/scripts/bundle-mpv-deps.js"
if [ -f "${BUNDLE_SCRIPT}" ] && command -v node >/dev/null 2>&1; then
    LIBMPV_FILE="${TARGET_DIR}/libmpv.so.2"
    if [ ! -f "${LIBMPV_FILE}" ]; then
        LIBMPV_FILE="${TARGET_DIR}/libmpv.so"
    fi
    if [ -f "${LIBMPV_FILE}" ]; then
        echo "Bundling transitive dependencies into ${TARGET_DIR}..."
        node "${BUNDLE_SCRIPT}" "${LIBMPV_FILE}" "${TARGET_DIR}"
    fi
fi

# Stamp $ORIGIN rpath on every staged library so the dependency closure
# resolves on clean target machines. A dlopen'd libmpv resolves its NEEDED
# entries via its OWN rpath — the loading binary's rpath does not apply —
# so without this the staged sibling .so files are invisible and dlopen
# fails on any distro without a system ffmpeg. Any build-tree absolute rpath
# left by meson is meaningless on targets, hence --set-rpath (replace).
if command -v patchelf >/dev/null 2>&1; then
    echo "Setting \$ORIGIN rpath on staged libraries in ${TARGET_DIR}..."
    patched=0
    while IFS= read -r -d '' lib; do
        existing="$(patchelf --print-rpath "${lib}" 2>/dev/null || true)"
        case "${existing}" in
            *'$ORIGIN'*)
                echo "  rpath already contains \$ORIGIN: $(basename "${lib}")"
                ;;
            *)
                patchelf --set-rpath '$ORIGIN' "${lib}"
                echo "  rpath -> \$ORIGIN: $(basename "${lib}")"
                patched=$((patched + 1))
                ;;
        esac
    done < <(find "${TARGET_DIR}" -maxdepth 1 -type f -name '*.so*' -print0)
    echo "Patched rpath on ${patched} libraries."
else
    echo "WARNING: patchelf not found - staged libmpv closure will NOT load on machines without system ffmpeg (install patchelf: sudo apt-get install patchelf)." >&2
fi

echo "=== Patched libmpv successfully generated for Linux ==="
ls -la "${TARGET_DIR}"/libmpv.so*
