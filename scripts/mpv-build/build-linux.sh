#!/usr/bin/env bash
set -euo pipefail

MPV_VERSION="${1:-v0.41.0}"
# B1: peeled commit for MPV_VERSION (from `git ls-remote` refs/tags/<tag>^{}).
# A shallow `git clone --branch <tag>` checks out the peeled commit, so
# HEAD must equal this; a moved tag fails the build instead of silently
# building different code. Verified before any of the source runs.
MPV_COMMIT="41f6a645068483470267271e1d09966ca3b9f413"
verify_git_head() {
  local src_dir="$1"
  local expected="$2"
  local head
  head="$(git -C "${src_dir}" rev-parse HEAD)"
  if [ "${head}" != "${expected}" ]; then
    echo "ERROR: ${src_dir} HEAD ${head} != pinned ${expected} for its tag (tag may have moved)" >&2
    exit 1
  fi
}
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
verify_git_head "${WORK_DIR}/mpv-src" "${MPV_COMMIT}"

cd "${WORK_DIR}/mpv-src"

echo "Applying PCM tap patch..."
# A5: the first attempt's failure output used to be swallowed by
# 2>/dev/null, so a patch drift after an mpv bump left CI with no
# diagnosis. Print it before falling back to CRLF conversion.
if git apply --ignore-whitespace --ignore-space-change "${PATCH_FILE}" > "${WORK_DIR}/patch.log" 2>&1; then
    echo "Patch applied cleanly."
else
    echo "First patch attempt failed (log follows); retrying with CRLF conversion..."
    cat "${WORK_DIR}/patch.log"
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
    # A4: this used to be a warning followed by a "successfully generated"
    # message, shipping a closure that cannot load on machines without a
    # system ffmpeg. CI installs patchelf; without it the build fails.
    echo "ERROR: patchelf not found - staged libmpv closure would NOT load on machines without system ffmpeg (install patchelf: sudo apt-get install patchelf)." >&2
    exit 1
fi

echo "=== Patched libmpv successfully generated for Linux ==="
ls -la "${TARGET_DIR}"/libmpv.so*
