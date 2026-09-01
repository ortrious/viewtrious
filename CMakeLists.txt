cmake_minimum_required(VERSION 3.21)

project(Viewtrious VERSION 0.7.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Viewtrious currently targets Windows only.")
endif()

set(VIEWTRIOUS_3DXWARE_SDK_ROOT "" CACHE PATH "Root of the 3Dconnexion 3DxWare SDK (required for SpaceMouse support)")
set(VIEWTRIOUS_OCCT_ROOT "" CACHE PATH "Root of the OCCT 8.0.1 dynamic install (required for STEP/STP support)")
if(NOT VIEWTRIOUS_3DXWARE_SDK_ROOT)
    message(FATAL_ERROR "Set VIEWTRIOUS_3DXWARE_SDK_ROOT to a 3DxWare SDK installation.")
endif()
set(VIEWTRIOUS_OCCT_INCLUDE_DIR "${VIEWTRIOUS_OCCT_ROOT}/inc")
set(VIEWTRIOUS_OCCT_LIBRARY_DIR "${VIEWTRIOUS_OCCT_ROOT}/win64/vc14/lib")
set(VIEWTRIOUS_OCCT_BINARY_DIR "${VIEWTRIOUS_OCCT_ROOT}/win64/vc14/bin")
set(VIEWTRIOUS_OCCT_TOOLKITS
    TKBO TKBRep TKCAF TKCDF TKDE TKDESTEP TKernel TKG2d TKG3d TKGeomAlgo
    TKGeomBase TKHLR TKLCAF TKMath TKMesh TKPrim TKService TKShHealing
    TKTopAlgo TKV3d TKVCAF TKXCAF TKXSBase)
if(NOT VIEWTRIOUS_OCCT_ROOT OR NOT EXISTS "${VIEWTRIOUS_OCCT_INCLUDE_DIR}/STEPCAFControl_Reader.hxx" OR NOT EXISTS "${VIEWTRIOUS_OCCT_LIBRARY_DIR}/TKDESTEP.lib")
    message(FATAL_ERROR "Set VIEWTRIOUS_OCCT_ROOT to an OCCT 8.0.1 x64 dynamic install containing inc and win64/vc14/lib.")
endif()
set(VIEWTRIOUS_3DXWARE_INCLUDE_DIR "${VIEWTRIOUS_3DXWARE_SDK_ROOT}/inc")
set(VIEWTRIOUS_3DXWARE_LIBRARY "${VIEWTRIOUS_3DXWARE_SDK_ROOT}/lib/x64/TDxNavLib.lib")
if(NOT EXISTS "${VIEWTRIOUS_3DXWARE_INCLUDE_DIR}/SpaceMouse/CNavigation3D.hpp" OR NOT EXISTS "${VIEWTRIOUS_3DXWARE_LIBRARY}")
    message(FATAL_ERROR "VIEWTRIOUS_3DXWARE_SDK_ROOT must contain inc/SpaceMouse/CNavigation3D.hpp and lib/x64/TDxNavLib.lib.")
endif()

string(REPLACE "." "," PROJECT_VERSION_COMMAS "${PROJECT_VERSION}")
configure_file(assets/Viewtrious.ico "${CMAKE_CURRENT_BINARY_DIR}/Viewtrious.ico" COPYONLY)
configure_file(src/Viewtrious.rc.in "${CMAKE_CURRENT_BINARY_DIR}/Viewtrious.rc" @ONLY)

add_executable(Viewtrious WIN32
    src/main.cpp
    src/lanczos_resampler.cpp
    src/model_camera.cpp
    src/stl_loader.cpp
    src/three_mf_loader.cpp
    src/step_loader.cpp
    src/three_mf_zip.cpp
    src/d3d11_model_viewport.cpp
    src/graphics_host.cpp
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
target_include_directories(Viewtrious PRIVATE "${VIEWTRIOUS_3DXWARE_INCLUDE_DIR}")
target_include_directories(Viewtrious PRIVATE "${VIEWTRIOUS_OCCT_INCLUDE_DIR}")
target_include_directories(Viewtrious PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz")
target_link_directories(Viewtrious PRIVATE "${VIEWTRIOUS_OCCT_LIBRARY_DIR}")
target_link_libraries(Viewtrious PRIVATE d2d1 dwrite gdiplus windowscodecs shell32 shlwapi propsys dwmapi advapi32 d3d11 dxgi d3dcompiler xmllite ole32 "${VIEWTRIOUS_3DXWARE_LIBRARY}" ${VIEWTRIOUS_OCCT_TOOLKITS})
foreach(occt_toolkit IN LISTS VIEWTRIOUS_OCCT_TOOLKITS)
    add_custom_command(TARGET Viewtrious POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VIEWTRIOUS_OCCT_BINARY_DIR}/${occt_toolkit}.dll" "$<TARGET_FILE_DIR:Viewtrious>/${occt_toolkit}.dll")
endforeach()
target_compile_features(ViewtriousLanczosBenchmark PRIVATE cxx_std_20)
target_compile_definitions(ViewtriousLanczosBenchmark PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(ViewtriousLanczosBenchmark PRIVATE windowscodecs ole32)

if(MSVC)
    target_compile_options(Viewtrious PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(Viewtrious PRIVATE $<$<CONFIG:Release>:/O2>)
    target_compile_options(ViewtriousLanczosBenchmark PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(ViewtriousLanczosBenchmark PRIVATE $<$<CONFIG:Release>:/O2>)
endif()

set_source_files_properties(third_party/miniz/miniz_tinfl.c PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "/wd4334")
target_sources(Viewtrious PRIVATE third_party/miniz/miniz_tinfl.c)





