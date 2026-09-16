<p align="center">
  <img src="docs/images/viewtrious-icon.png" width="140" alt="viewtrious">
</p>

<h1 align="center">viewtrious</h1>

<p align="center">
  <strong>extremely lightweight media viewer for Windows 11</strong>
</p>

<p align="center">
  images · animated GIFs · video · STL · 3MF
</p>

<p align="center">
  <img src="docs/images/viewtrious-viewer.png" width="100%" alt="viewtrious image viewer with filmstrip and adjustments">
</p>

viewtrious is a native Windows viewer built around fast launch, direct presentation, and a small footprint rather than a media-library workflow.

The base viewer uses Windows-native graphics, media, shell, and persistence components wherever practical. It has no background service, does not require a bundled codec framework, and keeps the main viewing experience local to the machine.

## Media families

<p align="center">
  <img src="docs/images/viewtrious-icon.png" width="96" alt="Images and GIFs">
  &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
  <img src="docs/images/viewtrious-video-icon.png" width="96" alt="Video">
  &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
  <img src="docs/images/viewtrious-3d-icon.png" width="96" alt="3D">
</p>

<p align="center">
  <strong>Images &amp; GIFs</strong>
  &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
  <strong>Video</strong>
  &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
  <strong>STL &amp; 3MF</strong>
</p>

## Highlights

- Native C++20 / Win32 application with one main window per process.
- Images, animated GIFs, video, STL, and 3MF in one viewer.
- Fast same-folder navigation with a filmstrip that follows the selected media.
- Zoom, pan, fit/actual-pixels viewing, fullscreen, drag-and-drop, clipboard, print, and Recycle Bin workflows.
- Shared image/video adjustment panel with Exposure, Brightness, Contrast, Shadows, Highlights, Saturation, and Sharpness.
- Deterministic Auto adjustments, Original preview, user presets, and optional per-media adjustment persistence.
- Media-aware custom context menus and Windows-native file associations.
- Per-user settings and adjustment data stored in WinSQLite under `%LOCALAPPDATA%\viewtrious\data`.
- 3Dconnexion SpaceMouse support for Model3D navigation.
- No STEP/STP or OCCT runtime.

## Images and animated GIFs

viewtrious uses Windows Imaging Component (WIC) for still-image decoding and supports the common formats exposed through its current Windows integration, including JPEG, PNG, BMP, HEIC/HEIF, DNG, and GIF.

Image features include:

- Fit / actual-pixels viewing with smooth zoom and pan.
- Safe JPEG/PNG rotation with replacement only after the new file has been validated.
- Non-destructive image adjustments.
- Content-hash-backed adjustment persistence without modifying the original media.
- HEIC/HEIF filmstrip thumbnails using embedded thumbnails when available, with full-frame fallback.
- Animated GIF playback with play/pause and frame stepping.
- Set as Desktop Background with a Windows-compatible fallback when the original source cannot be used directly.

## Filmstrip

The filmstrip is designed for fast browsing without turning viewtrious into a media-library application.

- Centers around the selected media when selection changes.
- Preserves manual wheel/drag positioning until the next selection or folder-membership change.
- Supports `hidden`, `automatic`, and `always show` visibility modes.
- Refreshes as files are added to or removed from the viewed folder.
- Still-image/GIF hover remains subtle and stays inside the filmstrip.
- Video siblings can play a silent one-shot moving preview directly inside their thumbnail.
- Downward canvas swipe can dismiss the transient filmstrip while swipe navigation is enabled.

## Video

Video playback uses Windows Media Foundation and the existing Direct3D/Direct2D presentation path.

Features include:

- Play/pause, seeking, elapsed/total time, volume, mute, and playback speed.
- Previous/next frame controls.
- Fullscreen and zoom/pan.
- Shared image/video adjustments and persistence.
- Auto-play Next with cancellable countdown.
- Save the currently displayed video frame as PNG.
- Same-folder navigation between images, GIFs, and videos.
- Source-derived resolution and frame-rate metadata in the top bar.

## 3D

Model3D supports **STL and 3MF**.

Features include:

- Orbit, pan, zoom, selection, Fit Selection, and Home/reset navigation.
- Orthographic/perspective control on the canvas.
- Configurable Build Plate and grid.
- Standard 3MF colors plus supported slicer material/color metadata.
- Multi-part/component hierarchy with synchronized viewport selection.
- Resilient loading of referenced 3MF model parts.
- 3Dconnexion SpaceMouse navigation.
- Optional per-user Explorer STL thumbnail provider.

STEP/STP is intentionally outside the 1.0 scope.

## Windows integration

viewtrious integrates with Windows without taking ownership of Windows-managed defaults.

- Per-user `RegisteredApplications` / Capabilities registration.
- Per-extension Viewtrious ProgIDs and Open With support.
- Separate image, video, and 3D icon families.
- Optional Explorer STL thumbnail provider.
- Windows Default Apps remains authoritative; viewtrious does not write or forge `UserChoice` data.

## Settings and data

Application preferences and per-media adjustment records share the Windows system WinSQLite database:

```text
%LOCALAPPDATA%\viewtrious\data\viewtrious.db
```

The database contains application settings, image/video adjustment state, and the file-hash cache used to reconnect adjustments after rename or move.

User-created exports such as saved video frames live under `Pictures\Viewtrious` and are treated as user content rather than disposable application data.

## Lightweight by design

viewtrious is intentionally useful even when opened with nothing loaded yet—no library scan, no background catalog, and no account required.

<p align="center">
  <img src="docs/images/viewtrious-empty.png" width="85%" alt="viewtrious empty state">
</p>

## Build from source

### Prerequisites

- Windows 11
- Visual Studio Build Tools with the **Desktop development with C++** workload
- A Windows SDK containing the C++/WinRT projection
- CMake 3.21 or newer
- Ninja
- 3Dconnexion 3DxWare SDK 4

From a Developer PowerShell for Visual Studio:

```powershell
cmake -S . -B out/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DVIEWTRIOUS_3DXWARE_SDK_ROOT="C:\path\to\3DxWare_SDK"
cmake --build out/debug

cmake -S . -B out/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DVIEWTRIOUS_3DXWARE_SDK_ROOT="C:\path\to\3DxWare_SDK"
cmake --build out/release
```

The canonical Release executable is:

```text
out\release\Viewtrious.exe
```

Debug builds emit selected startup and frame-pacing diagnostics to the debugger output. Release builds compile that instrumentation out.

## Architecture

The base viewer is built from:

- Win32
- Windows Imaging Component
- Direct2D
- Direct3D 11 / DXGI
- Windows Media Foundation
- Windows.UI.Composition
- WinSQLite
- 3Dconnexion NavLib integration
- miniz for ZIP/DEFLATE access used by 3MF packaging

The project intentionally avoids heavyweight media/CAD frameworks in the base application.

## Licensing

Viewtrious is **source-available** under the **Apache License 2.0 with the Commons Clause License Condition v1.0**.

Source may be viewed, modified, and redistributed subject to those terms. The Commons Clause restricts selling Viewtrious itself, or a product or service whose value derives entirely or substantially from Viewtrious functionality, as defined by the clause. Commercial and internal business use are not categorically prohibited.

See [LICENSE](LICENSE) for the controlling terms.

Copyright 2026 Dustin Wilson<br>
Published under the Ortrious brand.

Third-party components retain their own licenses and attribution requirements.
