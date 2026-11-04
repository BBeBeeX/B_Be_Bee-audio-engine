#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# BBeBee Audio Engine - iOS Real mpv Cross-Compilation Script
# Targets: device (arm64), simulator (arm64, x86_64)
# Toolchain: Apple Clang / Xcode SDK (iOS 16.0+)
# Audio Backends: AudioUnit / CoreAudio
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TARGET_DIR="${1:-${ROOT_DIR}/staging/mobile-mpv/ios/mpv.framework}"
PATCH_FILE="${ROOT_DIR}/patches/mpv-pcm-tap.patch"

# A3: same mpv override as build-android.sh (see there), and the
# framework version follows the repo-root VERSION file instead of a
# second hardcoded number that had already drifted to 1.2.0 vs tag 1.3.0.
MPV_VERSION="${MPV_VERSION:-v0.41.0}"
FFMPEG_VERSION="n6.1.1"
FRAMEWORK_VERSION="${FRAMEWORK_VERSION:-$(cat "${ROOT_DIR}/VERSION" 2>/dev/null | tr -d '[:space:]')}"
[ -n "${FRAMEWORK_VERSION}" ] || FRAMEWORK_VERSION="1.2.0"
LIBPLACEBO_VERSION="v6.338.2"
LIBASS_VERSION="0.17.2"
FREETYPE_VERSION="VER-2-13-2"
FRIBIDI_VERSION="v1.0.14"
HARFBUZZ_VERSION="8.3.0"
# iOS transcode: external encoder libraries, same versions as
# build-android.sh (the transcode matrix is shared across platforms, so
# the encoder backends must be too). These are built BEFORE FFmpeg's
# configure runs — FFmpeg only enables libmp3lame/libvorbis/libopus
# when their headers are discoverable at configure time.
#
# Provenance differs from Android on purpose: Android clones the xiph
# git tags and runs autogen.sh (its workflow installs autoconf,
# automake, libtool). iOS uses the xiph release tarballs instead,
# pinned by sha256 below — release tarballs ship a pre-generated
# ./configure, while the git tags ship only configure.ac, and the
# git-based step died on Macs without autoconf (autoreconf: command
# not found, exit 127). No machine building iOS needs autotools now.
LAME_VERSION="3.100"
LAME_SHA256="ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e"
LIBOGG_VERSION="1.3.5"
LIBOGG_SHA256="0eb4b4b9420a0f51db142ba3f9c64b333f826532dc0f48c6410ae51f4799b664"
LIBVORBIS_VERSION="1.3.7"
LIBVORBIS_SHA256="0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab"
LIBOPUS_VERSION="1.4"
LIBOPUS_SHA256="c9b32b4253be5ae63d1ff16eea06b94b5f0f2951b7a02aceef58e3a3ce49c51f"
# Same selective encoder/muxer set as Android: every entry of
# resolveTranscodeTarget() must actually resolve, nothing more.
TRANSCODE_ENCODERS="aac alac flac pcm_s16le pcm_s24le pcm_f32le wmav2 libmp3lame libvorbis libopus"
TRANSCODE_MUXERS="adts asf flac ipod mp4 mp3 ogg opus wav"
# B1: peeled commits, same provenance as build-android.sh (see there).
MPV_COMMIT="41f6a645068483470267271e1d09966ca3b9f413"
FFMPEG_COMMIT="e38092ef9395d7049f871ef4d5411eb410e283e0"
FREETYPE_COMMIT="920c5502cc3ddda88f6c7d85ee834ac611bb11cc"
FRIBIDI_COMMIT="bca04dc3cd3af85a9d9220c430737333634d622a"
HARFBUZZ_COMMIT="894a1f72ee93a1fd8dc1d9218cb3fd8f048be29a"
LIBASS_COMMIT="cbb48cc4f2f076300004b8b06a86bec55281d0c2"
LIBPLACEBO_COMMIT="64c1954570f1cd57f8570a57e51fb0249b57bb90"
# B1: a tag is movable, so "clone --branch <tag>" alone proves nothing —
# the checked-out HEAD must equal the pinned peeled commit, verified
# before any of the source's code runs.
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

verify_sha256() {
  local file="$1"
  local expected="$2"
  local actual=""
  if command -v sha256sum >/dev/null 2>&1; then
    actual="$(sha256sum "${file}" | awk '{print $1}')"
  elif command -v shasum >/dev/null 2>&1; then
    actual="$(shasum -a 256 "${file}" | awk '{print $1}')"
  else
    echo "ERROR: neither sha256sum nor shasum is available to verify ${file}." >&2
    exit 1
  fi
  if [ "${actual}" != "${expected}" ]; then
    echo "ERROR: ${file} sha256 ${actual} != expected ${expected}" >&2
    exit 1
  fi
}

