cmake_minimum_required(VERSION 3.22.1)
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

set_target_properties(bbebee_audio_jni PROPERTIES
    OUTPUT_NAME "bbebee_audio_jni"
)
