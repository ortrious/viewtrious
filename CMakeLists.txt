cmake_minimum_required(VERSION 3.21)

project(Viewtrious VERSION 0.5.1.8 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Viewtrious currently targets Windows only.")
endif()

string(REPLACE "." "," PROJECT_VERSION_COMMAS "${PROJECT_VERSION}")
configure_file(assets/Viewtrious.ico "${CMAKE_CURRENT_BINARY_DIR}/Viewtrious.ico" COPYONLY)
configure_file(src/Viewtrious.rc.in "${CMAKE_CURRENT_BINARY_DIR}/Viewtrious.rc" @ONLY)

add_executable(Viewtrious WIN32
    src/main.cpp
    src/lanczos_resampler.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/Viewtrious.rc"
)

# Investigation-only command-line harness.  It is intentionally not linked into
# Viewtrious so the production Direct2D canvas remains unchanged.
add_executable(ViewtriousLanczosBenchmark
    src/lanczos_benchmark.cpp
    src/lanczos_resampler.cpp
)

target_compile_features(Viewtrious PRIVATE cxx_std_20)
target_compile_definitions(Viewtrious PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX VIEWTRIOUS_VERSION=L\"${PROJECT_VERSION}\")
target_link_libraries(Viewtrious PRIVATE d2d1 dwrite gdiplus windowscodecs shell32 shlwapi propsys dwmapi advapi32)
target_compile_features(ViewtriousLanczosBenchmark PRIVATE cxx_std_20)
target_compile_definitions(ViewtriousLanczosBenchmark PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(ViewtriousLanczosBenchmark PRIVATE windowscodecs ole32)

if(MSVC)
    target_compile_options(Viewtrious PRIVATE /W4 /permissive-)
    target_compile_options(Viewtrious PRIVATE $<$<CONFIG:Release>:/O2>)
    target_compile_options(ViewtriousLanczosBenchmark PRIVATE /W4 /permissive-)
    target_compile_options(ViewtriousLanczosBenchmark PRIVATE $<$<CONFIG:Release>:/O2>)
endif()





