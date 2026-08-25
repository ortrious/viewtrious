# FeatherView handoff

- Current version: `0.2.5.0` (CMake is the source of truth; the executable embeds matching VERSIONINFO metadata).
- Branch: `master`.
- Purpose: FeatherView is a tiny Windows-native image viewer optimized for rapid startup, first presentation, and shutdown.
- Architecture: C++20, Win32, WIC decoding, and Direct2D rendering; no framework or background process. The icon is embedded; the logo asset is retained for future About UI use and is not decoded during normal viewing.
- Build: `cmake -S . -B out/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug; cmake --build out/debug` and the equivalent `out/release` command with `Release`.
- Canonical executables: `out\debug\FeatherView.exe` and `out\release\FeatherView.exe`. Do not change these paths between versions.
- Transparency: alpha images render over a subtle DPI-aware checkerboard clipped to their transformed image rectangle; the plain viewer canvas remains outside.
- Top bar: a 40-DIP custom title bar contains a full-height hamburger, decoded resolution, cheap `GetFileAttributesExW` file-size metadata, and an ellipsized filename, followed by custom caption controls. Separators use a shared DPI-aware section gutter; the hamburger glyph is centered within its visual compartment, and the filename has a stable post-separator gutter. The hamburger opens a FeatherView-rendered, theme-aware `Keyboard Shortcuts`, separator, `About`, `Close` dropdown with vertically centered labels.
- Overlays: Keyboard Shortcuts and About are compact, centered, theme-aware Direct2D panels. Shortcut panel height is derived from title, row count, row height, and shared padding; rows use stable shortcut/description columns. About is a wider content-driven card with a centered, aspect-preserved full logo and `Extremely lightweight image viewer`. Esc dismisses the dropdown, then an open overlay, before fullscreen exit or app close; transient input does not reach viewer pan, zoom, navigation, or fullscreen actions. The About logo is decoded only when that overlay opens.
- Image actions: right-clicking a loaded image opens a FeatherView-rendered context menu with content-driven height. `Open With >` is a true toggle; its child stays open over its parent row, the child, and the short bridge between them, while moving to a different parent row closes only the child and restores parent hover/click handling. Escape closes the child first. Open With lazily enumerates Shell-recommended handlers via `SHAssocEnumHandlers`/`IAssocHandler`, invokes the chosen handler with the current file data object, and retains `Choose another app` (`SHOpenWithDialog`) as fallback. Copy (alpha-capable top-down `CF_DIBV5` pixels) and Print (Shell `print` verb) are enabled. Rotate Left/Right are enabled for JPEG and PNG: JPEG rotation updates only writable `System.Photo.Orientation`/EXIF orientation metadata via the Windows Property System, never re-encodes JPEG pixels, and decoding applies that orientation for display; PNG rotation uses WIC to produce an alpha-capable 90-degree rotated PNG in a sibling temporary file, commits it, then uses `ReplaceFileW` so the original is retained on failure. The JPEG four-turn SHA-256 check has not yet been run because the repository has no disposable JPEG fixture. Delete uses `IFileOperation` with `FOFX_RECYCLEONDELETE`; on success it removes the item from the current sibling list and loads the next item, otherwise the previous item, or returns to the normal empty state. `Delete` and the context-menu action share this path. Set as Background and Set as Lock Screen remain disabled. Ctrl+C and Ctrl+P use the same action paths. Successful copy displays a centered 200-DIP FeatherView-blue, rounded overlapping-squares indicator with white outlines and outlined `Copied to Clipboard`, fading from 100% to zero in about one second using a timer that exists only during feedback.
- Fullscreen: F11 or double-click toggles borderless current-monitor fullscreen; Esc exits fullscreen before closing.
- Controls: Left/Right navigate siblings; wheel and `+`/`-` zoom; `0` returns to the centered base scale; left drag pans any image; drop replaces; Esc exits. Base scale is `min(1.0, fitScale)`, so small images begin at native 1:1 size.
- Placement: normal rectangle and maximized state persist under `HKCU\Software\FeatherView`; invalid off-screen placements are moved onto the nearest monitor.
- Versioning: `major.minor.feature.fix`; features increment `feature`, fixes increment `fix`. Commit every implementation pass separately.
- Performance principle: first requested image wins; sibling discovery happens only after first presentation or lazily after a dropped image.
- Deferred: personalization actions, video, editing, thumbnails, settings framework, and file-association installation.








