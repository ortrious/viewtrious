# Viewtrious

`Viewtrious` is a deliberately lightweight Windows-native media viewer focused on fast launch, direct presentation, and immediate shutdown. It uses one main window, has no background process or network activity, and relies primarily on Windows media and graphics components rather than bundled codec frameworks.

## Current scope

- Opens images, animated GIFs, videos, STL models, and 3MF models.
- Uses WIC for still images and Windows Media Foundation for video playback.
- Renders with Direct2D and Direct3D 11 through the in-box Windows composition stack.
- Provides sibling-file navigation, filmstrip thumbnails, zoom/pan, fullscreen playback, image/video adjustments, and native drag-and-drop and clipboard workflows.
- Supports JPEG/PNG rotation with safe replacement and preserves the original file on failure.
- Stores per-user preferences and adjustment data in the Windows system WinSQLite component under `%LOCALAPPDATA%\viewtrious\data`.
- Registers per-user Windows Default Apps capabilities without modifying `UserChoice` values.
- Supports 3Dconnexion SpaceMouse input through the 3DxWare SDK/driver integration.
- Fails softly when an optional codec, media feature, AI add-on, or SpaceMouse runtime is unavailable.

STEP/STP and an OCCT runtime are not part of the current product.

## Licensing

Viewtrious is source-available under the Apache License 2.0 with the Commons Clause License Condition v1.0. Source may be viewed, modified, and redistributed subject to those terms. The Commons Clause restricts selling Viewtrious itself, or a product or service whose value derives entirely or substantially from Viewtrious functionality, as defined by the clause. Commercial and internal business use are not categorically prohibited.

See [LICENSE](LICENSE) for the controlling terms.

Copyright 2026 Dustin Wilson
Published under the Ortrious brand.

## Build prerequisites

- Windows 11
- Visual Studio Build Tools with the **Desktop development with C++** workload
- A Windows SDK containing the C++/WinRT projection
- CMake 3.21 or newer
- Ninja
- 3Dconnexion 3DxWare SDK 4

From a Developer PowerShell for Visual Studio, set the SDK path while configuring:

```powershell
cmake -S . -B out/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DVIEWTRIOUS_3DXWARE_SDK_ROOT="C:\path\to\3DxWare_SDK"
cmake --build out/debug

cmake -S . -B out/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DVIEWTRIOUS_3DXWARE_SDK_ROOT="C:\path\to\3DxWare_SDK"
cmake --build out/release
```

`out\release\Viewtrious.exe` is the canonical Release executable. The optional local AI adjustment add-on is disabled by default and has separate ONNX Runtime/model packaging and licensing requirements.

## Development timing

Debug builds write selected startup and frame-pacing diagnostics to the debugger output. Release builds compile this instrumentation out.
