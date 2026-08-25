# FeatherView handoff

- Current version: `0.2.2.1` (CMake is the source of truth; the executable embeds matching VERSIONINFO metadata).
- Branch: `master`.
- Purpose: FeatherView is a tiny Windows-native image viewer optimized for rapid startup, first presentation, and shutdown.
- Architecture: C++20, Win32, WIC decoding, and Direct2D rendering; no framework or background process. The icon is embedded; the logo asset is retained for future About UI use and is not decoded during normal viewing.
- Build: `cmake -S . -B out/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug; cmake --build out/debug` and the equivalent `out/release` command with `Release`.
- Canonical executables: `out\debug\FeatherView.exe` and `out\release\FeatherView.exe`. Do not change these paths between versions.
- Transparency: alpha images render over a subtle DPI-aware checkerboard clipped to their transformed image rectangle; the plain viewer canvas remains outside.
- Top bar: a 40-DIP custom title bar contains a full-height hamburger, decoded resolution, cheap `GetFileAttributesExW` file-size metadata, and an ellipsized filename, followed by custom caption controls. Resolution and file size use stable centered slots; the filename takes remaining width and is trimmed first. The hamburger opens a FeatherView-rendered, theme-aware `Keyboard Shortcuts`, separator, `About`, `Close` dropdown.
- Overlays: Keyboard Shortcuts and About are compact, centered, theme-aware Direct2D panels. The shortcuts use stable shortcut/description columns. About preserves the centered full-logo aspect ratio and shows `Extremely lightweight image viewer`. Esc dismisses the dropdown, then an open overlay, before fullscreen exit or app close; transient input does not reach viewer pan, zoom, navigation, or fullscreen actions. The About logo is decoded only when that overlay opens.
- Fullscreen: F11 or double-click toggles borderless current-monitor fullscreen; Esc exits fullscreen before closing.
- Controls: Left/Right navigate siblings; wheel and `+`/`-` zoom; `0` returns to the centered base scale; left drag pans any image; drop replaces; Esc exits. Base scale is `min(1.0, fitScale)`, so small images begin at native 1:1 size.
- Placement: normal rectangle and maximized state persist under `HKCU\Software\FeatherView`; invalid off-screen placements are moved onto the nearest monitor.
- Versioning: `major.minor.feature.fix`; features increment `feature`, fixes increment `fix`. Commit every implementation pass separately.
- Performance principle: first requested image wins; sibling discovery happens only after first presentation or lazily after a dropped image.
- Deferred: image-action context menu, video, editing, thumbnails, settings framework, and file-association installation.