# Download a release tarball once, verify its sha256 before anything
# is extracted from it (same rule as LAME), and extract it into a
# per-slice build dir. The tarball is verified on every run, not only
# on download, so a corrupt file can never slip through a re-run.
extract_tarball() {
  local url="$1"
  local expected_sha="$2"
  local tarball="$3"
  local dest="$4"
  local file="${WORK_DIR}/${tarball}"
  if [ ! -f "${file}" ]; then
    curl -fL -o "${file}" "${url}"
  fi
  verify_sha256 "${file}" "${expected_sha}"
  rm -rf "${dest}"; mkdir -p "${dest}"
  tar -xz -C "${dest}" --strip-components=1 -f "${file}"
}

echo "=========================================================="
echo " BBeBee iOS Real mpv Builder"
echo "=========================================================="
echo "Target Dir:      ${TARGET_DIR}"
echo "mpv Version:     ${MPV_VERSION}"
echo "FFmpeg Version:  ${FFMPEG_VERSION}"
echo "Version Tag:     ${FRAMEWORK_VERSION}"
echo "=========================================================="

if ! command -v xcrun >/dev/null 2>&1; then
  echo "ERROR: xcrun not found. Building iOS mpv requires macOS with Xcode command line tools." >&2
  exit 1
fi

IPHONEOS_SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
IPHONESIM_SDK="$(xcrun --sdk iphonesimulator --show-sdk-path)"

if [ -z "${IPHONEOS_SDK}" ] || [ -z "${IPHONESIM_SDK}" ]; then
  echo "ERROR: Unable to locate iOS SDKs via xcrun." >&2
  exit 1
fi

