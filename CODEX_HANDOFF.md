# FeatherView handoff

- Current version: `0.1.2.2` (CMake is the source of truth; the executable embeds matching VERSIONINFO metadata).
- Branch: `master`.
- Purpose: FeatherView is a tiny Windows-native image viewer optimized for rapid startup, first presentation, and shutdown.
- Architecture: C++20, Win32, WIC decoding, and Direct2D rendering; no framework or background process.
- Build: `cmake -S . -B out/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug; cmake --build out/debug` and the equivalent `out/release` command with `Release`.
- Canonical executables: `out\debug\FeatherView.exe` and `out\release\FeatherView.exe`. Do not change these paths between versions.
- Controls: Left/Right navigate siblings; wheel and `+`/`-` zoom; `0` returns to the centered base scale; left drag pans any image; drop replaces; Esc exits. Base scale is `min(1.0, fitScale)`, so small images begin at native 1:1 size.
- Placement: normal rectangle and maximized state persist under `HKCU\Software\FeatherView`; invalid off-screen placements are moved onto the nearest monitor.
- Versioning: `major.minor.feature.fix`; features increment `feature`, fixes increment `fix`. Commit every implementation pass separately.
- Performance principle: first requested image wins; sibling discovery happens only after first presentation or lazily after a dropped image.
- Deferred: buttons/menus, video, metadata, editing, thumbnails, settings framework, and file-association installation.
