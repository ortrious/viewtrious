# Viewtrious

`Viewtrious` is a deliberately small Windows-native image viewer focused on fast launch, direct image display, and immediate shutdown. It has no background process, network activity, settings framework, or bundled codec library.

## Current scope

- Opens an image path supplied on the command line.
- Fits and centers the image in a resizable native window with a dark background.
- Closes with `Esc` and accepts a replacement image via drag-and-drop.
- Navigates sibling images with the Left and Right arrow keys (natural filename order).
- Zooms with the mouse wheel or `+`/`-`, returns to base scale with `0`, and pans images freely by dragging with the left mouse button.
- Stops zoom-out at Fit to Window, supports free manual panning, and restores the last normal window placement (including maximized state).
- Uses the current Windows app light/dark preference for the native title bar.
- Uses Windows Imaging Component (WIC), supporting JPEG/JPG, PNG, BMP, GIF, TIFF/TIF, and ICO. Other installed WIC codecs (such as WebP, HEIF, or AVIF) work automatically when available.
- Gracefully shows an in-window error for unsupported or corrupt images.

Video playback and animated-image playback are intentionally outside the current scope. Viewtrious includes native menus, persisted viewer preferences, safe JPEG/PNG rotation, and per-user Windows Default Apps capability registration without modifying Windows `UserChoice` values.

## Build prerequisites

- Windows 11 (Windows 10 SDK also provides the required native APIs)
- Visual Studio 2022 Build Tools or Visual Studio with the **Desktop development with C++** workload
- CMake 3.21 or newer

From a Developer PowerShell for Visual Studio:

```powershell
cmake -S . -B out/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/debug

cmake -S . -B out/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/release
```

Run a built executable with a quoted path, as Explorer would:

```powershell
.\out\release\Viewtrious.exe "C:\path\to\image.jpg"
```

`out\release\Viewtrious.exe` is the canonical Release output. Viewtrious refreshes its per-user Default Apps capability registration at startup so a copied portable executable can repair its registered command and icon paths; users still choose actual defaults through Windows Settings.

## Development timing

Debug builds write startup timing checkpoints to the Visual Studio debugger output: decode start/completion, rendering/window initialization, and first successful image presentation. Release builds compile this instrumentation out completely.

The initial startup path performs only command-line parsing, COM/WIC/Direct2D initialization, image decode, and window creation. Benchmark before introducing additional startup work.
