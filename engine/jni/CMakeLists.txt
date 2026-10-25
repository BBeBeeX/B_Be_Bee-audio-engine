cmake_minimum_required(VERSION 3.22.1)

# Shared C++ runtime, not the static default: with c++_static nothing
# guarantees libc++_shared.so is in the linker namespace before we
# dlopen(libmpv.so) — which links libc++_shared — and a personality symbol
# relocation dangled on a real device. Loading us first pulls libc++
# _shared.so in deterministically, and every RN app packages it.
set(ANDROID_STL c++_shared)

project(bbebee_audio_jni LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable compiler optimization and warnings
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall -Wextra -fPIC")

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_library(bbebee_audio_jni SHARED
    bbebee_audio_jni.cpp
    ../pcm_ring_buffer.cpp
)

# Link against Android NDK core libraries and dl for dynamic loading of libmpv
target_link_libraries(bbebee_audio_jni
    log
    android
    dl
)

# ---------------------------------------------------------------------------
# Optional FFmpeg transcoder support (HAVE_LIBAV).
#
# Point BEE_FFMPEG_SDK_DIR at an SDK staged by
# scripts/mpv-build/build-android.sh (`<target>/ffmpeg-sdk`) — static libav
# archives plus the external encoder libraries (libmp3lame, libvorbis,
# libopus, libogg) they reference. Without the SDK the bridge still builds
# and every transcode export answers honestly that transcoding is
# unsupported, mirroring the desktop engine's HAVE_LIBAV degradation.
# ---------------------------------------------------------------------------
set(BEE_FFMPEG_SDK_DIR "" CACHE PATH "Directory with staged FFmpeg SDK (include/ + lib/) for JNI transcode support")

if(BEE_FFMPEG_SDK_DIR)
    if(NOT EXISTS "${BEE_FFMPEG_SDK_DIR}/include/libavcodec/avcodec.h")
        message(FATAL_ERROR "BEE_FFMPEG_SDK_DIR='${BEE_FFMPEG_SDK_DIR}' does not look like an FFmpeg SDK (missing include/libavcodec/avcodec.h)")
    endif()

    target_sources(bbebee_audio_jni PRIVATE
        ../transcode.cpp
        transcode_caps.cpp
    )

    target_include_directories(bbebee_audio_jni PRIVATE "${BEE_FFMPEG_SDK_DIR}/include")
    target_compile_definitions(bbebee_audio_jni PRIVATE HAVE_LIBAV)

    # Static archive link order matters: libavformat pulls libavcodec, which
    # pulls libswresample and libavutil; the external encoder archives are
    # referenced by libavcodec's enabled encoders. `-Wl,--start-group`
    # resolves the intra-group cycles without hand-ordering every pair.
    target_link_libraries(bbebee_audio_jni
        -Wl,--start-group
        "${BEE_FFMPEG_SDK_DIR}/lib/libavformat.a"
        "${BEE_FFMPEG_SDK_DIR}/lib/libavcodec.a"
        "${BEE_FFMPEG_SDK_DIR}/lib/libswresample.a"
        "${BEE_FFMPEG_SDK_DIR}/lib/libavutil.a"
        "${BEE_FFMPEG_SDK_DIR}/lib/libmp3lame.a"
        "${BEE_FFMPEG_SDK_DIR}/lib/libvorbis.a"
        "${BEE_FFMPEG_SDK_DIR}/lib/libvorbisenc.a"
        "${BEE_FFMPEG_SDK_DIR}/lib/libopus.a"
        "${BEE_FFMPEG_SDK_DIR}/lib/libogg.a"
        -Wl,--end-group
        z
        m
    )

    message(STATUS "bbebee_audio_jni: HAVE_LIBAV enabled from ${BEE_FFMPEG_SDK_DIR}")
else()
    message(STATUS "bbebee_audio_jni: BEE_FFMPEG_SDK_DIR not set — building WITHOUT transcode support (transcode exports report unsupported)")
endif()

set_target_properties(bbebee_audio_jni PROPERTIES
    OUTPUT_NAME "bbebee_audio_jni"
)
