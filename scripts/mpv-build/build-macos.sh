#!/usr/bin/env bash
set -euo pipefail

MPV_VERSION="${1:-v0.41.0}"
# B1: peeled commit for MPV_VERSION, same provenance and rule as build-linux.sh.
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
TARGET_DIR="${2:-${ROOT_DIR}/build/libmpv/darwin}"
WORK_DIR="/tmp/mpv-build-macos-$$"

echo "=== Building Patched libmpv for macOS (${MPV_VERSION}) ==="
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
# A5: see build-linux.sh — print the first attempt's failure before
# falling back to CRLF conversion, instead of swallowing it.
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
    -Dcoreaudio=enabled \
    -Dc_args='-Wno-deprecated-declarations -DGL_SILENCE_DEPRECATION'

echo "Compiling libmpv with Ninja..."
ninja -C build

echo "Copying compiled libraries to ${TARGET_DIR}..."
mkdir -p "${TARGET_DIR}"
for f in build/libmpv*.dylib*; do
    if [ -f "$f" ] || [ -L "$f" ]; then
        cp -P "$f" "${TARGET_DIR}/"
    fi
done

# Bundle transitive dylib dependencies and rewrite references to @rpath
bundle_macos_deps() {
    local target_dir="$1"
    echo "=== Bundling macOS transitive dependencies into ${target_dir} ==="

    local processed=()
    local queue=()
    for f in "${target_dir}"/libmpv*.dylib; do
        if [ -f "$f" ] && [ ! -L "$f" ]; then
            queue+=("$f")
        fi
    done

    while [ ${#queue[@]} -gt 0 ]; do
        local current="${queue[0]}"
        queue=("${queue[@]:1}")

        local current_base
        current_base="$(basename "$current")"

        local already=0
        for p in "${processed[@]}"; do
            if [ "$p" = "$current_base" ]; then
                already=1
                break
            fi
        done
        [ "$already" -eq 1 ] && continue
        processed+=("$current_base")

        echo "Tracing dependencies for: ${current_base}"
        chmod u+w "$current" 2>/dev/null || true
        install_name_tool -id "@rpath/${current_base}" "$current" 2>/dev/null || true

        while IFS= read -r line; do
            local dep_path
            dep_path="$(echo "$line" | awk '{print $1}')"
            [ -z "$dep_path" ] && continue

            case "$dep_path" in
                "$current"|"$current_base"|@rpath/*|@loader_path/*|@executable_path/*|/usr/lib/*|/System/*)
                    continue
                    ;;
            esac

            local dep_base
            dep_base="$(basename "$dep_path")"
            [ "$dep_base" = "$current_base" ] && continue

            local staged_copy="${target_dir}/${dep_base}"
            if [ ! -f "$staged_copy" ]; then
                local real_src="$dep_path"
                if [[ "$dep_path" == @rpath/* ]]; then
                    for rpath_candidate in "/opt/homebrew/lib/${dep_base}" "/usr/local/lib/${dep_base}" "/opt/homebrew/opt"/*"/lib/${dep_base}"; do
                        if [ -f "$rpath_candidate" ]; then
                            real_src="$rpath_candidate"
                            break
                        fi
                    done
                elif [ -L "$real_src" ]; then
                    real_src="$(python3 -c "import os, sys; print(os.path.realpath(sys.argv[1]))" "$real_src" 2>/dev/null || readlink -f "$real_src" || echo "$real_src")"
                fi

                if [ -f "$real_src" ]; then
                    echo "  Bundling macOS dep: ${dep_base} (from ${real_src})"
                    cp -P "$real_src" "$staged_copy"
                    chmod 755 "$staged_copy" 2>/dev/null || true
                    chmod u+w "$staged_copy" 2>/dev/null || true
                    install_name_tool -id "@rpath/${dep_base}" "$staged_copy" 2>/dev/null || true
                    queue+=("$staged_copy")
                else
                    echo "  Warning: macOS dependency not found: ${dep_path}"
                fi
            fi

            install_name_tool -change "$dep_path" "@rpath/${dep_base}" "$current" 2>/dev/null || true
        done < <(otool -L "$current" 2>/dev/null | tail -n +2)
    done

    echo "Configuring RPATH and re-signing all dylibs in ${target_dir}..."
    for f in "${target_dir}"/*.dylib; do
        if [ -f "$f" ] && [ ! -L "$f" ]; then
            install_name_tool -add_rpath "@loader_path" "$f" 2>/dev/null || true
            install_name_tool -add_rpath "@executable_path" "$f" 2>/dev/null || true
            # A4: a failed ad-hoc sign used to be swallowed by `|| true`
            # while the script still reported success. Verify the result
            # and fail instead of shipping an unsigned/unsignable dylib.
            if ! codesign --force --deep --sign - "$f" 2>/dev/null; then
                echo "ERROR: codesign failed for ${f}" >&2
                return 1
            fi
        fi
    done
    echo "=== macOS dependency bundling complete ==="
}

bundle_macos_deps "${TARGET_DIR}"

echo "=== Patched libmpv successfully generated for macOS ==="
ls -la "${TARGET_DIR}/"
