# FeatherView handoff

- Current version: `0.2.3.0` (CMake is the source of truth; the executable embeds matching VERSIONINFO metadata).
- Branch: `master`.
- Purpose: FeatherView is a tiny Windows-native image viewer optimized for rapid startup, first presentation, and shutdown.
- Architecture: C++20, Win32, WIC decoding, and Direct2D rendering; no framework or background process. The icon is embedded; the logo asset is retained for future About UI use and is not decoded during normal viewing.
- Build: `cmake -S . -B out/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug; cmake --build out/debug` and the equivalent `out/release` command with `Release`.
- Canonical executables: `out\debug\FeatherView.exe` and `out\release\FeatherView.exe`. Do not change these paths between versions.
- Transparency: alpha images render over a subtle DPI-aware checkerboard clipped to their transformed image rectangle; the plain viewer canvas remains outside.
- Top bar: a 40-DIP custom title bar contains a full-height hamburger, decoded resolution, cheap `GetFileAttributesExW` file-size metadata, and an ellipsized filename, followed by custom caption controls. Separators use a shared DPI-aware section gutter; the hamburger glyph is centered within its visual compartment, and the filename has a stable post-separator gutter. The hamburger opens a FeatherView-rendered, theme-aware `Keyboard Shortcuts`, separator, `About`, `Close` dropdown with vertically centered labels.
- Overlays: Keyboard Shortcuts and About are compact, centered, theme-aware Direct2D panels. Shortcut panel height is derived from title, row count, row height, and shared padding; rows use stable shortcut/description columns. About is a wider content-driven card with a centered, aspect-preserved full logo and `Extremely lightweight image viewer`. Esc dismisses the dropdown, then an open overlay, before fullscreen exit or app close; transient input does not reach viewer pan, zoom, navigation, or fullscreen actions. The About logo is decoded only when that overlay opens.
- Image actions: right-clicking a loaded image opens a FeatherView-rendered context menu. Open With (`SHOpenWithDialog`), Copy (alpha-capable top-down `CF_DIBV5` pixels), and Print (Shell `print` verb) are enabled; rotate, personalization, and Delete remain visibly disabled. Ctrl+C and Ctrl+P use the same action paths. Successful copy displays a centered duplicate-glyph fade for about one second using a timer that exists only during feedback.
- Fullscreen: F11 or double-click toggles borderless current-monitor fullscreen; Esc exits fullscreen before closing.
- Controls: Left/Right navigate siblings; wheel and `+`/`-` zoom; `0` returns to the centered base scale; left drag pans any image; drop replaces; Esc exits. Base scale is `min(1.0, fitScale)`, so small images begin at native 1:1 size.
- Placement: normal rectangle and maximized state persist under `HKCU\Software\FeatherView`; invalid off-screen placements are moved onto the nearest monitor.
- Versioning: `major.minor.feature.fix`; features increment `feature`, fixes increment `fix`. Commit every implementation pass separately.
- Performance principle: first requested image wins; sibling discovery happens only after first presentation or lazily after a dropped image.
- Deferred: rotation, delete, personalization actions, video, editing, thumbnails, settings framework, and file-association installation.








