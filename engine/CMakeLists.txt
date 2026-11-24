cmake_minimum_required(VERSION 3.16)
project(audio-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# A3: single source of truth for the engine version. The repo-root VERSION
# file is what release tags track; --version used to hardcode 1.0.0 while
# tags had already moved to 1.3.0. Fall back to 1.0.0 only when the file
# is absent (e.g. a bare engine/ checkout).
set(AUDIO_ENGINE_VERSION "1.0.0")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" AUDIO_ENGINE_VERSION_RAW)
    string(STRIP "${AUDIO_ENGINE_VERSION_RAW}" AUDIO_ENGINE_VERSION_RAW)
    if(NOT AUDIO_ENGINE_VERSION_RAW STREQUAL "")
        set(AUDIO_ENGINE_VERSION "${AUDIO_ENGINE_VERSION_RAW}")
    endif()
endif()

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# --- FFmpeg (libav*) for the "transcode" action --------------------------------
# Probed via pkg-config. When found, transcode.cpp is built with HAVE_LIBAV and
# the engine can transcode using the FFmpeg libraries bundled next to libmpv.
# When absent (e.g. minimal build machines), the engine still builds; the
# transcode action then reports "not supported" at runtime.
find_package(PkgConfig QUIET)
set(HAVE_LIBAV OFF)
set(LIBAV_INCLUDE_DIRS "")
set(LIBAV_LIBRARIES "")
set(LIBAV_LIBRARY_DIRS "")
set(LIBAV_PC_INCLUDEDIR "")
set(LIBAV_PC_LIBDIR "")
set(LIBAV_RESOLVED_LIB_DIR "")
if(PkgConfig_FOUND)
    if(APPLE)
        # Keep Homebrew's pkgconf from stripping /opt/homebrew/{include,lib}
        # when configured with system-dir defaults.
        set(ENV{PKG_CONFIG_ALLOW_SYSTEM_CFLAGS} "1")
        set(ENV{PKG_CONFIG_ALLOW_SYSTEM_LIBS} "1")
    endif()
    pkg_check_modules(LIBAV QUIET IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
    if(LIBAV_FOUND)
        if(APPLE)
            pkg_get_variable(LIBAV_PC_INCLUDEDIR libavformat includedir)
            pkg_get_variable(LIBAV_PC_LIBDIR libavformat libdir)
            find_path(LIBAV_RESOLVED_INCLUDE_DIR
                NAMES libavformat/avformat.h
                HINTS
                    ${LIBAV_INCLUDE_DIRS}
                    ${LIBAV_PC_INCLUDEDIR}
                    /opt/homebrew/opt/ffmpeg/include
                    /opt/homebrew/include
                    /usr/local/opt/ffmpeg/include
                    /usr/local/include
            )
            find_library(LIBAV_RESOLVED_AVFORMAT_LIB
                NAMES avformat
                HINTS
                    ${LIBAV_LIBRARY_DIRS}
                    ${LIBAV_PC_LIBDIR}
                    /opt/homebrew/opt/ffmpeg/lib
                    /opt/homebrew/lib
                    /usr/local/opt/ffmpeg/lib
                    /usr/local/lib
            )
            if(LIBAV_RESOLVED_AVFORMAT_LIB)
                get_filename_component(LIBAV_RESOLVED_LIB_DIR "${LIBAV_RESOLVED_AVFORMAT_LIB}" DIRECTORY)
            endif()
            if(LIBAV_RESOLVED_INCLUDE_DIR)
                set(HAVE_LIBAV ON)
            else()
                message(WARNING "FFmpeg pkg-config matched libav*, but libavformat/avformat.h was not found; disabling HAVE_LIBAV")
            endif()
        else()
            set(HAVE_LIBAV ON)
        endif()
    endif()
endif()
if(HAVE_LIBAV)
    if(LIBAV_RESOLVED_INCLUDE_DIR)
        message(STATUS "FFmpeg (libav*) found: transcode action enabled (include: ${LIBAV_RESOLVED_INCLUDE_DIR})")
    else()
        message(STATUS "FFmpeg (libav*) found: transcode action enabled")
    endif()
else()
    message(STATUS "FFmpeg (libav*) NOT found: transcode action will report unsupported at runtime")
endif()

function(audio_engine_link_libav target_name)
    if(HAVE_LIBAV)
        target_compile_definitions(${target_name} PRIVATE HAVE_LIBAV)
        if(TARGET PkgConfig::LIBAV)
            target_link_libraries(${target_name} PRIVATE PkgConfig::LIBAV)
        else()
            target_include_directories(${target_name} PRIVATE ${LIBAV_INCLUDE_DIRS})
            target_link_directories(${target_name} PRIVATE ${LIBAV_LIBRARY_DIRS})
            target_link_libraries(${target_name} PRIVATE ${LIBAV_LIBRARIES})
        endif()
        if(APPLE)
            target_include_directories(${target_name} SYSTEM PRIVATE
                ${LIBAV_RESOLVED_INCLUDE_DIR} ${LIBAV_INCLUDE_DIRS})
            if(LIBAV_RESOLVED_LIB_DIR)
                target_link_directories(${target_name} PRIVATE "${LIBAV_RESOLVED_LIB_DIR}")
            elseif(LIBAV_PC_LIBDIR)
                target_link_directories(${target_name} PRIVATE "${LIBAV_PC_LIBDIR}")
            endif()
        endif()
    endif()
endfunction()

add_executable(audio-engine
    main.cpp
    pcm_ring_buffer.cpp
    pcm_ring_buffer.hpp
    pcm_broadcaster.cpp
    pcm_broadcaster.hpp
    fft.hpp
    json.hpp
    mpv_client.h
    transcode.cpp
    transcode.hpp
    jni/transcode_caps.cpp
    jni/transcode_caps.hpp
)
target_compile_definitions(audio-engine PRIVATE
    AUDIO_ENGINE_VERSION="${AUDIO_ENGINE_VERSION}")

# C3: the fake-load fallback in main.cpp is compile-time gated and OFF
# by default; a test build can opt in with -DBBEBEE_TEST_FALLBACK=ON.
option(BBEBEE_TEST_FALLBACK "Enable the VITEST/NODE_ENV=test fake load (test builds only)" OFF)
if(BBEBEE_TEST_FALLBACK)
    target_compile_definitions(audio-engine PRIVATE AUDIO_ENGINE_TEST_FALLBACK)
endif()

find_package(Threads REQUIRED)
target_link_libraries(audio-engine PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(audio-engine PRIVATE ws2_32)
endif()
audio_engine_link_libav(audio-engine)

if(UNIX AND NOT APPLE)
    target_link_libraries(audio-engine PRIVATE ${CMAKE_DL_LIBS})
    set_target_properties(audio-engine PROPERTIES
        BUILD_RPATH "\$ORIGIN"
        INSTALL_RPATH "\$ORIGIN"
    )
elseif(APPLE)
    target_link_libraries(audio-engine PRIVATE ${CMAKE_DL_LIBS})
    set_target_properties(audio-engine PROPERTIES
        BUILD_RPATH "@executable_path;@loader_path"
        INSTALL_RPATH "@executable_path;@loader_path"
    )
endif()

enable_testing()
add_executable(audio-engine-tests
    tests/test_pcm_tap.cpp
    pcm_ring_buffer.cpp
    pcm_ring_buffer.hpp
    fft.hpp
)
target_link_libraries(audio-engine-tests PRIVATE Threads::Threads)
if(UNIX)
    target_link_libraries(audio-engine-tests PRIVATE ${CMAKE_DL_LIBS})
endif()
add_test(NAME audio-engine-tests COMMAND audio-engine-tests)

# Per-channel FFT checks: stereo extraction, mono clearing, sample rate.
add_executable(audio-engine-fft-stereo-tests
    tests/test_fft_stereo.cpp
    fft.hpp
)
add_test(NAME audio-engine-fft-stereo-tests COMMAND audio-engine-fft-stereo-tests)

# Transcode tests (separate binary: test_pcm_tap.cpp has its own main()).
add_executable(audio-engine-transcode-tests
    tests/test_transcode.cpp
    transcode.cpp
    transcode.hpp
)
target_link_libraries(audio-engine-transcode-tests PRIVATE Threads::Threads)
audio_engine_link_libav(audio-engine-transcode-tests)
if(UNIX)
    target_link_libraries(audio-engine-transcode-tests PRIVATE ${CMAKE_DL_LIBS})
endif()
if(APPLE)
    set_target_properties(audio-engine-transcode-tests PROPERTIES
        BUILD_RPATH "@executable_path;@loader_path"
        INSTALL_RPATH "@executable_path;@loader_path"
    )
endif()
add_test(NAME audio-engine-transcode-tests COMMAND audio-engine-transcode-tests)

# Post-DSP LAN PCM broadcast tap tests (BBPC header, s16le conversion, loopback TCP).
add_executable(audio-engine-broadcast-tests
    tests/test_pcm_broadcaster.cpp
    pcm_broadcaster.cpp
    pcm_broadcaster.hpp
    pcm_ring_buffer.cpp
    pcm_ring_buffer.hpp
)
target_link_libraries(audio-engine-broadcast-tests PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(audio-engine-broadcast-tests PRIVATE ws2_32)
endif()
add_test(NAME audio-engine-broadcast-tests COMMAND audio-engine-broadcast-tests)
