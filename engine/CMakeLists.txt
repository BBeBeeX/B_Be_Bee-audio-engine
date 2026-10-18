cmake_minimum_required(VERSION 3.16)
project(audio-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
if(PkgConfig_FOUND)
    pkg_check_modules(LIBAV QUIET IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
    if(LIBAV_FOUND)
        set(HAVE_LIBAV ON)
    endif()
endif()
if(HAVE_LIBAV)
    message(STATUS "FFmpeg (libav*) found: transcode action enabled")
else()
    message(STATUS "FFmpeg (libav*) NOT found: transcode action will report unsupported at runtime")
endif()

add_executable(audio-engine
    main.cpp
    pcm_ring_buffer.cpp
    pcm_ring_buffer.hpp
    fft.hpp
    json.hpp
    mpv_client.h
    transcode.cpp
    transcode.hpp
)

find_package(Threads REQUIRED)
target_link_libraries(audio-engine PRIVATE Threads::Threads)
if(HAVE_LIBAV)
    target_compile_definitions(audio-engine PRIVATE HAVE_LIBAV)
    if(TARGET PkgConfig::LIBAV)
        target_link_libraries(audio-engine PRIVATE PkgConfig::LIBAV)
    else()
        target_include_directories(audio-engine PRIVATE ${LIBAV_INCLUDE_DIRS})
        target_link_directories(audio-engine PRIVATE ${LIBAV_LIBRARY_DIRS})
        target_link_libraries(audio-engine PRIVATE ${LIBAV_LIBRARIES})
    endif()
endif()

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

# Transcode tests (separate binary: test_pcm_tap.cpp has its own main()).
add_executable(audio-engine-transcode-tests
    tests/test_transcode.cpp
    transcode.cpp
    transcode.hpp
)
target_link_libraries(audio-engine-transcode-tests PRIVATE Threads::Threads)
if(HAVE_LIBAV)
    target_compile_definitions(audio-engine-transcode-tests PRIVATE HAVE_LIBAV)
    if(TARGET PkgConfig::LIBAV)
        target_link_libraries(audio-engine-transcode-tests PRIVATE PkgConfig::LIBAV)
    else()
        target_include_directories(audio-engine-transcode-tests PRIVATE ${LIBAV_INCLUDE_DIRS})
        target_link_directories(audio-engine-transcode-tests PRIVATE ${LIBAV_LIBRARY_DIRS})
        target_link_libraries(audio-engine-transcode-tests PRIVATE ${LIBAV_LIBRARIES})
    endif()
endif()
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
