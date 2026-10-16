cmake_minimum_required(VERSION 3.16)
project(audio-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

add_executable(audio-engine
    main.cpp
    pcm_ring_buffer.cpp
    pcm_ring_buffer.hpp
    fft.hpp
    json.hpp
    mpv_client.h
)

find_package(Threads REQUIRED)
target_link_libraries(audio-engine PRIVATE Threads::Threads)

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
