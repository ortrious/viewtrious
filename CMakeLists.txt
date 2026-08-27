cmake_minimum_required(VERSION 3.21)

project(Viewtrious VERSION 0.3.7.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Viewtrious currently targets Windows only.")
endif()

string(REPLACE "." "," PROJECT_VERSION_COMMAS "${PROJECT_VERSION}")
configure_file(assets/Viewtrious.ico "${CMAKE_CURRENT_BINARY_DIR}/Viewtrious.ico" COPYONLY)
configure_file(src/Viewtrious.rc.in "${CMAKE_CURRENT_BINARY_DIR}/Viewtrious.rc" @ONLY)

add_executable(Viewtrious WIN32
    src/main.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/Viewtrious.rc"
)

target_compile_features(Viewtrious PRIVATE cxx_std_20)
target_compile_definitions(Viewtrious PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX VIEWTRIOUS_VERSION=L\"${PROJECT_VERSION}\")
target_link_libraries(Viewtrious PRIVATE d2d1 dwrite gdiplus windowscodecs shell32 shlwapi propsys dwmapi advapi32)

if(MSVC)
    target_compile_options(Viewtrious PRIVATE /W4 /permissive-)
    target_compile_options(Viewtrious PRIVATE $<$<CONFIG:Release>:/O2>)
endif()





