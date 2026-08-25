cmake_minimum_required(VERSION 3.21)

project(FeatherView VERSION 0.2.3.1 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "FeatherView currently targets Windows only.")
endif()

string(REPLACE "." "," PROJECT_VERSION_COMMAS "${PROJECT_VERSION}")
configure_file(assets/FeatherView.ico "${CMAKE_CURRENT_BINARY_DIR}/FeatherView.ico" COPYONLY)
configure_file(assets/FeatherViewLogo.png "${CMAKE_CURRENT_BINARY_DIR}/FeatherViewLogo.png" COPYONLY)
configure_file(src/FeatherView.rc.in "${CMAKE_CURRENT_BINARY_DIR}/FeatherView.rc" @ONLY)

add_executable(FeatherView WIN32
    src/main.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/FeatherView.rc"
)

target_compile_features(FeatherView PRIVATE cxx_std_20)
target_compile_definitions(FeatherView PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX FEATHERVIEW_VERSION=L\"${PROJECT_VERSION}\")
target_link_libraries(FeatherView PRIVATE d2d1 dwrite windowscodecs shell32 shlwapi dwmapi advapi32)

if(MSVC)
    target_compile_options(FeatherView PRIVATE /W4 /permissive-)
    target_compile_options(FeatherView PRIVATE $<$<CONFIG:Release>:/O2>)
endif()





