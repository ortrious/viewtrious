cmake_minimum_required(VERSION 3.21)

project(Viewtrious VERSION 0.9.19.6 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Viewtrious currently targets Windows only.")
endif()

set(VIEWTRIOUS_3DXWARE_SDK_ROOT "" CACHE PATH "Root of the 3Dconnexion 3DxWare SDK (required for SpaceMouse support)")
option(VIEWTRIOUS_BUILD_AI_ADDON "Build the optional local AI adjustment add-on" OFF)
set(VIEWTRIOUS_ONNXRUNTIME_ROOT "" CACHE PATH "Root of the ONNX Runtime SDK (required only for the AI add-on)")
if(NOT VIEWTRIOUS_3DXWARE_SDK_ROOT)
    message(FATAL_ERROR "Set VIEWTRIOUS_3DXWARE_SDK_ROOT to a 3DxWare SDK installation.")
endif()
set(VIEWTRIOUS_3DXWARE_INCLUDE_DIR "${VIEWTRIOUS_3DXWARE_SDK_ROOT}/inc")
set(VIEWTRIOUS_3DXWARE_LIBRARY "${VIEWTRIOUS_3DXWARE_SDK_ROOT}/lib/x64/TDxNavLib.lib")
if(NOT EXISTS "${VIEWTRIOUS_3DXWARE_INCLUDE_DIR}/SpaceMouse/CNavigation3D.hpp" OR NOT EXISTS "${VIEWTRIOUS_3DXWARE_LIBRARY}")
    message(FATAL_ERROR "VIEWTRIOUS_3DXWARE_SDK_ROOT must contain inc/SpaceMouse/CNavigation3D.hpp and lib/x64/TDxNavLib.lib.")
endif()

string(REPLACE "." "," PROJECT_VERSION_COMMAS "${PROJECT_VERSION}")
configure_file(assets/Viewtrious.ico "${CMAKE_CURRENT_BINARY_DIR}/Viewtrious.ico" COPYONLY)
configure_file(assets/ViewtriousVideo.ico "${CMAKE_CURRENT_BINARY_DIR}/ViewtriousVideo.ico" COPYONLY)
configure_file(assets/Viewtrious3D.ico "${CMAKE_CURRENT_BINARY_DIR}/Viewtrious3D.ico" COPYONLY)
configure_file(src/Viewtrious.rc.in "${CMAKE_CURRENT_BINARY_DIR}/Viewtrious.rc" @ONLY)

add_executable(Viewtrious WIN32
    src/main.cpp
    src/lanczos_resampler.cpp
    src/model_camera.cpp
    src/stl_loader.cpp
    src/three_mf_loader.cpp
    src/three_mf_zip.cpp
    src/d3d11_model_viewport.cpp
    src/graphics_host.cpp
    src/media_adjustments.cpp
    src/adjustment_persistence.cpp
    src/ai_addon_loader.cpp
    src/file_open_diagnostics.cpp
    src/video_player.cpp
    src/shell_thumbnail_reader.cpp
    src/video_hover_frame_stream.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/Viewtrious.rc"
)

# Investigation-only command-line harness.  It is intentionally not linked into
# Viewtrious so the production Direct2D canvas remains unchanged.
add_executable(ViewtriousLanczosBenchmark
    src/lanczos_benchmark.cpp
    src/lanczos_resampler.cpp
)

add_library(ViewtriousStlThumbnail SHARED
    shell/stl_thumbnail_provider.cpp
    shell/ViewtriousStlThumbnail.rc
    shell/ViewtriousStlThumbnail.def
)
set_target_properties(ViewtriousStlThumbnail PROPERTIES OUTPUT_NAME ViewtriousStlThumbnail)
add_custom_command(TARGET ViewtriousStlThumbnail POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/shell/Register-ViewtriousStlThumbnail.ps1" "$<TARGET_FILE_DIR:ViewtriousStlThumbnail>/Register-ViewtriousStlThumbnail.ps1")
target_compile_features(ViewtriousStlThumbnail PRIVATE cxx_std_20)
target_compile_definitions(ViewtriousStlThumbnail PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX VIEWTRIOUS_VERSION=L"${PROJECT_VERSION}")
target_link_libraries(ViewtriousStlThumbnail PRIVATE ole32 uuid gdi32)

target_compile_features(Viewtrious PRIVATE cxx_std_20)
target_compile_definitions(Viewtrious PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX VIEWTRIOUS_VERSION=L\"${PROJECT_VERSION}\")
target_include_directories(Viewtrious PRIVATE "${VIEWTRIOUS_3DXWARE_INCLUDE_DIR}")
target_include_directories(Viewtrious PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz")
file(GLOB VIEWTRIOUS_CPPWINRT_INCLUDE_CANDIDATES
    "$ENV{WindowsSdkDir}Include/$ENV{WindowsSDKVersion}cppwinrt"
    "C:/Program Files (x86)/Windows Kits/10/Include/*/cppwinrt")
find_path(VIEWTRIOUS_CPPWINRT_INCLUDE_DIR winrt/base.h
    HINTS ${VIEWTRIOUS_CPPWINRT_INCLUDE_CANDIDATES})
if(NOT VIEWTRIOUS_CPPWINRT_INCLUDE_DIR)
    message(FATAL_ERROR "The installed Windows SDK C++/WinRT projection is required for the Windows.UI.Composition experiment.")
endif()
target_include_directories(Viewtrious PRIVATE "${VIEWTRIOUS_CPPWINRT_INCLUDE_DIR}")
target_link_libraries(Viewtrious PRIVATE d2d1 dwrite gdiplus windowscodecs shell32 shlwapi propsys dwmapi advapi32 bcrypt d3d11 dxgi d3dcompiler dcomp xmllite ole32 oleaut32 mfplat mf mfreadwrite mfuuid windowsapp coremessaging "${VIEWTRIOUS_3DXWARE_LIBRARY}")

if(VIEWTRIOUS_BUILD_AI_ADDON)
    set(VIEWTRIOUS_ONNXRUNTIME_INCLUDE_DIR "${VIEWTRIOUS_ONNXRUNTIME_ROOT}/include")
    set(VIEWTRIOUS_ONNXRUNTIME_LIBRARY "${VIEWTRIOUS_ONNXRUNTIME_ROOT}/lib/onnxruntime.lib")
    if(NOT VIEWTRIOUS_ONNXRUNTIME_ROOT OR NOT EXISTS "${VIEWTRIOUS_ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h" OR NOT EXISTS "${VIEWTRIOUS_ONNXRUNTIME_LIBRARY}")
        message(FATAL_ERROR "VIEWTRIOUS_BUILD_AI_ADDON requires VIEWTRIOUS_ONNXRUNTIME_ROOT with the ONNX Runtime SDK.")
    endif()
    add_library(ViewtriousAI SHARED addons/ai/viewtrious_ai_addon.cpp)
    target_compile_features(ViewtriousAI PRIVATE cxx_std_20)
    target_compile_definitions(ViewtriousAI PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(ViewtriousAI PRIVATE src "${VIEWTRIOUS_ONNXRUNTIME_INCLUDE_DIR}")
    target_link_libraries(ViewtriousAI PRIVATE "${VIEWTRIOUS_ONNXRUNTIME_LIBRARY}")
    if(MSVC)
        target_compile_options(ViewtriousAI PRIVATE /W4 /permissive- /EHsc)
    endif()
    set_target_properties(ViewtriousAI PROPERTIES OUTPUT_NAME ViewtriousAI RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:Viewtrious>/Addons/AI")
    add_custom_command(TARGET ViewtriousAI POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VIEWTRIOUS_ONNXRUNTIME_ROOT}/lib/onnxruntime.dll" "$<TARGET_FILE_DIR:ViewtriousAI>/onnxruntime.dll" COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/addons/ai/THIRD_PARTY_NOTICES.txt" "$<TARGET_FILE_DIR:ViewtriousAI>/THIRD_PARTY_NOTICES.txt")
endif()
target_compile_features(ViewtriousLanczosBenchmark PRIVATE cxx_std_20)
target_compile_definitions(ViewtriousLanczosBenchmark PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(ViewtriousLanczosBenchmark PRIVATE windowscodecs ole32)

if(MSVC)
    target_compile_options(Viewtrious PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(Viewtrious PRIVATE $<$<CONFIG:Release>:/O2 /Gy /Gw>)
    target_link_options(Viewtrious PRIVATE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
    target_compile_options(ViewtriousLanczosBenchmark PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(ViewtriousStlThumbnail PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(ViewtriousLanczosBenchmark PRIVATE $<$<CONFIG:Release>:/O2>)
endif()

set_source_files_properties(third_party/miniz/miniz_tinfl.c PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "/wd4334")
target_sources(Viewtrious PRIVATE third_party/miniz/miniz_tinfl.c)