WORK_DIR="${TMPDIR:-/tmp}/mpv-build-ios-$$"
mkdir -p "${WORK_DIR}"
cleanup() {
  rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

CORES=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

build_slice() {
  local slice_name="$1"
  local arch="$2"
  local sdk_name="$3"
  local sdk_path="$4"
  local min_ver_flag="$5"

  echo "=== Building slice: ${slice_name} (${arch} for ${sdk_name}) ==="
  local slice_prefix="${WORK_DIR}/prefix_${slice_name}"
  mkdir -p "${slice_prefix}/lib/pkgconfig" "${slice_prefix}/include"

  local clang_bin="$(xcrun --sdk "${sdk_name}" -f clang)"
  local clangxx_bin="$(xcrun --sdk "${sdk_name}" -f clang++)"
  local ar_bin="$(xcrun --sdk "${sdk_name}" -f ar)"
  local nm_bin="$(xcrun --sdk "${sdk_name}" -f nm)"
  local ranlib_bin="$(xcrun --sdk "${sdk_name}" -f ranlib)"
  local strip_bin="$(xcrun --sdk "${sdk_name}" -f strip)"

  export PKG_CONFIG_LIBDIR="${slice_prefix}/lib/pkgconfig:${slice_prefix}/lib64/pkgconfig:${slice_prefix}/share/pkgconfig"
  export PKG_CONFIG_PATH="${slice_prefix}/lib/pkgconfig:${slice_prefix}/lib64/pkgconfig:${slice_prefix}/share/pkgconfig"

  local cpu_family="${arch}"
  [ "${arch}" = "arm64" ] && cpu_family="aarch64"

  # Autotools host triple for the xiph/LAME builds below. The SDK and
  # -arch flags (not the triple) select device vs simulator; the triple
  # only has to get CPU family and OS right.
  local host_triple="aarch64-apple-darwin"
  [ "${arch}" = "x86_64" ] && host_triple="x86_64-apple-darwin"
  local ios_cflags="-arch ${arch} -isysroot ${sdk_path} ${min_ver_flag} -O3 -fPIC"

  local cross_file_static="${WORK_DIR}/crossfile_static_${slice_name}.txt"
  cat << CROSSFILE > "${cross_file_static}"
[built-in options]
buildtype = 'release'
default_library = 'static'
c_args = ['-arch', '${arch}', '-isysroot', '${sdk_path}', '${min_ver_flag}', '-I${slice_prefix}/include', '-O3', '-fPIC']
cpp_args = ['-arch', '${arch}', '-isysroot', '${sdk_path}', '${min_ver_flag}', '-I${slice_prefix}/include', '-O3', '-fPIC']
objc_args = ['-arch', '${arch}', '-isysroot', '${sdk_path}', '${min_ver_flag}', '-I${slice_prefix}/include', '-O3', '-fPIC']
c_link_args = ['-arch', '${arch}', '-isysroot', '${sdk_path}', '${min_ver_flag}', '-L${slice_prefix}/lib']
cpp_link_args = ['-arch', '${arch}', '-isysroot', '${sdk_path}', '${min_ver_flag}', '-L${slice_prefix}/lib']
objc_link_args = ['-arch', '${arch}', '-isysroot', '${sdk_path}', '${min_ver_flag}', '-L${slice_prefix}/lib']

[binaries]
c = '${clang_bin}'
cpp = '${clangxx_bin}'
objc = '${clang_bin}'
ar = '${ar_bin}'
nm = '${nm_bin}'
strip = '${strip_bin}'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = ['${slice_prefix}/lib/pkgconfig', '${slice_prefix}/lib64/pkgconfig', '${slice_prefix}/share/pkgconfig']

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = '${cpu_family}'
cpu = '${arch}'
endian = 'little'
CROSSFILE

  # 0. Build external encoder libraries for this iOS slice, BEFORE
  # FFmpeg's configure (see the top-of-file note). Each library is
  # extracted into a per-slice build dir (three slices share one
  # script run, so configuring in a shared tree would pollute it for
  # the next slice) and configured there — the tarballs already
  # contain ./configure, so no autogen step runs at all.
  echo "--- [1/7] Compiling external encoder libraries for ${slice_name} ---"

  local ogg_build="${WORK_DIR}/ogg_${slice_name}"
  extract_tarball "https://downloads.xiph.org/releases/ogg/libogg-${LIBOGG_VERSION}.tar.gz" \
    "${LIBOGG_SHA256}" "libogg-${LIBOGG_VERSION}.tar.gz" "${ogg_build}"
  (
    cd "${ogg_build}"
    ./configure --host="${host_triple}" --prefix="${slice_prefix}" \
      --enable-static --disable-shared --with-pic \
      CC="${clang_bin}" CFLAGS="${ios_cflags}" CPPFLAGS="${ios_cflags}" \
      LDFLAGS="-arch ${arch} -isysroot ${sdk_path} ${min_ver_flag}"
    make -j"${CORES}"
    make install
  )

  if [ ! -f "${WORK_DIR}/lame-${LAME_VERSION}.tar.gz" ]; then
    curl -fL -o "${WORK_DIR}/lame-${LAME_VERSION}.tar.gz" \
      "https://downloads.sourceforge.net/project/lame/lame/${LAME_VERSION}/lame-${LAME_VERSION}.tar.gz"
    verify_sha256 "${WORK_DIR}/lame-${LAME_VERSION}.tar.gz" "${LAME_SHA256}"
  fi
  local lame_build="${WORK_DIR}/lame_${slice_name}"
  rm -rf "${lame_build}"; mkdir -p "${lame_build}"
  tar -xz -C "${lame_build}" --strip-components=1 -f "${WORK_DIR}/lame-${LAME_VERSION}.tar.gz"
  (
    cd "${lame_build}"
    ./configure --host="${host_triple}" --prefix="${slice_prefix}" \
      --enable-static --disable-shared --with-pic \
      --disable-frontend --disable-decoder --disable-gtktest \
      CC="${clang_bin}" CFLAGS="${ios_cflags}" CPPFLAGS="${ios_cflags}" \
      LDFLAGS="-arch ${arch} -isysroot ${sdk_path} ${min_ver_flag}"
    make -j"${CORES}"
    make install
  )

  local vorbis_build="${WORK_DIR}/vorbis_${slice_name}"
  extract_tarball "https://downloads.xiph.org/releases/vorbis/libvorbis-${LIBVORBIS_VERSION}.tar.gz" \
    "${LIBVORBIS_SHA256}" "libvorbis-${LIBVORBIS_VERSION}.tar.gz" "${vorbis_build}"
  (
    cd "${vorbis_build}"
    ./configure --host="${host_triple}" --prefix="${slice_prefix}" \
      --enable-static --disable-shared --with-pic \
      --disable-docs --disable-examples --disable-oggtest \
      CC="${clang_bin}" CFLAGS="${ios_cflags}" CPPFLAGS="${ios_cflags}" \
      LDFLAGS="-arch ${arch} -isysroot ${sdk_path} ${min_ver_flag}" LIBS="-lm"
    # The tarball's 2020-vintage libtool passes -force_cpusubtype_ALL
    # when linking, and Xcode 16.4's linker rejects that flag — this
    # kills `make all` at the test_sharedbook link (the library objects
    # themselves build fine). There is no configure switch for this:
    # --disable-vorbistest does not exist (configure warns it is
    # unrecognized) and test_sharedbook is built unconditionally by
    # `make all`. So strip the dead flag from the libtool script that
    # ./configure just generated — patching before configure would be
    # overwritten, since configure writes this file. Static library
    # links use ar, not ld, so removing the flag changes nothing about
    # the libraries installed below.
    if grep -q "force_cpusubtype_ALL" libtool 2>/dev/null; then
      sed -i '' 's/-force_cpusubtype_ALL//g' libtool
    fi
    make -j"${CORES}"
    make install
  )

  local opus_build="${WORK_DIR}/opus_${slice_name}"
  extract_tarball "https://downloads.xiph.org/releases/opus/opus-${LIBOPUS_VERSION}.tar.gz" \
    "${LIBOPUS_SHA256}" "opus-${LIBOPUS_VERSION}.tar.gz" "${opus_build}"
  (
    cd "${opus_build}"
    ./configure --host="${host_triple}" --prefix="${slice_prefix}" \
      --enable-static --disable-shared --with-pic \
      --disable-doc --disable-extra-programs \
      CC="${clang_bin}" CFLAGS="${ios_cflags}" CPPFLAGS="${ios_cflags}" \
      LDFLAGS="-arch ${arch} -isysroot ${sdk_path} ${min_ver_flag}"
    make -j"${CORES}"
    make install
  )

  # 1. Build FFmpeg for this iOS slice
  echo "--- [2/7] Compiling FFmpeg for ${slice_name} ---"
  local ffmpeg_build="${WORK_DIR}/ffmpeg_${slice_name}"
  mkdir -p "${ffmpeg_build}"
  if [ ! -d "${WORK_DIR}/ffmpeg-src" ]; then
    git clone --depth 1 --branch "${FFMPEG_VERSION}" https://github.com/FFmpeg/FFmpeg.git "${WORK_DIR}/ffmpeg-src"
    verify_git_head "${WORK_DIR}/ffmpeg-src" "${FFMPEG_COMMIT}"
  fi
  cp -r "${WORK_DIR}/ffmpeg-src"/* "${ffmpeg_build}/"

  local extra_ff_args=""
  [ "${arch}" = "x86_64" ] && extra_ff_args="--disable-x86asm"

  # iOS transcode: same selective set as Android. The --disable-*
  # base keeps the archive small; the enables below are what make
  # every resolveTranscodeTarget() entry resolvable. libmp3lame/
  # libvorbis/libopus come from step 0 above, so they must be
  # discoverable here via -I/-L on the slice prefix. (No --disable-asm
  # for arm64 here, unlike Android: iOS links statically into an app
  # executable, where the movrel relocation that lld rejects for a
  # shared .so is not a problem.)
  local encoder_flags=""
  local muxer_flags=""
  local enc mux
  for enc in ${TRANSCODE_ENCODERS}; do encoder_flags="${encoder_flags} --enable-encoder=${enc}"; done
  for mux in ${TRANSCODE_MUXERS}; do muxer_flags="${muxer_flags} --enable-muxer=${mux}"; done

  (
    cd "${ffmpeg_build}"
    ./configure \
      --prefix="${slice_prefix}" \
      --libdir="${slice_prefix}/lib" \
      --enable-cross-compile \
      --target-os=darwin \
      --arch="${arch}" \
      --cc="${clang_bin}" \
      --ar="${ar_bin}" \
      --nm="${nm_bin}" \
      --ranlib="${ranlib_bin}" \
      --strip="${strip_bin}" \
      --extra-cflags="-arch ${arch} -isysroot ${sdk_path} ${min_ver_flag} -fPIC -O3 -I${slice_prefix}/include" \
      --extra-ldflags="-arch ${arch} -isysroot ${sdk_path} ${min_ver_flag} -L${slice_prefix}/lib" \
      --disable-programs \
      --disable-doc \
      --disable-avdevice \
      --disable-postproc \
      --disable-encoders \
      --disable-muxers \
      ${encoder_flags} \
      ${muxer_flags} \
      --enable-libmp3lame \
      --enable-libvorbis \
      --enable-libopus \
      --enable-static \
      --disable-shared \
      --enable-pic \
      --enable-gpl \
      --enable-version3 \
      --enable-audiotoolbox \
      --enable-videotoolbox \
      ${extra_ff_args}
    make -j"${CORES}"
    make install
  )

  # 2. Build FreeType for this iOS slice
  echo "--- [3/7] Compiling FreeType for ${slice_name} ---"
  local freetype_build="${WORK_DIR}/freetype_${slice_name}"
  mkdir -p "${freetype_build}"
  if [ ! -d "${WORK_DIR}/freetype-src" ]; then
    git clone --depth 1 --branch "${FREETYPE_VERSION}" https://github.com/freetype/freetype.git "${WORK_DIR}/freetype-src"
    verify_git_head "${WORK_DIR}/freetype-src" "${FREETYPE_COMMIT}"
  fi
  cp -r "${WORK_DIR}/freetype-src"/* "${freetype_build}/"

  (
    cd "${freetype_build}"
    meson setup build \
      --cross-file "${cross_file_static}" \
      --prefix="${slice_prefix}" \
      --libdir=lib \
      --default-library=static \
      -Dbrotli=disabled \
      -Dbzip2=disabled \
      -Dharfbuzz=disabled \
      -Dpng=disabled \
      -Dzlib=disabled \
      -Dtests=disabled
    ninja -C build install
  )

  # 3. Build FriBidi for this iOS slice
  echo "--- [4/7] Compiling FriBidi for ${slice_name} ---"
  local fribidi_build="${WORK_DIR}/fribidi_${slice_name}"
  mkdir -p "${fribidi_build}"
  if [ ! -d "${WORK_DIR}/fribidi-src" ]; then
    git clone --depth 1 --branch "${FRIBIDI_VERSION}" https://github.com/fribidi/fribidi.git "${WORK_DIR}/fribidi-src"
    verify_git_head "${WORK_DIR}/fribidi-src" "${FRIBIDI_COMMIT}"
  fi
  cp -r "${WORK_DIR}/fribidi-src"/* "${fribidi_build}/"

  (
    cd "${fribidi_build}"
    meson setup build \
      --cross-file "${cross_file_static}" \
      --prefix="${slice_prefix}" \
      --libdir=lib \
      --default-library=static \
      -Dbin=false \
      -Ddocs=false \
      -Dtests=false
    ninja -C build install
  )

  # 4. Build HarfBuzz for this iOS slice
  echo "--- [5/7] Compiling HarfBuzz for ${slice_name} ---"
  local harfbuzz_build="${WORK_DIR}/harfbuzz_${slice_name}"
  mkdir -p "${harfbuzz_build}"
  if [ ! -d "${WORK_DIR}/harfbuzz-src" ]; then
    git clone --depth 1 --branch "${HARFBUZZ_VERSION}" https://github.com/harfbuzz/harfbuzz.git "${WORK_DIR}/harfbuzz-src"
    verify_git_head "${WORK_DIR}/harfbuzz-src" "${HARFBUZZ_COMMIT}"
  fi
  cp -r "${WORK_DIR}/harfbuzz-src"/* "${harfbuzz_build}/"

  (
    cd "${harfbuzz_build}"
    meson setup build \
      --cross-file "${cross_file_static}" \
      --prefix="${slice_prefix}" \
      --libdir=lib \
      --default-library=static \
      -Dglib=disabled \
      -Dgobject=disabled \
      -Dcairo=disabled \
      -Dchafa=disabled \
      -Dicu=disabled \
      -Dgraphite=disabled \
      -Dgraphite2=disabled \
      -Dfreetype=disabled \
      -Dutilities=disabled \
      -Dtests=disabled \
      -Ddocs=disabled \
      -Dbenchmark=disabled \
      -Dintrospection=disabled
    ninja -C build install
  )

  # 5. Build libass for this iOS slice
  echo "--- [6/7] Compiling libass for ${slice_name} ---"
  local libass_build="${WORK_DIR}/libass_${slice_name}"
  mkdir -p "${libass_build}"
  if [ ! -d "${WORK_DIR}/libass-src" ]; then
    git clone --depth 1 --branch "${LIBASS_VERSION}" https://github.com/libass/libass.git "${WORK_DIR}/libass-src"
    verify_git_head "${WORK_DIR}/libass-src" "${LIBASS_COMMIT}"
  fi
  cp -r "${WORK_DIR}/libass-src"/* "${libass_build}/"

  (
    cd "${libass_build}"
    meson setup build \
      --cross-file "${cross_file_static}" \
      --prefix="${slice_prefix}" \
      --libdir=lib \
      --default-library=static \
      -Dfontconfig=disabled \
      -Ddirectwrite=disabled \
      -Dcoretext=disabled \
      -Dasm=disabled \
      -Dlibunibreak=disabled \
      -Drequire-system-font-provider=false \
      -Dtest=false \
      -Dprofile=false
    ninja -C build install
  )

  # 6. Build libplacebo for this iOS slice
  echo "--- [7/7] Compiling libplacebo for ${slice_name} ---"
  local libplacebo_build="${WORK_DIR}/libplacebo_${slice_name}"
  mkdir -p "${libplacebo_build}"
  if [ ! -d "${WORK_DIR}/libplacebo-src" ]; then
    git clone --depth 1 --recurse-submodules --shallow-submodules --branch "${LIBPLACEBO_VERSION}" https://github.com/haasn/libplacebo.git "${WORK_DIR}/libplacebo-src"
    verify_git_head "${WORK_DIR}/libplacebo-src" "${LIBPLACEBO_COMMIT}"
  fi
  cp -r "${WORK_DIR}/libplacebo-src"/* "${libplacebo_build}/"

  (
    cd "${libplacebo_build}"
    meson setup build \
      --cross-file "${cross_file_static}" \
      --prefix="${slice_prefix}" \
      --libdir=lib \
      --default-library=static \
      -Dvulkan=disabled \
      -Dopengl=disabled \
      -Dshaderc=disabled \
      -Dglslang=disabled \
      -Dlcms=disabled \
      -Ddovi=disabled \
      -Dlibdovi=disabled \
      -Dunwind=disabled \
      -Dxxhash=disabled \
      -Ddemos=false \
      -Dtests=false \
      -Dbench=false \
      -Dfuzz=false
    ninja -C build install
  )

  # 7. Build mpv for this iOS slice
  echo "--- Compiling Patched mpv for ${slice_name} ---"
  local mpv_build="${WORK_DIR}/mpv_${slice_name}"
  mkdir -p "${mpv_build}"
  if [ ! -d "${WORK_DIR}/mpv-src" ]; then
    git clone --depth 1 --branch "${MPV_VERSION}" https://github.com/mpv-player/mpv.git "${WORK_DIR}/mpv-src"
    verify_git_head "${WORK_DIR}/mpv-src" "${MPV_COMMIT}"
    (
      cd "${WORK_DIR}/mpv-src"
      # A5: see build-linux.sh — print the first attempt's failure
      # before falling back to CRLF conversion, instead of swallowing it.
      if git apply --ignore-whitespace --ignore-space-change "${PATCH_FILE}" > "${WORK_DIR}/patch.log" 2>&1; then
        echo "PCM tap patch applied cleanly."
      else
        echo "First patch attempt failed (log follows); retrying with CRLF conversion..."
        cat "${WORK_DIR}/patch.log"
        tr -d '\r' < "${PATCH_FILE}" | git apply --ignore-whitespace --ignore-space-change -
      fi
      # Remove host macOS /usr/lib and /usr/local/lib search flags that break iOS linking
      sed -i.bak "s|'-L/usr/lib', '-L/usr/local/lib'||g" meson.build
    )
  fi
  cp -r "${WORK_DIR}/mpv-src"/* "${mpv_build}/"

  local cross_file="${WORK_DIR}/crossfile_${slice_name}.txt"
  cat << CROSSFILE > "${cross_file}"
[built-in options]
buildtype = 'release'
default_library = 'shared'
c_args = ['-arch', '${arch}', '-isysroot', '${sdk_path}', '${min_ver_flag}', '-I${slice_prefix}/include', '-O3', '-fPIC', '-Wno-deprecated-declarations']
cpp_args = ['-arch', '${arch}', '-isysroot', '${sdk_path}', '${min_ver_flag}', '-I${slice_prefix}/include', '-O3', '-fPIC', '-Wno-deprecated-declarations']
objc_args = ['-arch', '${arch}', '-isysroot', '${sdk_path}', '${min_ver_flag}', '-I${slice_prefix}/include', '-O3', '-fPIC', '-Wno-deprecated-declarations']
c_link_args = ['-arch', '${arch}', '-isysroot', '${sdk_path}', '${min_ver_flag}', '-L${slice_prefix}/lib', '-framework', 'AudioToolbox', '-framework', 'AVFoundation', '-framework', 'CoreMedia', '-framework', 'CoreVideo', '-framework', 'VideoToolbox', '-framework', 'Security', '-framework', 'Foundation', '-framework', 'CoreFoundation', '-framework', 'CoreAudio', '-liconv', '-lz', '-lbz2']
cpp_link_args = ['-arch', '${arch}', '-isysroot', '${sdk_path}', '${min_ver_flag}', '-L${slice_prefix}/lib', '-framework', 'AudioToolbox', '-framework', 'AVFoundation', '-framework', 'CoreMedia', '-framework', 'CoreVideo', '-framework', 'VideoToolbox', '-framework', 'Security', '-framework', 'Foundation', '-framework', 'CoreFoundation', '-framework', 'CoreAudio', '-liconv', '-lz', '-lbz2']
objc_link_args = ['-arch', '${arch}', '-isysroot', '${sdk_path}', '${min_ver_flag}', '-L${slice_prefix}/lib', '-framework', 'AudioToolbox', '-framework', 'AVFoundation', '-framework', 'CoreMedia', '-framework', 'CoreVideo', '-framework', 'VideoToolbox', '-framework', 'Security', '-framework', 'Foundation', '-framework', 'CoreFoundation', '-framework', 'CoreAudio', '-liconv', '-lz', '-lbz2']

[binaries]
c = '${clang_bin}'
cpp = '${clangxx_bin}'
objc = '${clang_bin}'
ar = '${ar_bin}'
nm = '${nm_bin}'
strip = '${strip_bin}'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = ['${slice_prefix}/lib/pkgconfig', '${slice_prefix}/lib64/pkgconfig', '${slice_prefix}/share/pkgconfig']

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = '${cpu_family}'
cpu = '${arch}'
endian = 'little'
CROSSFILE

  (
    cd "${mpv_build}"
    export MACOS_SDK="${sdk_path}"
    export MACOS_SDK_VERSION="$(xcrun --sdk "${sdk_name}" --show-sdk-version 2>/dev/null || echo '15.0')"
    meson setup build \
      --cross-file "${cross_file}" \
      --default-library shared \
      -Dlibmpv=true \
      -Dcplayer=false \
      -Dcoreaudio=disabled \
      -Daudiounit=enabled \
      -Dcocoa=disabled \
      -Davfoundation=disabled \
      -Dios-gl=disabled \
      -Dmacos-10-15-4-features=disabled \
      -Dmacos-11-features=disabled \
      -Dmacos-11-3-features=disabled \
      -Dmacos-12-features=disabled \
      -Dmacos-cocoa-cb=disabled \
      -Dmacos-media-player=disabled \
      -Dmacos-touchbar=disabled \
      -Dalsa=disabled \
      -Dpulse=disabled \
      -Dpipewire=disabled \
      -Djack=disabled \
      -Dsndio=disabled \
      -Doss-audio=disabled \
      -Dwasapi=disabled \
      -Daudiotrack=disabled \
      -Daaudio=disabled \
      -Dopensles=disabled \
      -Dgl=disabled \
      -Dvulkan=disabled \
      -Dwayland=disabled \
      -Dx11=disabled \
      -Dsdl2-video=disabled \
      -Dsdl2-audio=disabled \
      -Dlua=disabled \
      -Djavascript=disabled \
      -Duchardet=disabled \
      -Drubberband=disabled \
      -Dlcms2=disabled \
      -Dlibarchive=disabled \
      -Dcdda=disabled \
      -Ddvdnav=disabled \
      -Diconv=disabled \
      -Dzlib=disabled \
      -Dcplugins=disabled \
      -Dvideotoolbox-pl=disabled \
      -Dmanpage-build=disabled \
      -Dhtml-build=disabled \
      -Dpdf-build=disabled \
      -Dlibavdevice=disabled \
      -Dlibbluray=disabled \
      -Dvapoursynth=disabled \
      -Dzimg=disabled \
      -Dcaca=disabled \
      -Ddrm=disabled \
      -Dgbm=disabled \
      -Djpeg=disabled \
      -Dshaderc=disabled \
      -Dsixel=disabled \
      -Dspirv-cross=disabled \
      -Dvaapi=disabled \
      -Dvdpau=disabled \
      -Ddvbin=disabled \
      -Degl=disabled \
      -Dvideotoolbox-gl=disabled \
      -Dswift-build=disabled

    ninja -C build
    mkdir -p "${WORK_DIR}/binaries"
    dylib_out=$(find build -maxdepth 1 -name "libmpv*.dylib" -type f | head -n 1)
    if [ -z "${dylib_out}" ]; then
      dylib_out=$(find build -maxdepth 1 -name "libmpv*.dylib" | head -n 1)
    fi
    if [ -z "${dylib_out}" ]; then
      dylib_out=$(find build -name "libmpv*.dylib" | head -n 1)
    fi
    if [ -z "${dylib_out}" ] || [ ! -e "${dylib_out}" ]; then
      echo "ERROR: libmpv dynamic library not found in build directory for ${slice_name}!" >&2
      ls -la build/
      exit 1
    fi
    cp -L "${dylib_out}" "${WORK_DIR}/binaries/mpv_${slice_name}"
    chmod 0755 "${WORK_DIR}/binaries/mpv_${slice_name}"
    install_name_tool -id "@rpath/mpv.framework/mpv" "${WORK_DIR}/binaries/mpv_${slice_name}" 2>/dev/null || true
  )

  # Stage the FFmpeg SDK for the bridge transcode build. The whole
  # prefix lives under WORK_DIR, which `trap cleanup EXIT` deletes,
  # so — mirroring Android's step 11 — the headers and static archives
  # the bridge links must outlive this script. Consumers (see
  # scripts/build-ios-framework.sh) find them at
  #   staging/mobile-mpv/ios/ffmpeg-sdk-<slice_name>/
  # next to the mpv.framework target.
  echo "=== Staging FFmpeg SDK for bridge transcode linking (${slice_name}) ==="
  local sdk_out_dir="${ROOT_DIR}/staging/mobile-mpv/ios/ffmpeg-sdk-${slice_name}"
  rm -rf "${sdk_out_dir}"
  mkdir -p "${sdk_out_dir}/include" "${sdk_out_dir}/lib"
  local hdr lib sdk_libs=""
  for hdr in libavcodec libavformat libavutil libswresample lame opus vorbis ogg; do
    if [ -d "${slice_prefix}/include/${hdr}" ]; then
      cp -R "${slice_prefix}/include/${hdr}" "${sdk_out_dir}/include/"
    fi
  done
  for lib in libavcodec libavformat libavutil libswresample libswscale \
             libmp3lame libopus libvorbis libvorbisenc libvorbisfile libogg; do
    if [ -f "${slice_prefix}/lib/${lib}.a" ]; then
      cp -L "${slice_prefix}/lib/${lib}.a" "${sdk_out_dir}/lib/"
      sdk_libs="${sdk_libs} ${lib}.a"
    fi
  done
  if [ ! -f "${sdk_out_dir}/lib/libavcodec.a" ]; then
    echo "ERROR: FFmpeg SDK staging failed — ${sdk_out_dir}/lib/libavcodec.a missing!" >&2
    exit 1
  fi
  cat > "${sdk_out_dir}/BUILD_INFO.txt" <<EOF
FFmpeg: ${FFMPEG_VERSION}
LAME: ${LAME_VERSION}
libogg: ${LIBOGG_VERSION}
libvorbis: ${LIBVORBIS_VERSION}
libopus: ${LIBOPUS_VERSION}
Slice: ${slice_name}
Staged archives:${sdk_libs}
EOF
  echo "FFmpeg SDK staged at ${sdk_out_dir}:${sdk_libs}"
}

# Build the 3 standard iOS slices
build_slice "device_arm64" "arm64" "iphoneos" "${IPHONEOS_SDK}" "-miphoneos-version-min=16.0"
build_slice "sim_arm64" "arm64" "iphonesimulator" "${IPHONESIM_SDK}" "-mios-simulator-version-min=16.0"
build_slice "sim_x86_64" "x86_64" "iphonesimulator" "${IPHONESIM_SDK}" "-mios-simulator-version-min=16.0"

# Combine simulator slices into universal binary
echo "=== Creating Universal Simulator mpv Binary ==="
lipo -create \
  "${WORK_DIR}/binaries/mpv_sim_arm64" \
  "${WORK_DIR}/binaries/mpv_sim_x86_64" \
  -output "${WORK_DIR}/binaries/mpv_simulator"
chmod 0755 "${WORK_DIR}/binaries/mpv_simulator" 2>/dev/null || true
install_name_tool -id "@rpath/mpv.framework/mpv" "${WORK_DIR}/binaries/mpv_simulator" 2>/dev/null || true

# Assemble target mpv.framework
echo "=== Assembling Authentic mpv.framework ==="
rm -rf "${TARGET_DIR}"
mkdir -p "${TARGET_DIR}/Headers"

# Copy device binary as primary framework executable
cp "${WORK_DIR}/binaries/mpv_device_arm64" "${TARGET_DIR}/mpv"
chmod 0755 "${TARGET_DIR}/mpv"
install_name_tool -id "@rpath/mpv.framework/mpv" "${TARGET_DIR}/mpv" 2>/dev/null || true

# Copy verified client header
if [ -f "${WORK_DIR}/mpv-src/include/mpv/client.h" ]; then
  cp "${WORK_DIR}/mpv-src/include/mpv/client.h" "${TARGET_DIR}/Headers/client.h"
elif [ -f "${ROOT_DIR}/engine/mpv_client.h" ]; then
  cp "${ROOT_DIR}/engine/mpv_client.h" "${TARGET_DIR}/Headers/client.h"
fi

# Generate valid Info.plist with pinned D1 version
cat << PLIST > "${TARGET_DIR}/Info.plist"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>mpv</string>
    <key>CFBundleIdentifier</key>
    <string>io.mpv.framework</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>mpv</string>
    <key>CFBundlePackageType</key>
    <string>FMWK</string>
    <key>CFBundleShortVersionString</key>
    <string>${FRAMEWORK_VERSION}</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>MinimumOSVersion</key>
    <string>16.0</string>
</dict>
</plist>
PLIST

echo "=== iOS mpv.framework Build Verification ==="
lipo -info "${TARGET_DIR}/mpv"
otool -D "${TARGET_DIR}/mpv" 2>/dev/null || true
SYM_COUNT=$(nm -gU "${TARGET_DIR}/mpv" 2>/dev/null | wc -l || echo 0)
if [ "${SYM_COUNT}" -lt 50 ]; then
  SYM_COUNT=$(nm "${TARGET_DIR}/mpv" 2>/dev/null | grep -E " [TtSs] " | wc -l || echo 0)
fi
echo "Exported symbols count: ${SYM_COUNT}"
if [ "${SYM_COUNT}" -lt 50 ]; then
  echo "ERROR: Suspiciously low symbol count (${SYM_COUNT}) in ${TARGET_DIR}/mpv!" >&2
  exit 1
fi

# NOTE: with `set -o pipefail`, `tool | grep -q` misreports "not found":
# grep -q exits on the first match, the producer then dies with SIGPIPE (141)
# and the whole pipeline fails. Capture the dumps into variables first and
# grep those instead (a herestring reads from a temp file: no pipe, no SIGPIPE).
NM_SYMS="$(nm -gU "${TARGET_DIR}/mpv" 2>/dev/null || true)"
if [ -z "${NM_SYMS}" ]; then
  NM_SYMS="$(nm "${TARGET_DIR}/mpv" 2>/dev/null || true)"
fi
BIN_STRS="$(strings "${TARGET_DIR}/mpv" 2>/dev/null || true)"
REQUIRED_SYMBOLS=("mpv_create" "mpv_initialize" "mpv_destroy" "mpv_command" "mpv_set_pcm_callback")
for sym in "${REQUIRED_SYMBOLS[@]}"; do
  if grep -q "${sym}" <<< "${NM_SYMS}" || grep -q "${sym}" <<< "${BIN_STRS}"; then
    echo "  [OK] Verified symbol: ${sym}"
  else
    echo "ERROR: Required symbol '${sym}' missing from ${TARGET_DIR}/mpv!" >&2
    exit 1
  fi
done
echo "=== iOS Genuine mpv.framework successfully staged at ${TARGET_DIR} ==="
