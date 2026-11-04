#!/usr/bin/env bash
# Run inside MSYS2 UCRT64 environment (or Git Bash configured with a
# ucrt64 toolchain). MINGW64 is deprecated by MSYS2 since 2026-03-15 and
# its leaf packages (notably luajit) are being removed, so the CI job
# installs the mingw-w64-ucrt-x86_64- package set instead.
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
TARGET_DIR="${2:-${ROOT_DIR}/build/libmpv/win64}"
WORK_DIR="/tmp/mpv-build-win64-$$"

echo "=== Building Patched libmpv for Windows x86_64 (${MPV_VERSION}) ==="
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
    -Dwasapi=enabled \
    -Dlua=disabled \
    -Dc_args='-Wno-deprecated-declarations'
# -Dlua=disabled is load-bearing, not cosmetic: the engine talks to
# libmpv only through the C API and never loads mpv scripts, so Lua
# buys nothing here — and luajit no longer exists as a MINGW64 package
# in MSYS2, so leaving lua on "auto" would fail the CI install step
# before meson even runs. Android/iOS already build with lua disabled.

echo "Compiling libmpv with Ninja..."
ninja -C build

echo "Copying compiled libmpv library to ${TARGET_DIR}..."
mkdir -p "${TARGET_DIR}"
DLL_SRC="$(ls build/*mpv*.dll 2>/dev/null | head -n 1)"
if [ -n "${DLL_SRC}" ]; then
    cp -P "${DLL_SRC}" "${TARGET_DIR}/libmpv-2.dll"
else
    echo "Error: No mpv dll found in build directory" >&2
    exit 1
fi

# Bundle transitive MinGW DLL dependencies for standalone distribution
bundle_windows_deps() {
    local target_dir="$1"
    echo "=== Bundling Windows transitive dependencies into ${target_dir} ==="

    local search_dirs=()
    if [ -n "${MINGW_PREFIX:-}" ]; then
        search_dirs+=("${MINGW_PREFIX}/bin")
    fi
    search_dirs+=("/ucrt64/bin" "/c/msys64/ucrt64/bin" "/mingw64/bin" "/c/msys64/mingw64/bin")

    # Include dirs from PATH
    IFS=':' read -r -a path_dirs <<< "${PATH:-}"
    for p in "${path_dirs[@]}"; do
        if [ -d "$p" ]; then
            search_dirs+=("$p")
        fi
    done

    local processed=()
    local queue=()
    local missing_deps=()
    for f in "${target_dir}"/*.dll; do
        if [ -f "$f" ]; then
            queue+=("$f")
        fi
    done

    while [ ${#queue[@]} -gt 0 ]; do
        local current="${queue[0]}"
        queue=("${queue[@]:1}")

        local current_base
        current_base="$(basename "$current")"
        local current_lower
        current_lower="$(echo "$current_base" | tr '[:upper:]' '[:lower:]')"

        local already=0
        for p in "${processed[@]}"; do
            if [ "$p" = "$current_lower" ]; then
                already=1
                break
            fi
        done
        [ "$already" -eq 1 ] && continue
        processed+=("$current_lower")

        echo "Tracing dependencies for: ${current_base}"

        local deps
        deps=$(objdump -p "$current" 2>/dev/null | grep -i "DLL Name:" | awk '{print $3}' || true)
        for dep in $deps; do
            local dep_lower
            dep_lower="$(echo "$dep" | tr '[:upper:]' '[:lower:]')"
            case "$dep_lower" in
                kernel32.dll|kernelbase.dll|user32.dll|gdi32.dll|gdi32full.dll|advapi32.dll|\
                shell32.dll|ole32.dll|oleaut32.dll|msvcrt.dll|ucrtbase.dll|ntdll.dll|\
                ws2_32.dll|wsock32.dll|imm32.dll|opengl32.dll|version.dll|shcore.dll|\
                shlwapi.dll|uxtheme.dll|avrt.dll|dwmapi.dll|bcrypt.dll|crypt32.dll|\
                secur32.dll|setupapi.dll|winmm.dll|rpcrt4.dll|comctl32.dll|comdlg32.dll|\
                d3d9.dll|d3d11.dll|dxgi.dll|cfgmgr32.dll|iphlpapi.dll|netapi32.dll|\
                userenv.dll|wldap32.dll|dnsapi.dll|mpr.dll|powrprof.dll|winhttp.dll|\
                api-ms-win-*|ext-ms-win-*)
                    continue
                    ;;
            esac

            local already_in_target=0
            if [ -f "${target_dir}/${dep}" ]; then
                already_in_target=1
            else
                local staged_match
                staged_match=$(find "$target_dir" -maxdepth 1 -iname "$dep" 2>/dev/null | head -n 1)
                if [ -n "$staged_match" ] && [ -f "$staged_match" ]; then
                    already_in_target=1
                fi
            fi
            [ "$already_in_target" -eq 1 ] && continue

            local found=""
            for sdir in "${search_dirs[@]}"; do
                [ ! -d "$sdir" ] && continue
                if [ -f "${sdir}/${dep}" ]; then
                    found="${sdir}/${dep}"
                    break
                else
                    local match
                    match=$(find "$sdir" -maxdepth 1 -iname "$dep" 2>/dev/null | head -n 1)
                    if [ -n "$match" ] && [ -f "$match" ]; then
                        found="$match"
                        break
                    fi
                fi
            done

            if [ -n "$found" ]; then
                echo "  Bundling Windows dep: ${dep} (from ${found})"
                cp -P "$found" "${target_dir}/${dep}"
                chmod 755 "${target_dir}/${dep}" 2>/dev/null || true
                queue+=("${target_dir}/${dep}")
            else
                # A4: this used to be a warning followed by a success
                # message, shipping an archive that fails to load.
                echo "  ERROR: Windows dependency not found: ${dep}" >&2
                missing_deps+=("${dep}")
            fi
        done
    done
    if [ "${#missing_deps[@]}" -gt 0 ]; then
        echo "ERROR: ${#missing_deps[@]} Windows dependencies could not be bundled: ${missing_deps[*]}" >&2
        return 1
    fi
    echo "=== Windows dependency bundling complete ==="
}

bundle_windows_deps "${TARGET_DIR}"

echo "=== Patched libmpv successfully generated for Windows ==="
ls -la "${TARGET_DIR}/"
