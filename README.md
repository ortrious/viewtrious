# mediaView

`mediaView` is a deliberately small Windows-native image viewer focused on fast launch, direct image display, and immediate shutdown. It has no background process, network activity, settings system, or bundled codec library.

## Current scope

- Opens an image path supplied on the command line.
- Fits and centers the image in a resizable native window with a dark background.
- Closes with `Esc` and accepts a replacement image via drag-and-drop.
- Navigates sibling images with the Left and Right arrow keys (natural filename order).
- Zooms with the mouse wheel or `+`/`-`, returns to fit with `0`, and pans zoomed images by dragging with the left mouse button.
- Uses Windows Imaging Component (WIC), supporting JPEG/JPG, PNG, BMP, GIF, TIFF/TIF, and ICO. Other installed WIC codecs (such as WebP, HEIF, or AVIF) work automatically when available.
- Gracefully shows an in-window error for unsupported or corrupt images.

Video, image navigation, zooming, editing, file associations, metadata, and persistent preferences are intentionally deferred.

## Build prerequisites

- Windows 11 (Windows 10 SDK also provides the required native APIs)
- Visual Studio 2022 Build Tools or Visual Studio with the **Desktop development with C++** workload
- CMake 3.21 or newer

From a Developer PowerShell for Visual Studio:

```powershell
cmake -S . -B build/debug
cmake --build build/debug --config Debug

cmake -S . -B build/release
cmake --build build/release --config Release
```

Run a built executable with a quoted path, as Explorer would:

```powershell
.\build\release\Release\mediaView.exe "C:\path\to\image.jpg"
```

No file associations are changed by the build or application.

## Development timing

Debug builds write startup timing checkpoints to the Visual Studio debugger output: decode start/completion, rendering/window initialization, and first successful image presentation. Release builds compile this instrumentation out completely.

The initial startup path performs only command-line parsing, COM/WIC/Direct2D initialization, image decode, and window creation. Benchmark before introducing additional startup work.
