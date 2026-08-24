cmake_minimum_required(VERSION 3.21)

project(mediaView VERSION 0.1.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "mediaView currently targets Windows only.")
endif()

add_executable(mediaView WIN32
    src/main.cpp
)

target_compile_features(mediaView PRIVATE cxx_std_20)
target_compile_definitions(mediaView PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(mediaView PRIVATE d2d1 windowscodecs shell32 shlwapi)

if(MSVC)
    target_compile_options(mediaView PRIVATE /W4 /permissive-)
    target_compile_options(mediaView PRIVATE $<$<CONFIG:Release>:/O2>)
endif()
