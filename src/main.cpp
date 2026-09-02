#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include <shobjidl_core.h>
#include <shlwapi.h>
#include <propkey.h>
#include <propsys.h>
#include <dwmapi.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <gdiplus.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <SpaceMouse/CNavigation3D.hpp>

#include "lanczos_resampler.h"
#include "d3d11_model_viewport.h"
#include "video_player.h"
#include "stl_loader.h"
#include "three_mf_loader.h"
#include "model_importer.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdiplus.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

constexpr wchar_t kWindowClass[] = L"ViewtriousWindow";
constexpr wchar_t kWindowTitle[] = L"Viewtrious";
constexpr UINT kBuildNavigationMessage = WM_APP + 1;
constexpr UINT kDirectoryChangedMessage = WM_APP + 3;
constexpr UINT kFullDecodeCompleteMessage = WM_APP + 4;
constexpr UINT kDecodeWorkerFinishedMessage = WM_APP + 6;
constexpr UINT kLanczosCompleteMessage = WM_APP + 7;
constexpr UINT kModelLoadCompleteMessage = WM_APP + 8;
constexpr UINT kVideoMediaEngineEventMessage = WM_APP + 9;
constexpr UINT_PTR kCopyFeedbackTimer = 1;
constexpr UINT_PTR kCanvasNavigationFadeTimer = 2;
constexpr UINT_PTR kDirectoryChangeDebounceTimer = 4;
constexpr UINT_PTR kNavigationDecodeDebounceTimer = 5;
constexpr UINT_PTR kShellRotationCheckTimer = 6;
constexpr UINT_PTR kLanczosSettleTimer = 7;
constexpr UINT_PTR kHeifRotationMenuRefreshTimer = 8;
constexpr UINT_PTR kGifPlaybackTimer = 10;
constexpr UINT_PTR kModelHomeAnimationTimer = 11;
constexpr UINT_PTR kModelLoadingAnimationTimer = 12;
constexpr UINT_PTR kTriangleCountTooltipTimer = 13;
constexpr UINT_PTR kVideoPlaybackTimer = 14;
constexpr UINT_PTR kVideoControlsTimer = 15;
constexpr UINT kShellRotationCheckIntervalMs = 100;
constexpr ULONGLONG kShellRotationTimeoutMs = 10000;
constexpr ULONGLONG kHeifRotationCooldownMs = 0;
constexpr int kTopBarLogoResourceId = 102;
constexpr int kAboutLogoResourceId = 103;
constexpr int kContextMenuRowCount = 8;
constexpr int kContextMenuSeparatorCount = 4;
constexpr int kContextMenuPaddingDip = 8;
constexpr float kMaximumZoom = 16.0f;
constexpr float kWheelZoomStep = 1.11f;
constexpr ULONGLONG kModelHomeAnimationDurationMs = 240;
constexpr ULONGLONG kModelLoadingOverlayDelayMs = 150;
constexpr UINT kTriangleCountTooltipDelayMs = 450;
constexpr ULONGLONG kVideoControlsIdleDelayMs = 1500;
constexpr ULONGLONG kVideoControlsFadeDurationMs = 500;
// Shared Settings grid geometry. Every page uses these values for section and control placement.
constexpr float kSettingsContentLeftPaddingDips = 206.0f;
constexpr float kSettingsContentRightPaddingDips = 18.0f;
constexpr float kSettingsColumnGapDips = 24.0f;
constexpr float kSettingsRowGapDips = 12.0f;
constexpr float kSettingsCheckboxRowGapDips = 6.0f;
constexpr float kSettingsLabelToControlGapDips = 6.0f;
constexpr float kSettingsMajorSectionGapDips = 24.0f;
constexpr float kSettingsControlHeightDips = 32.0f;
constexpr float kSettingsSectionHeadingHeightDips = 20.0f;
constexpr float kSettingsLabelHeightDips = 20.0f;
constexpr float kSettingsRowHeightDips = kSettingsLabelHeightDips + kSettingsLabelToControlGapDips + kSettingsControlHeightDips;
constexpr float kSettingsFirstRowTopDips = 106.0f;
constexpr int kDropdownLeftPaddingDips = 12;
constexpr int kDropdownChevronReserveDips = 28;
constexpr int kDropdownChevronHalfWidthDips = 4;
constexpr int kDropdownChevronHalfHeightDips = 2;
constexpr std::array<const wchar_t*, 6> kAntiAliasingOptions{ L"Off", L"2x MSAA", L"4x MSAA", L"8x MSAA", L"1.5x SSAA", L"2x SSAA" };
constexpr std::array<const wchar_t*, 7> kAntiAliasingDisplayOptions{ L"Off", L"2x MSAA", L"4x MSAA", L"8x MSAA", L"1.5x SSAA", L"2x SSAA", L"8x MSAA (Wireframe fallback)" };
constexpr std::array<const wchar_t*, 2> kProjectionOptions{ L"Perspective", L"Orthographic" };
constexpr std::array<const wchar_t*, 3> kVisualStyleOptions{ L"Shaded", L"Shaded with Visible Edges", L"Wireframe" };
constexpr wchar_t kSettingsKey[] = L"Software\\Viewtrious";
constexpr wchar_t kRegisteredApplicationName[] = L"Viewtrious";
constexpr wchar_t kCapabilitiesPath[] = L"Software\\Viewtrious\\Capabilities";
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
const D2D1_COLOR_F kViewerBackground = D2D1::ColorF(26.0f / 255.0f, 26.0f / 255.0f, 26.0f / 255.0f);


enum class OverlayKind { None, KeyboardShortcuts, About, Settings, ResetConfirm, DeleteConfirm, Welcome, DefaultAppsHelper, Feedback };
enum class DropdownItem { None, OpenFile, Settings, QuickTour, KeyboardShortcuts, About, Feedback, Close };
enum class ContextAction { None, Fullscreen, RotateLeft, RotateRight, OpenWith, Copy, Print, SetBackground, Delete, SnapViewToFace };
enum class ButtonKind { None, EmptyOpenFile, CanvasPrevious, CanvasNext, SettingsGeneralPage, SettingsImage2DPage, SettingsModel3DPage, SettingsRememberPlacement, SettingsIncludeHidden,
    SettingsConfirmDelete, SettingsShowZoomHud, SettingsAnimations, SettingsReverseWheelZoom, SettingsThemeSystem, SettingsThemeLight, SettingsThemeDark,
    SettingsZoomHudPositionToggle, SettingsZoomHudBottomLeft, SettingsZoomHudBottomRight, SettingsZoomHudTopLeft, SettingsZoomHudTopRight, SettingsImageScalingToggle, SettingsScrollUp, SettingsScrollDown,
    SettingsSpaceMouse, SettingsUpAxisToggle, SettingsUpAxisZ, SettingsUpAxisY, SettingsUpAxisX, SettingsBuildPlateToggle, SettingsBuildPlateAuto, SettingsBuildPlateOn, SettingsBuildPlateOff, SettingsAxisIndicatorPositionToggle, SettingsAxisIndicatorBottomLeft, SettingsAxisIndicatorBottomRight, SettingsAxisIndicatorTopLeft, SettingsAxisIndicatorTopRight, SettingsProjectionToggle, SettingsProjectionPerspective, SettingsProjectionOrthographic, SettingsGraphicsAdapterToggle, SettingsGraphicsAdapterOption, SettingsAntiAliasingToggle, SettingsAntiAliasingOff, SettingsAntiAliasing2x, SettingsAntiAliasing4x, SettingsAntiAliasing8x, SettingsAntiAliasingSsaa1_5x, SettingsAntiAliasingSsaa2x, ModelOffscreenIndicator, ViewBarProjectionToggle, ViewBarProjectionPerspective, ViewBarProjectionOrthographic, ViewBarVisualStyleToggle, ViewBarVisualStyleShaded, ViewBarVisualStyleVisibleEdges, ViewBarVisualStyleWireframe, SettingsScalingPerformance, SettingsScalingQuality, SettingsDefaultApps, SettingsReset, ResetCancel, ResetConfirm, DeleteWarningSuppress, DeleteCancel, DeleteConfirm, WelcomeSecondary, WelcomePrimary, FeedbackBug,
    DefaultAppsHelperCancel, DefaultAppsHelperOpen, FeedbackFeature, TutorialSkip, TutorialNext, VideoPlayPause, VideoMute };
enum class TutorialStep { None, OpenImages, ResizeWindow, MenuSettings, ImageDetails, ContextMenu, Shortcuts };
enum class ThemePreference : DWORD { System = 0, Light = 1, Dark = 2 };
enum class ImageScaling : DWORD { Performance = 0, Quality = 1 };
enum class ModelRenderingApi : DWORD { Direct3D11 = 0 };
enum class AxisIndicatorPosition : DWORD { BottomLeft = 0, BottomRight = 1, TopLeft = 2, TopRight = 3 };
enum class ZoomHudPosition : DWORD { BottomLeft = 0, BottomRight = 1, TopLeft = 2, TopRight = 3 };
enum class SettingsPage { General, Image2D, Model3D };
enum class ContentKind { None, Image2D, Model3D, Video2D };

struct ShortcutEntry { const wchar_t* shortcut; const wchar_t* description; };
struct OpenWithHandler { std::wstring name; ComPtr<IAssocHandler> handler; };
struct PixelBuffer {
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    std::shared_ptr<std::vector<BYTE>> pixels;
};
struct DecodeRequest {
    std::wstring path;
    uint64_t requestGeneration = 0;
    uint64_t folderGeneration = 0;
};
struct FullDecodeResult : PixelBuffer {
    DecodeRequest request;
    HRESULT result = E_FAIL;
    std::thread::id workerId{};
    bool deliveredSynchronously = false;
};
struct DecodeWorkerFinished { std::thread::id workerId{}; };
struct ModelLoadResult { std::wstring path; uint64_t generation = 0; std::shared_ptr<ModelDocument> document; std::wstring error; bool IsSuccess() const { return document != nullptr; } };
struct ModelLoadWorker { uint64_t generation = 0; std::thread thread; };
struct LanczosRequest {
    std::shared_ptr<std::vector<BYTE>> sourcePixels;
    UINT sourceWidth = 0, sourceHeight = 0, targetWidth = 0, targetHeight = 0;
    uint64_t generation = 0;
    viewtrious::LanczosMapping mapping{};
    D2D1_RECT_F destination{};
};
struct LanczosResult {
    std::shared_ptr<std::vector<BYTE>> pixels;
    UINT width = 0, height = 0;
    uint64_t generation = 0;
    bool succeeded = false;
    D2D1_RECT_F destination{};
};

// The official NavLib wrapper owns device calibration and its event-driven input loop.  This
// accessor deliberately exposes only the orthographic state Viewtrious actually has: camera
// translation becomes image pan and view extents become center-anchored zoom.
class SpaceMouseNavigation final : public TDx::SpaceMouse::Navigation3D::CNavigation3D {
public:
    std::function<navlib::matrix_t()> getCameraMatrix;
    std::function<void(const navlib::matrix_t&)> setCameraMatrix;
    std::function<navlib::box_t()> getViewExtents;
    std::function<void(const navlib::box_t&)> setViewExtents;
    std::function<double()> getViewFov;
    std::function<void(double)> setViewFov;
    std::function<bool()> getPerspective;
    std::function<bool()> getRotatable;
    std::function<navlib::point_t()> getCameraTarget;
    std::function<void(const navlib::point_t&)> setCameraTarget;
    std::function<navlib::point_t()> getPivot;
    std::function<void(const navlib::point_t&)> setPivot;
    std::function<navlib::box_t()> getModelExtents;
    std::function<void(bool)> setMotion;

    SpaceMouseNavigation() : CNavigation3D(false, navlib::none) { PutProfileHint("Viewtrious"); }

protected:
    long GetCameraMatrix(navlib::matrix_t& matrix) const override {
        if (!getCameraMatrix) return navlib::make_result_code(navlib::navlib_errc::no_data_available);
        matrix = getCameraMatrix(); return 0;
    }
    long SetCameraMatrix(const navlib::matrix_t& matrix) override {
        if (setCameraMatrix) setCameraMatrix(matrix); return 0;
    }
    long GetViewExtents(navlib::box_t& extents) const override {
        if (!getViewExtents) return navlib::make_result_code(navlib::navlib_errc::no_data_available);
        extents = getViewExtents(); return 0;
    }
    long SetViewExtents(const navlib::box_t& extents) override {
        if (setViewExtents) setViewExtents(extents); return 0;
    }
    long GetPointerPosition(navlib::point_t&) const override { return navlib::make_result_code(navlib::navlib_errc::no_data_available); }
    long GetViewFOV(double& fov) const override { if (!getViewFov) return navlib::make_result_code(navlib::navlib_errc::invalid_operation); fov = getViewFov(); return 0; }
    long GetViewFrustum(navlib::frustum_t&) const override { return navlib::make_result_code(navlib::navlib_errc::invalid_operation); }
    long SetViewFOV(double fov) override { if (!setViewFov) return navlib::make_result_code(navlib::navlib_errc::invalid_operation); setViewFov(fov); return 0; }
    long SetViewFrustum(const navlib::frustum_t&) override { return navlib::make_result_code(navlib::navlib_errc::function_not_supported); }
    long GetIsViewPerspective(navlib::bool_t& perspective) const override { perspective = getPerspective && getPerspective(); return 0; }
    long GetIsViewRotatable(navlib::bool_t& rotatable) const override { rotatable = getRotatable && getRotatable(); return 0; }
    long GetModelExtents(navlib::box_t& extents) const override { if (!getModelExtents) return navlib::make_result_code(navlib::navlib_errc::no_data_available); extents = getModelExtents(); return 0; }
    long GetSelectionExtents(navlib::box_t&) const override { return navlib::make_result_code(navlib::navlib_errc::no_data_available); }
    long GetSelectionTransform(navlib::matrix_t&) const override { return navlib::make_result_code(navlib::navlib_errc::no_data_available); }
    long GetIsSelectionEmpty(navlib::bool_t& empty) const override { empty = true; return 0; }
    long SetSelectionTransform(const navlib::matrix_t&) override { return navlib::make_result_code(navlib::navlib_errc::function_not_supported); }
    long GetCameraTarget(navlib::point_t& target) const override { if (!getCameraTarget) return navlib::make_result_code(navlib::navlib_errc::no_data_available); target = getCameraTarget(); return 0; }
    long SetCameraTarget(const navlib::point_t& target) override { if (!setCameraTarget) return navlib::make_result_code(navlib::navlib_errc::function_not_supported); setCameraTarget(target); return 0; }
    long GetPivotPosition(navlib::point_t& pivot) const override { if (!getPivot) return navlib::make_result_code(navlib::navlib_errc::no_data_available); pivot = getPivot(); return 0; }
    long IsUserPivot(navlib::bool_t& userPivot) const override { userPivot = getPivot != nullptr; return 0; }
    long SetPivotPosition(const navlib::point_t& pivot) override { if (!setPivot) return navlib::make_result_code(navlib::navlib_errc::function_not_supported); setPivot(pivot); return 0; }
    long GetPivotVisible(navlib::bool_t& visible) const override { visible = getPivot != nullptr; return 0; }
    long SetPivotVisible(bool) override { return getPivot ? 0 : navlib::make_result_code(navlib::navlib_errc::function_not_supported); }
    long GetHitLookAt(navlib::point_t&) const override { return navlib::make_result_code(navlib::navlib_errc::no_data_available); }
    long SetHitAperture(double) override { return navlib::make_result_code(navlib::navlib_errc::function_not_supported); }
    long SetHitDirection(const navlib::vector_t&) override { return navlib::make_result_code(navlib::navlib_errc::function_not_supported); }
    long SetHitLookFrom(const navlib::point_t&) override { return navlib::make_result_code(navlib::navlib_errc::function_not_supported); }
    long SetHitSelectionOnly(bool) override { return navlib::make_result_code(navlib::navlib_errc::function_not_supported); }
    long SetActiveCommand(std::string) override { return navlib::make_result_code(navlib::navlib_errc::function_not_supported); }
    long SetMotionFlag(bool motion) override { if (setMotion) setMotion(motion); return 0; }
};
constexpr ShortcutEntry kShortcutEntries[] = {
    { L"Ctrl+O", L"Open file" }, { L"Left Arrow", L"Previous image" }, { L"Right Arrow", L"Next image" }, { L"Mouse Wheel", L"Zoom in/out" },
    { L"+", L"Zoom in" }, { L"-", L"Zoom out" }, { L"0", L"Reset zoom and center" },
    { L"Left mouse drag", L"Pan" }, { L"Right mouse click", L"Open right-click menu" }, { L"Double-click image", L"Toggle Fit / 100%" }, { L"F11", L"Toggle fullscreen" },
    { L"Ctrl+C", L"Copy image" }, { L"Ctrl+P", L"Print" }, { L"Delete", L"Move image to Recycle Bin" },
    { L"Esc", L"Exit fullscreen, or close Viewtrious" },
};
constexpr size_t kShortcutEntryCount = sizeof(kShortcutEntries) / sizeof(kShortcutEntries[0]);
constexpr wchar_t kBugReportUrl[] = L"https://github.com/ortrious/Viewtrious/issues/new?template=bug_report.md";
constexpr wchar_t kFeatureRequestUrl[] = L"https://github.com/ortrious/Viewtrious/issues/new?template=feature_request.md";

#if defined(_DEBUG)
class StartupTimer {
public:
    StartupTimer() { QueryPerformanceFrequency(&frequency_); QueryPerformanceCounter(&start_); }

    void Log(const wchar_t* label) const {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsedMs = 1000.0 * static_cast<double>(now.QuadPart - start_.QuadPart) /
            static_cast<double>(frequency_.QuadPart);
        wchar_t message[160]{};
        swprintf_s(message, L"Viewtrious startup: %s: %.2f ms\n", label, elapsedMs);
        OutputDebugStringW(message);
    }

private:
    LARGE_INTEGER frequency_{};
    LARGE_INTEGER start_{};
};
#else
class StartupTimer {
public:
    void Log(const wchar_t*) const {}
};
#endif

bool IsSupportedExtension(const fs::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return extension == L".jpg" || extension == L".jpeg" || extension == L".png" ||
        extension == L".bmp" || extension == L".gif" || extension == L".tif" ||
        extension == L".tiff" || extension == L".ico" || extension == L".webp" ||
        extension == L".heic" || extension == L".heif" || extension == L".avif" ||
        extension == L".dng" || extension == L".cr2" || extension == L".cr3" ||
        extension == L".nef" || extension == L".arw" || extension == L".raf" || extension == L".mp4" || extension == L".stl" || extension == L".3mf" || extension == L".step" || extension == L".stp";
}


std::wstring LowercaseExtension(const std::wstring& path) {
    std::wstring extension = fs::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return extension;
}

bool IsStlPath(const std::wstring& path) { return LowercaseExtension(path) == L".stl"; }
bool IsThreeMfPath(const std::wstring& path) { return LowercaseExtension(path) == L".3mf"; }
bool IsStepPath(const std::wstring& path) { const std::wstring extension=LowercaseExtension(path); return extension == L".step" || extension == L".stp"; }
bool IsModelPath(const std::wstring& path) { return IsStlPath(path) || IsThreeMfPath(path) || IsStepPath(path); }
bool IsVideoPath(const std::wstring& path) { return LowercaseExtension(path) == L".mp4"; }
bool IsTwoDimensionalMediaPath(const fs::path& path) { return IsSupportedExtension(path) && !IsModelPath(path.wstring()); }

bool IsJpegPath(const std::wstring& path) {
    const std::wstring extension = LowercaseExtension(path);
    return extension == L".jpg" || extension == L".jpeg";
}

bool IsPngPath(const std::wstring& path) {
    return LowercaseExtension(path) == L".png";
}

bool IsHeifPath(const std::wstring& path) {
    const std::wstring extension = LowercaseExtension(path);
    return extension == L".heic" || extension == L".heif";
}

bool IsGifPath(const std::wstring& path) { return LowercaseExtension(path) == L".gif"; }

std::wstring FormatFramesPerSecond(float value) {
    if (!std::isfinite(value) || value <= 0.0f) return {};
    const float rounded = std::round(value * 100.0f) / 100.0f;
    wchar_t text[32]{};
    if (std::fabs(rounded - std::round(rounded)) < 0.005f) swprintf_s(text, L"%.0f fps", rounded);
    else {
        swprintf_s(text, L"%.2f", rounded);
        std::wstring compact(text);
        while (!compact.empty() && compact.back() == L'0') compact.pop_back();
        if (!compact.empty() && compact.back() == L'.') compact.pop_back();
        return compact + L" fps";
    }
    return text;
}

UINT GifMetadataUInt(IWICMetadataQueryReader* reader, const wchar_t* name, UINT fallback = 0) {
    if (!reader) return fallback;
    PROPVARIANT value{};
    PropVariantInit(&value);
    const HRESULT hr = reader->GetMetadataByName(name, &value);
    UINT result = fallback;
    if (SUCCEEDED(hr)) {
        if (value.vt == VT_UI1) result = value.bVal;
        else if (value.vt == VT_UI2) result = value.uiVal;
        else if (value.vt == VT_UI4) result = value.ulVal;
    }
    PropVariantClear(&value);
    return result;
}

bool PathsEqual(const fs::path& left, const fs::path& right) {
    const std::wstring leftText = left.lexically_normal().wstring();
    const std::wstring rightText = right.lexically_normal().wstring();
    return CompareStringOrdinal(leftText.c_str(), static_cast<int>(leftText.size()),
        rightText.c_str(), static_cast<int>(rightText.size()), TRUE) == CSTR_EQUAL;
}

struct FileIdentity {
    DWORD volumeSerial = 0;
    DWORD fileIndexHigh = 0;
    DWORD fileIndexLow = 0;
    bool valid = false;
};

FileIdentity ReadFileIdentity(const fs::path& path) {
    const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION information{};
    const BOOL succeeded = GetFileInformationByHandle(file, &information);
    CloseHandle(file);
    if (!succeeded) return {};
    return { information.dwVolumeSerialNumber, information.nFileIndexHigh, information.nFileIndexLow, true };
}

bool SameFileIdentity(const FileIdentity& left, const FileIdentity& right) {
    return left.valid && right.valid && left.volumeSerial == right.volumeSerial &&
        left.fileIndexHigh == right.fileIndexHigh && left.fileIndexLow == right.fileIndexLow;
}

bool ReadSetting(const wchar_t* name, DWORD& value) {
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
}

void WriteSetting(const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

bool WriteRegistryString(HKEY root, const wchar_t* path, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const LONG result = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

struct SavedPlacement {
    RECT rect{};
    bool maximized = false;
};

bool RememberWindowPlacementEnabled() {
    DWORD rememberPlacement = 1;
    ReadSetting(L"RememberWindowPlacement", rememberPlacement);
    return rememberPlacement != 0;
}

bool LoadPlacement(SavedPlacement& placement) {
    if (!RememberWindowPlacementEnabled()) return false;
    DWORD left = 0, top = 0, width = 0, height = 0, maximized = 0;
    if (!ReadSetting(L"WindowLeft", left) || !ReadSetting(L"WindowTop", top) ||
        !ReadSetting(L"WindowWidth", width) || !ReadSetting(L"WindowHeight", height) ||
        width < 200 || height < 150) {
        return false;
    }
    const LONG savedLeft = static_cast<LONG>(left);
    const LONG savedTop = static_cast<LONG>(top);
    placement.rect = { savedLeft, savedTop, savedLeft + static_cast<LONG>(width),
        savedTop + static_cast<LONG>(height) };
    ReadSetting(L"WindowMaximized", maximized);
    placement.maximized = maximized != 0;
    return true;
}

void MakePlacementVisible(RECT& rect) {
    if (MonitorFromRect(&rect, MONITOR_DEFAULTTONULL)) return;
    MONITORINFO monitor{ sizeof(monitor) };
    GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT work = monitor.rcWork;
    const LONG width = std::min(rect.right - rect.left, work.right - work.left);
    const LONG height = std::min(rect.bottom - rect.top, work.bottom - work.top);
    rect.left = std::clamp(rect.left, work.left, work.right - width);
    rect.top = std::clamp(rect.top, work.top, work.bottom - height);
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
}

bool IsLikelySnappedWindow(HWND window) {
    if (IsZoomed(window)) return false;
    RECT rect{};
    if (!GetWindowRect(window, &rect)) return false;
    MONITORINFO monitor{ sizeof(monitor) };
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    const RECT& work = monitor.rcWork;
    constexpr int tolerance = 2;
    const bool spansHeight = std::abs(rect.top - work.top) <= tolerance && std::abs(rect.bottom - work.bottom) <= tolerance;
    const LONG width = rect.right - rect.left;
    const LONG workWidth = work.right - work.left;
    const bool snappedWidth = std::abs(width * 2 - workWidth) <= tolerance * 2 ||
        std::abs(width * 3 - workWidth) <= tolerance * 3 || std::abs(width * 3 - workWidth * 2) <= tolerance * 3;
    return spansHeight && snappedWidth && (std::abs(rect.left - work.left) <= tolerance || std::abs(rect.right - work.right) <= tolerance ||
        std::abs((rect.left + rect.right) - (work.left + work.right)) <= tolerance * 2);
}

bool UseDarkAppMode() {
    DWORD preference = static_cast<DWORD>(ThemePreference::System);
    ReadSetting(L"Theme", preference);
    if (preference == static_cast<DWORD>(ThemePreference::Light)) return false;
    if (preference == static_cast<DWORD>(ThemePreference::Dark)) return true;
    DWORD appsUseLightTheme = 1;
    DWORD size = sizeof(appsUseLightTheme);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &appsUseLightTheme, &size);
    return appsUseLightTheme == 0;
}

struct FrameMetrics {
    int titleBarHeight;
    int border;
    RECT titleBarContent;
    RECT hamburger;
    RECT hamburgerSeparator;
    int resolutionLeft;
    int resolutionWidth;
    RECT resolutionSeparator;
    int fileSizeLeft;
    int fileSizeWidth;
    RECT fileSizeSeparator;
    int filenameLeft;
    RECT minimize;
    RECT maximize;
    RECT close;
};

struct VideoControlsLayout {
    RECT island;
    RECT playPause;
    RECT currentTime;
    RECT scrubber;
    RECT duration;
    RECT mute;
};

std::wstring FormatVideoTime(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) return L"--:--";
    const uint64_t total = static_cast<uint64_t>(std::floor(seconds));
    const uint64_t hours = total / 3600;
    const uint64_t minutes = (total / 60) % 60;
    const uint64_t remaining = total % 60;
    wchar_t text[32]{};
    if (hours) swprintf_s(text, L"%llu:%02llu:%02llu", hours, minutes, remaining);
    else swprintf_s(text, L"%llu:%02llu", total / 60, remaining);
    return text;
}

FrameMetrics GetFrameMetrics(HWND window, bool includeVideoMetadata = false) {
    const UINT dpi = GetDpiForWindow(window);
    const int titleBarHeight = MulDiv(40, dpi, 96);
    const int border = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    const int buttonWidth = MulDiv(46, dpi, 96);
    const int hamburgerWidth = MulDiv(46, dpi, 96);
    const int separatorWidth = MulDiv(1, dpi, 96);
    const int separatorHeight = MulDiv(20, dpi, 96);
    const int sectionGutter = MulDiv(14, dpi, 96);
    const int filenameLeadIn = MulDiv(14, dpi, 96);
    const int resolutionWidth = MulDiv(includeVideoMetadata ? 160 : 92, dpi, 96);
    const int fileSizeWidth = MulDiv(72, dpi, 96);
    RECT client{};
    GetClientRect(window, &client);
    const int buttonLeft = std::max(0L, client.right - buttonWidth * 3);
    const int separatorTop = std::max(0, (titleBarHeight - separatorHeight) / 2);
    const int hamburgerSeparatorLeft = hamburgerWidth - separatorWidth;
    const int resolutionLeft = hamburgerSeparatorLeft + separatorWidth + sectionGutter;
    const int resolutionSeparatorLeft = resolutionLeft + resolutionWidth + sectionGutter;
    const int fileSizeLeft = resolutionSeparatorLeft + separatorWidth + sectionGutter;
    const int fileSizeSeparatorLeft = fileSizeLeft + fileSizeWidth + sectionGutter;
    const int filenameLeft = fileSizeSeparatorLeft + separatorWidth + sectionGutter + filenameLeadIn;
    return { titleBarHeight, border,
        { 0, 0, buttonLeft, titleBarHeight },
        { 0, 0, hamburgerWidth, titleBarHeight },
        { hamburgerSeparatorLeft, separatorTop, hamburgerSeparatorLeft + separatorWidth, separatorTop + separatorHeight },
        resolutionLeft, resolutionWidth,
        { resolutionSeparatorLeft, separatorTop, resolutionSeparatorLeft + separatorWidth, separatorTop + separatorHeight },
        fileSizeLeft, fileSizeWidth,
        { fileSizeSeparatorLeft, separatorTop, fileSizeSeparatorLeft + separatorWidth, separatorTop + separatorHeight },
        filenameLeft,
        { buttonLeft, 0, buttonLeft + buttonWidth, titleBarHeight },
        { buttonLeft + buttonWidth, 0, buttonLeft + buttonWidth * 2, titleBarHeight },
        { buttonLeft + buttonWidth * 2, 0, client.right, titleBarHeight } };
}

enum class CaptionButton { None, Minimize, Maximize, Close };

CaptionButton CaptionButtonAt(const FrameMetrics& frame, POINT point) {
    if (PtInRect(&frame.minimize, point)) return CaptionButton::Minimize;
    if (PtInRect(&frame.maximize, point)) return CaptionButton::Maximize;
    if (PtInRect(&frame.close, point)) return CaptionButton::Close;
    return CaptionButton::None;
}

CaptionButton CaptionButtonFromHitTest(WPARAM hitTest) {
    if (hitTest == HTMINBUTTON) return CaptionButton::Minimize;
    if (hitTest == HTMAXBUTTON) return CaptionButton::Maximize;
    if (hitTest == HTCLOSE) return CaptionButton::Close;
    return CaptionButton::None;
}

WPARAM SystemCommandForCaptionButton(HWND window, CaptionButton button) {
    if (button == CaptionButton::Minimize) return SC_MINIMIZE;
    if (button == CaptionButton::Maximize) return IsZoomed(window) ? SC_RESTORE : SC_MAXIMIZE;
    if (button == CaptionButton::Close) return SC_CLOSE;
    return 0;
}

void ApplyWindowCornerPreference(HWND window, bool roundCorners) {
    const DWM_WINDOW_CORNER_PREFERENCE preference = roundCorners ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
}

void ApplyTitleBarTheme(HWND window) {
    const BOOL dark = UseDarkAppMode() ? TRUE : FALSE;
    DwmSetWindowAttribute(window, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
}

std::wstring FormatFileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) return L"";
    ULARGE_INTEGER size{};
    size.HighPart = attributes.nFileSizeHigh;
    size.LowPart = attributes.nFileSizeLow;
    constexpr wchar_t units[] = L"BKMGT";
    double value = static_cast<double>(size.QuadPart);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) { value /= 1024.0; ++unit; }
    wchar_t text[32]{};
    if (unit == 0 || value >= 10.0) swprintf_s(text, L"%.0f %cB", value, units[unit]);
    else swprintf_s(text, L"%.1f %cB", value, units[unit]);
    return text;
}

class Viewer {
public:
    explicit Viewer(const StartupTimer& timer) : timer_(timer) {}

    HRESULT Initialize(const std::wstring& path) {
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wicFactory_));
        if (FAILED(hr)) {
            error_ = L"Windows Imaging Component could not be initialized.";
            return hr;
        }
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf());
        if (FAILED(hr)) {
            error_ = L"Direct2D could not be initialized.";
            return hr;
        }
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
        if (FAILED(hr)) {
            error_ = L"DirectWrite could not be initialized.";
            return hr;
        }
        RegisterDefaultAppCapabilities();
        DWORD rememberPlacement = 1;
        ReadSetting(L"RememberWindowPlacement", rememberPlacement);
        rememberWindowPlacement_ = rememberPlacement != 0;
        DWORD includeHidden = 1;
        ReadSetting(L"IncludeHiddenImages", includeHidden);
        includeHiddenImages_ = includeHidden != 0;
        DWORD confirmDelete = 1;
        ReadSetting(L"ConfirmBeforeDeleting", confirmDelete);
        confirmBeforeDeleting_ = confirmDelete != 0;
        DWORD showZoomHud = 1;
        ReadSetting(L"ShowZoomPercentage", showZoomHud);
        showZoomPercentage_ = showZoomHud != 0;
        DWORD zoomHudPosition = static_cast<DWORD>(ZoomHudPosition::BottomRight);
        ReadSetting(L"ZoomHudPosition", zoomHudPosition);
        zoomHudPosition_ = zoomHudPosition <= static_cast<DWORD>(ZoomHudPosition::TopRight) ? static_cast<ZoomHudPosition>(zoomHudPosition) : ZoomHudPosition::BottomRight;
        DWORD animations = 1;
        ReadSetting(L"AnimationsAndFadeEffects", animations);
        animationsEnabled_ = animations != 0;
        DWORD reverseWheelZoom = 0;
        ReadSetting(L"ReverseMouseWheelZoom", reverseWheelZoom);
        reverseMouseWheelZoom_ = reverseWheelZoom != 0;
        DWORD spaceMouseEnabled = 1;
        ReadSetting(L"EnableSpaceMouse", spaceMouseEnabled);
        spaceMouseEnabled_ = spaceMouseEnabled != 0;
        DWORD theme = static_cast<DWORD>(ThemePreference::System);
        ReadSetting(L"Theme", theme);
        themePreference_ = theme <= static_cast<DWORD>(ThemePreference::Dark) ? static_cast<ThemePreference>(theme) : ThemePreference::System;
        DWORD projectionMode = static_cast<DWORD>(ModelProjectionMode::Perspective);
        ReadSetting(L"ModelProjectionMode", projectionMode);
        modelProjectionMode_ = projectionMode == static_cast<DWORD>(ModelProjectionMode::Orthographic) ? ModelProjectionMode::Orthographic : ModelProjectionMode::Perspective;
        DWORD visualStyle = static_cast<DWORD>(ModelVisualStyle::Shaded);
        ReadSetting(L"ModelVisualStyle", visualStyle);
        modelVisualStyle_ = visualStyle <= static_cast<DWORD>(ModelVisualStyle::Wireframe) ? static_cast<ModelVisualStyle>(visualStyle) : ModelVisualStyle::Shaded;
        DWORD upAxis = static_cast<DWORD>(ModelUpAxis::ZUp);
        ReadSetting(L"ModelUpAxis", upAxis);
        modelUpAxis_ = upAxis <= static_cast<DWORD>(ModelUpAxis::ZUp) ? static_cast<ModelUpAxis>(upAxis) : ModelUpAxis::ZUp;
        DWORD buildPlate = static_cast<DWORD>(ModelBuildPlate::Auto);
        ReadSetting(L"ModelBuildPlate", buildPlate);
        modelBuildPlate_ = buildPlate <= static_cast<DWORD>(ModelBuildPlate::Off) ? static_cast<ModelBuildPlate>(buildPlate) : ModelBuildPlate::Auto;
        DWORD axisIndicatorPosition = static_cast<DWORD>(AxisIndicatorPosition::BottomRight);
        ReadSetting(L"AxisIndicatorPosition", axisIndicatorPosition);
        axisIndicatorPosition_ = axisIndicatorPosition <= static_cast<DWORD>(AxisIndicatorPosition::TopRight) ? static_cast<AxisIndicatorPosition>(axisIndicatorPosition) : AxisIndicatorPosition::BottomRight;
        DWORD renderingApi = static_cast<DWORD>(ModelRenderingApi::Direct3D11); ReadSetting(L"ModelRenderingApi", renderingApi); modelRenderingApi_ = ModelRenderingApi::Direct3D11;
        DWORD graphicsAdapterAuto = 1, graphicsAdapterLuidLow = 0, graphicsAdapterLuidHigh = 0;
        ReadSetting(L"GraphicsAdapterAuto", graphicsAdapterAuto); ReadSetting(L"GraphicsAdapterLuidLow", graphicsAdapterLuidLow); ReadSetting(L"GraphicsAdapterLuidHigh", graphicsAdapterLuidHigh);
        graphicsAdapterAuto_ = graphicsAdapterAuto != 0; graphicsAdapterLuid_.LowPart = graphicsAdapterLuidLow; graphicsAdapterLuid_.HighPart = static_cast<LONG>(graphicsAdapterLuidHigh);
        graphicsAdapters_ = GraphicsHost::EnumerateHardwareAdapters();
        DWORD antiAliasing = static_cast<DWORD>(ModelAntiAliasing::Msaa4x); ReadSetting(L"ModelAntiAliasing", antiAliasing); modelAntiAliasing_ = antiAliasing <= static_cast<DWORD>(ModelAntiAliasing::Ssaa2x) ? static_cast<ModelAntiAliasing>(antiAliasing) : ModelAntiAliasing::Msaa4x;
        DWORD imageScaling = static_cast<DWORD>(ImageScaling::Quality);
        ReadSetting(L"ImageScaling", imageScaling);
        imageScaling_ = imageScaling == static_cast<DWORD>(ImageScaling::Performance) ? ImageScaling::Performance : ImageScaling::Quality;
        lanczosSelected_ = imageScaling_ == ImageScaling::Quality;
        DWORD onboardingVersion = 0;
        onboardingRequired_ = !ReadSetting(L"OnboardingVersion", onboardingVersion) || onboardingVersion < 1;
        DWORD tourPending = 0;
        ReadSetting(L"TourPending", tourPending);
        tourPending_ = tourPending != 0;
        startupPath_ = path;
        return S_OK;
    }

    HRESULT LoadContent(const std::wstring& path, bool resetNavigation = true) {
        if (IsModelPath(path)) { BeginModelLoad(path); return S_OK; }
        if (IsVideoPath(path)) { BeginVideoLoad(path); return S_OK; }
        DeactivateVideo();
        DeactivateModel();
        contentKind_ = ContentKind::Image2D;
        return LoadImage(path, resetNavigation);
    }

    HRESULT LoadImage(const std::wstring& path, bool resetNavigation = true) {
        ++modelLoadGeneration_;
        StopGifPlayback();
        KillTimer(window_, kShellRotationCheckTimer);
        shellRotationPending_ = false;
        ++decodeRequestGeneration_;
        pendingFullDecode_.reset();
        imageDecodePending_ = false;
        KillTimer(window_, kNavigationDecodeDebounceTimer);
        if (IsGifPath(path)) {
            const HRESULT gifResult = LoadAnimatedGif(path, resetNavigation);
            if (SUCCEEDED(gifResult)) return S_OK;
        }
        ComPtr<IWICBitmapSource> source;
        UINT width = 0;
        UINT height = 0;
        const HRESULT hr = DecodeImage(path, source, width, height);
        if (SUCCEEDED(hr)) {
            CommitImage(path, source, width, height, resetNavigation);
        } else {
            StopDirectoryWatcher();
            source_.Reset();
            bitmap_.Reset();
            imageWidth_ = imageHeight_ = 0;
            currentPath_ = path;
            displayedPath_.clear();
            resolutionText_.clear();
            fileSizeText_ = FormatFileSize(path);
            filenameText_ = fs::path(path).filename().wstring();
            navigationFiles_.clear();
            navigationBuilt_ = false;
            error_ = L"Unable to open this image. It may be corrupt or use an unsupported codec.";
        }
        return hr;
    }

    void ModelLoadCompleteMessage(ModelLoadResult* result) {
        std::unique_ptr<ModelLoadResult> owned(result);
        if (!result) return;
        ReapModelLoadWorker(result->generation);
        if (shuttingDown_ || result->generation != modelLoadGeneration_ || !PathsEqual(fs::path(result->path), fs::path(currentPath_))) return;
        StopModelLoadingAnimation();
        modelLoading_ = false;
        if (!result->IsSuccess()) { contentKind_ = ContentKind::None; error_ = result->error; InvalidateRect(window_, nullptr, FALSE); return; }
        modelDocument_ = result->document;
        std::wstring viewportError;
        EnsureRenderTarget();
        if (!graphicsHost_.Ready() || (!modelViewport_.Active() && !modelViewport_.Create(graphicsHost_, modelDocument_, viewportError, ModelUpVector()))) {
            modelDocument_.reset(); contentKind_ = ContentKind::None; error_ = viewportError; InvalidateRect(window_, nullptr, FALSE); return;
        }
        modelViewport_.SetProjectionMode(modelProjectionMode_);
        modelViewport_.SetVisualStyle(modelVisualStyle_);
        modelViewport_.SetAntiAliasing(modelAntiAliasing_);
        contentKind_ = ContentKind::Model3D;
        modelViewport_.SetBuildPlate(BuildPlateVisible(), ModelUpVector());
        modelTriangleCount_ = modelDocument_->geometries.front().indices.size() / 3;
        resolutionText_ = FormatCompactTriangleCount(modelTriangleCount_) + L" triangles";
        error_.clear(); InvalidateRect(window_, nullptr, FALSE);
    }

    void SetWindow(HWND window) {
        window_ = window;
        EnsureRenderTarget();
        InitializeSpaceMouse();
        ActivateGifPlayback();
        if (!startupPath_.empty()) LoadContent(std::exchange(startupPath_, {}));
    }
    void InitializeSpaceMouse() {
        if (!window_ || spaceMouse_) return;
        spaceMouse_ = std::make_unique<SpaceMouseNavigation>();
        spaceMouse_->getCameraMatrix = [this] { return SpaceMouseCameraMatrix(); };
        spaceMouse_->setCameraMatrix = [this](const navlib::matrix_t& matrix) { SetSpaceMouseCameraMatrix(matrix); };
        spaceMouse_->getViewExtents = [this] { return SpaceMouseViewExtents(); };
        spaceMouse_->setViewExtents = [this](const navlib::box_t& extents) { SetSpaceMouseViewExtents(extents); };
        spaceMouse_->getViewFov = [this] { return ModelActive() ? static_cast<double>(modelViewport_.Camera().FieldOfView()) : 0.0; };
        spaceMouse_->setViewFov = [this](double fov) { if (ModelActive()) modelViewport_.SetNavLibFieldOfView(static_cast<float>(fov)); };
        spaceMouse_->getPerspective = [this] { return ModelActive() && modelViewport_.Camera().ProjectionMode() == ModelProjectionMode::Perspective; };
        spaceMouse_->getRotatable = [this] { return ModelActive(); };
        spaceMouse_->getCameraTarget = [this] { return SpaceMouseModelCameraTarget(); };
        spaceMouse_->setCameraTarget = [this](const navlib::point_t& point) { SetSpaceMouseModelCameraTarget(point); };
        spaceMouse_->getPivot = [this] { return SpaceMouseModelPivot(); };
        spaceMouse_->setPivot = [this](const navlib::point_t& point) { SetSpaceMouseModelPivot(point); };
        spaceMouse_->getModelExtents = [this] { return SpaceMouseModelExtents(); };
        spaceMouse_->setMotion = [this](bool motion) { SetSpaceMouseMotion(motion); };
        std::error_code error;
        spaceMouse_->EnableNavigation(true, error);
        spaceMouseRuntimeAvailable_ = !error && spaceMouse_->IsEnabled();
        if (!spaceMouseRuntimeAvailable_) {
            spaceMouse_.reset();
            return;
        }
        if (!spaceMouseEnabled_) spaceMouse_->EnableNavigation(false, error);
    }
    void ShowWelcomeIfNeeded() {
        if (tourPending_) { StartPendingTour(); return; }
        if (!onboardingRequired_) return;
        overlay_ = OverlayKind::Welcome;
    }
    bool WelcomeOpen() const { return overlay_ == OverlayKind::Welcome || overlay_ == OverlayKind::DefaultAppsHelper; }
    bool IsFullscreen() const { return fullscreen_; }
    void SetCaptionButtonHover(CaptionButton button) {
        if (hoveredCaptionButton_ == button) return;
        hoveredCaptionButton_ = button;
        InvalidateRect(window_, nullptr, FALSE);
    }
    void SetCaptionButtonPressed(CaptionButton button) {
        pressedCaptionButton_ = button;
        InvalidateRect(window_, nullptr, FALSE);
    }
    CaptionButton PressedCaptionButton() const { return pressedCaptionButton_; }
    void ClearCaptionButtonPressed() {
        if (pressedCaptionButton_ == CaptionButton::None) return;
        pressedCaptionButton_ = CaptionButton::None;
        InvalidateRect(window_, nullptr, FALSE);
    }
    void SetHamburgerHover(bool hovered) {
        if (hamburgerHovered_ == hovered) return;
        hamburgerHovered_ = hovered;
        InvalidateRect(window_, nullptr, FALSE);
    }
    void SetHamburgerPressed(bool pressed) {
        if (hamburgerPressed_ == pressed) return;
        hamburgerPressed_ = pressed;
        InvalidateRect(window_, nullptr, FALSE);
    }
    bool HamburgerPressed() const { return hamburgerPressed_; }
    bool DropdownOpen() const { return dropdownOpen_; }
    void ToggleDropdown() {
        if (fullscreen_) return;
        dropdownOpen_ = !dropdownOpen_;
        if (dropdownOpen_) { DismissTriangleCountTooltip(false); DismissOverlay(); DismissContextMenu(); }
        dropdownHovered_ = DropdownItem::None;
        dropdownPressed_ = DropdownItem::None;
        InvalidateRect(window_, nullptr, FALSE);
    }
    void DismissDropdown(bool invalidate = true) {
        if (!dropdownOpen_) return;
        dropdownOpen_ = false;
        dropdownHovered_ = DropdownItem::None;
        dropdownPressed_ = DropdownItem::None;
        if (invalidate) InvalidateRect(window_, nullptr, FALSE);
    }
    DropdownItem DropdownItemAt(POINT point) const {
        if (!dropdownOpen_) return DropdownItem::None;
        const RECT bounds = GetDropdownBounds();
        if (!PtInRect(&bounds, point)) return DropdownItem::None;
        const int rowHeight = MulDiv(38, GetDpiForWindow(window_), 96);
        int top = bounds.top + MulDiv(4, GetDpiForWindow(window_), 96);
        const int separatorGap = MulDiv(9, GetDpiForWindow(window_), 96);
        const auto hit = [&](DropdownItem item) {
            const bool contains = point.y >= top && point.y < top + rowHeight;
            top += rowHeight;
            return contains ? item : DropdownItem::None;
        };
        DropdownItem item = hit(DropdownItem::OpenFile); if (item != DropdownItem::None) return item;
        item = hit(DropdownItem::Settings); if (item != DropdownItem::None) return item;
        top += separatorGap;
        item = hit(DropdownItem::QuickTour); if (item != DropdownItem::None) return item;
        item = hit(DropdownItem::KeyboardShortcuts); if (item != DropdownItem::None) return item;
        top += separatorGap;
        item = hit(DropdownItem::About); if (item != DropdownItem::None) return item;
        item = hit(DropdownItem::Feedback); if (item != DropdownItem::None) return item;
        top += separatorGap;
        return hit(DropdownItem::Close);
    }
    void SetDropdownHover(DropdownItem item) {
        if (dropdownHovered_ == item) return;
        dropdownHovered_ = item;
        InvalidateRect(window_, nullptr, FALSE);
    }
    void SetDropdownPressed(DropdownItem item) {
        dropdownPressed_ = item;
        InvalidateRect(window_, nullptr, FALSE);
    }
    DropdownItem PressedDropdownItem() const { return dropdownPressed_; }
    void ClearDropdownPressed(bool invalidate = true) {
        if (dropdownPressed_ == DropdownItem::None) return;
        dropdownPressed_ = DropdownItem::None;
        if (invalidate) InvalidateRect(window_, nullptr, FALSE);
    }
    void InvokeDropdownItem(DropdownItem item) {
        const bool openingFile = item == DropdownItem::OpenFile;
        DismissDropdown(!openingFile);
        if (openingFile) OpenFile();
        else if (item == DropdownItem::Settings) ShowOverlay(OverlayKind::Settings);
        else if (item == DropdownItem::QuickTour) StartTutorial();
        else if (item == DropdownItem::KeyboardShortcuts) ShowOverlay(OverlayKind::KeyboardShortcuts);
        else if (item == DropdownItem::About) ShowOverlay(OverlayKind::About);
        else if (item == DropdownItem::Feedback) ShowOverlay(OverlayKind::Feedback);
        else if (item == DropdownItem::Close) SendMessageW(window_, WM_SYSCOMMAND, SC_CLOSE, 0);
    }
    void OpenFile() {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
        static const COMDLG_FILTERSPEC filters[] = {
            { L"Supported files", StepAddonPresent() ? L"*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.ico;*.webp;*.heic;*.heif;*.avif;*.dng;*.cr2;*.cr3;*.nef;*.arw;*.raf;*.mp4;*.stl;*.3mf;*.step;*.stp" : L"*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.ico;*.webp;*.heic;*.heif;*.avif;*.dng;*.cr2;*.cr3;*.nef;*.arw;*.raf;*.mp4;*.stl;*.3mf" },
            { L"All files", L"*.*" },
        };
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        dialog->SetFileTypeIndex(1);
        dialog->SetTitle(L"Open Image");
        const HRESULT show = dialog->Show(window_);
        if (FAILED(show)) { InvalidateRect(window_, nullptr, FALSE); return; }
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            LoadContent(path);
            CoTaskMemFree(path);
        }
        InvalidateRect(window_, nullptr, FALSE);
    }
    bool HasImage() const { return source_ != nullptr; }
    bool ModelActive() const { return contentKind_ == ContentKind::Model3D && modelViewport_.Active(); }
    bool VideoActive() const { return contentKind_ == ContentKind::Video2D && videoPlayer_.Active(); }
    void ToggleVideoPlayPause() {
        if (!VideoActive()) return;
        videoPlayer_.TogglePlayPause();
        ShowVideoControls();
        if (videoPlayer_.Playing()) SetTimer(window_, kVideoPlaybackTimer, 16, nullptr);
        else KillTimer(window_, kVideoPlaybackTimer);
        InvalidateRect(window_, nullptr, FALSE);
    }
    VideoControlsLayout GetVideoControlsLayout() const {
        const RECT canvas = ModelCanvasBounds();
        const UINT dpi = GetDpiForWindow(window_);
        const int margin = MulDiv(16, dpi, 96);
        const int bottomGap = MulDiv(24, dpi, 96);
        const int height = MulDiv(48, dpi, 96);
        const int preferredWidth = MulDiv(600, dpi, 96);
        const LONG availableWidth = std::max(1L, canvas.right - canvas.left - margin * 2);
        const int width = std::min(preferredWidth, static_cast<int>(availableWidth));
        const int left = static_cast<int>(canvas.left + (canvas.right - canvas.left - width) / 2);
        const int top = static_cast<int>(std::max(canvas.top, canvas.bottom - bottomGap - height));
        const int padding = std::min(MulDiv(10, dpi, 96), std::max(2, width / 24));
        const int buttonWidth = std::min(MulDiv(32, dpi, 96), std::max(MulDiv(24, dpi, 96), height - padding * 2));
        const int gap = std::min(MulDiv(8, dpi, 96), std::max(3, width / 80));
        const int minimumTrackWidth = MulDiv(40, dpi, 96);
        const int maximumTimeWidth = std::max(MulDiv(16, dpi, 96), (width - padding * 2 - buttonWidth * 2 - gap * 4 - minimumTrackWidth) / 2);
        const int timeWidth = std::min(MulDiv(48, dpi, 96), maximumTimeWidth);
        const int playLeft = left + padding;
        const int currentLeft = playLeft + buttonWidth + gap;
        const int durationRight = left + width - padding - buttonWidth - gap;
        const int scrubberLeft = currentLeft + timeWidth + gap;
        const int scrubberRight = std::max(scrubberLeft, durationRight - timeWidth - gap);
        const int controlTop = top + (height - buttonWidth) / 2;
        return { { left, top, left + width, top + height },
            { playLeft, controlTop, playLeft + buttonWidth, controlTop + buttonWidth },
            { currentLeft, top, currentLeft + timeWidth, top + height },
            { scrubberLeft, top, scrubberRight, top + height },
            { durationRight - timeWidth, top, durationRight, top + height },
            { left + width - padding - buttonWidth, controlTop, left + width - padding, controlTop + buttonWidth } };
    }
    bool VideoControlsInteractive() const { return VideoActive() && videoControlsOpacity_ > 0.05f; }
    ButtonKind VideoControlAt(POINT point) const {
        if (!VideoControlsInteractive()) return ButtonKind::None;
        const VideoControlsLayout layout = GetVideoControlsLayout();
        if (PtInRect(&layout.playPause, point)) return ButtonKind::VideoPlayPause;
        if (PtInRect(&layout.mute, point)) return ButtonKind::VideoMute;
        return ButtonKind::None;
    }
    bool VideoScrubberContains(POINT point) const {
        if (!VideoControlsInteractive()) return false;
        const VideoControlsLayout layout = GetVideoControlsLayout();
        const RECT hit{ layout.scrubber.left, layout.scrubber.top + (layout.scrubber.bottom - layout.scrubber.top) / 2 - MulDiv(12, GetDpiForWindow(window_), 96),
            layout.scrubber.right, layout.scrubber.top + (layout.scrubber.bottom - layout.scrubber.top) / 2 + MulDiv(12, GetDpiForWindow(window_), 96) };
        return hit.right > hit.left && PtInRect(&hit, point);
    }
    bool VideoControlsContains(POINT point) const {
        if (!VideoControlsInteractive()) return false;
        const RECT island = GetVideoControlsLayout().island;
        return PtInRect(&island, point);
    }
    void RestoreVideoCursor() {
        if (!videoCursorHidden_) return;
        ShowCursor(TRUE);
        videoCursorHidden_ = false;
    }
    void HideVideoCursorIfAppropriate() {
        if (videoCursorHidden_ || !VideoActive() || !videoPlayer_.Playing() || videoControlsOpacity_ > 0.01f) return;
        POINT point{};
        if (!GetCursorPos(&point) || !ScreenToClient(window_, &point)) return;
        const RECT canvas = ModelCanvasBounds();
        if (!PtInRect(&canvas, point)) return;
        ShowCursor(FALSE);
        videoCursorHidden_ = true;
    }
    void ShowVideoControls() {
        if (!VideoActive()) return;
        RestoreVideoCursor();
        videoControlsOpacity_ = 1.0f;
        videoControlsFadeActive_ = false;
        videoControlsLastActivity_ = GetTickCount64();
        KillTimer(window_, kVideoControlsTimer);
        if (videoPlayer_.Playing() && !videoControlsPointerOver_ && !videoScrubbing_)
            SetTimer(window_, kVideoControlsTimer, static_cast<UINT>(kVideoControlsIdleDelayMs), nullptr);
        InvalidateRect(window_, nullptr, FALSE);
    }
    void ResetVideoControls() {
        KillTimer(window_, kVideoControlsTimer);
        videoControlsOpacity_ = 1.0f;
        videoControlsFadeActive_ = false;
        videoControlsPointerOver_ = false;
        videoScrubbing_ = false;
        videoControlsHovered_ = ButtonKind::None;
        videoControlsLastActivity_ = GetTickCount64();
        RestoreVideoCursor();
    }
    void StopVideoControls() {
        KillTimer(window_, kVideoControlsTimer);
        videoScrubbing_ = false;
        videoControlsFadeActive_ = false;
        videoControlsOpacity_ = 0.0f;
        videoControlsHovered_ = ButtonKind::None;
        RestoreVideoCursor();
    }
    void UpdateVideoScrub(POINT point) {
        double current = 0.0, duration = 0.0;
        if (!videoPlayer_.GetPlaybackTimes(current, duration)) return;
        const RECT scrubber = GetVideoControlsLayout().scrubber;
        if (scrubber.right <= scrubber.left) return;
        const float fraction = std::clamp(static_cast<float>(point.x - scrubber.left) / static_cast<float>(scrubber.right - scrubber.left), 0.0f, 1.0f);
        videoScrubSeconds_ = duration * fraction;
        videoPlayer_.Seek(videoScrubSeconds_);
        InvalidateRect(window_, nullptr, FALSE);
    }
    bool BeginVideoControlsInteraction(POINT point) {
        if (!VideoActive()) return false;
        ShowVideoControls();
        if (!VideoControlsContains(point)) return false;
        videoControlsPointerOver_ = true;
        KillTimer(window_, kVideoControlsTimer);
        if (VideoScrubberContains(point)) {
            videoScrubbing_ = true;
            UpdateVideoScrub(point);
            return true;
        }
        const ButtonKind control = VideoControlAt(point);
        if (control == ButtonKind::VideoPlayPause) ToggleVideoPlayPause();
        else if (control == ButtonKind::VideoMute) { videoPlayer_.ToggleMute(); ShowVideoControls(); }
        return true;
    }
    bool ContinueVideoControlsInteraction(POINT point) {
        if (!videoScrubbing_) return false;
        ShowVideoControls();
        videoScrubbing_ = true;
        UpdateVideoScrub(point);
        return true;
    }
    bool EndVideoControlsInteraction(POINT point) {
        if (!videoScrubbing_) return false;
        UpdateVideoScrub(point);
        videoScrubbing_ = false;
        ShowVideoControls();
        return true;
    }
    void UpdateVideoControlsMouse(POINT point) {
        if (!VideoActive()) return;
        lastMousePoint_ = point;
        ShowVideoControls();
        videoControlsPointerOver_ = VideoControlsContains(point);
        videoControlsHovered_ = VideoControlAt(point);
        if (videoControlsPointerOver_) KillTimer(window_, kVideoControlsTimer);
        else if (videoPlayer_.Playing() && !videoScrubbing_) SetTimer(window_, kVideoControlsTimer, static_cast<UINT>(kVideoControlsIdleDelayMs), nullptr);
        if (videoScrubbing_) UpdateVideoScrub(point);
    }
    void VideoControlsMouseLeave() {
        if (!VideoActive()) return;
        videoControlsPointerOver_ = false;
        videoControlsHovered_ = ButtonKind::None;
        if (videoPlayer_.Playing() && !videoScrubbing_) SetTimer(window_, kVideoControlsTimer, static_cast<UINT>(kVideoControlsIdleDelayMs), nullptr);
    }
    void CancelVideoControlsInteraction() {
        if (!videoScrubbing_) return;
        videoScrubbing_ = false;
        ShowVideoControls();
    }
    void UpdateVideoControlsFade() {
        if (!VideoActive()) { StopVideoControls(); return; }
        if (!videoPlayer_.Playing() || videoControlsPointerOver_ || videoScrubbing_) { KillTimer(window_, kVideoControlsTimer); return; }
        const ULONGLONG elapsed = GetTickCount64() - videoControlsLastActivity_;
        if (!videoControlsFadeActive_) {
            if (elapsed < kVideoControlsIdleDelayMs) {
                SetTimer(window_, kVideoControlsTimer, static_cast<UINT>(kVideoControlsIdleDelayMs - elapsed), nullptr);
                return;
            }
            videoControlsFadeActive_ = true;
            videoControlsFadeStart_ = GetTickCount64();
            videoControlsFadeStartOpacity_ = videoControlsOpacity_;
        }
        const float progress = std::min(1.0f, static_cast<float>(GetTickCount64() - videoControlsFadeStart_) / static_cast<float>(kVideoControlsFadeDurationMs));
        videoControlsOpacity_ = videoControlsFadeStartOpacity_ * (1.0f - progress);
        InvalidateRect(window_, nullptr, FALSE);
        if (progress < 1.0f) SetTimer(window_, kVideoControlsTimer, 16, nullptr);
        else { videoControlsFadeActive_ = false; KillTimer(window_, kVideoControlsTimer); HideVideoCursorIfAppropriate(); }
    }
    void UpdateTriangleCountTooltipHover(POINT point) {
        const RECT textBounds = TriangleCountTitleTextBounds();
        const bool hovering = TriangleCountTooltipAvailable() && PtInRect(&textBounds, point);
        if (triangleCountTooltipHovering_ == hovering) return;
        triangleCountTooltipHovering_ = hovering;
        if (!hovering) {
            DismissTriangleCountTooltip();
            return;
        }
        SetTimer(window_, kTriangleCountTooltipTimer, kTriangleCountTooltipDelayMs, nullptr);
    }
    void TriangleCountTooltipTimerMessage() {
        KillTimer(window_, kTriangleCountTooltipTimer);
        if (!triangleCountTooltipHovering_ || !TriangleCountTooltipAvailable()) return;
        triangleCountTooltipVisible_ = true;
        InvalidateRect(window_, nullptr, FALSE);
    }
    void CancelAnimatedModelHome() { KillTimer(window_, kModelHomeAnimationTimer); modelViewport_.CancelAnimatedHome(); }
    Float3 ModelUpVector() const { switch (modelUpAxis_) { case ModelUpAxis::XUp: return { 1, 0, 0 }; case ModelUpAxis::YUp: return { 0, 1, 0 }; case ModelUpAxis::ZUp: return { 0, 0, 1 }; } return { 0, 0, 1 }; }
    bool BuildPlateVisible() const { const ModelDocument* document=modelViewport_.Document(); return modelBuildPlate_ == ModelBuildPlate::On || (modelBuildPlate_ == ModelBuildPlate::Auto && document && document->sourceFormat == ModelSourceFormat::ThreeMf); }
    void FitModel() { if (ModelActive()) { ClearModelFaceSelection(); CancelAnimatedModelHome(); modelViewport_.Fit(ModelUpVector()); InvalidateRect(window_, nullptr, FALSE); } }
    void BeginAnimatedModelHome() { if (!ModelActive()) return; ClearModelFaceSelection(); if (!modelViewport_.BeginAnimatedHome(ModelUpVector())) return; modelAnimationDurationMs_=kModelHomeAnimationDurationMs; modelHomeAnimationStartMs_ = GetTickCount64(); SetTimer(window_, kModelHomeAnimationTimer, 16, nullptr); InvalidateRect(window_, nullptr, FALSE); }
    void BeginAnimatedFitSelection() { if (!ModelActive() || !modelViewport_.BeginAnimatedFitSelected(ModelUpVector())) return; modelAnimationDurationMs_=kModelHomeAnimationDurationMs; modelHomeAnimationStartMs_ = GetTickCount64(); SetTimer(window_, kModelHomeAnimationTimer, 16, nullptr); InvalidateRect(window_, nullptr, FALSE); }
    void BeginAnimatedModelFramingRecovery() { if (!ModelActive() || !modelViewport_.BeginAnimatedFramingRecovery()) return; modelAnimationDurationMs_=200; modelHomeAnimationStartMs_ = GetTickCount64(); SetTimer(window_, kModelHomeAnimationTimer, 16, nullptr); InvalidateRect(window_, nullptr, FALSE); }
    void BeginAnimatedModelOrientation(Float3 forward, Float3 up) { if (!ModelActive() || !modelViewport_.BeginAnimatedOrientation(forward, up)) return; modelAnimationDurationMs_=kModelHomeAnimationDurationMs; modelHomeAnimationStartMs_ = GetTickCount64(); SetTimer(window_, kModelHomeAnimationTimer, 16, nullptr); InvalidateRect(window_, nullptr, FALSE); }
    void BeginAnimatedModelSnapView(Float3 forward, Float3 up, Float3 hitPoint) { if (!ModelActive() || !modelViewport_.BeginAnimatedSnapView(forward, up, hitPoint)) return; modelAnimationDurationMs_=kModelHomeAnimationDurationMs; modelHomeAnimationStartMs_ = GetTickCount64(); SetTimer(window_, kModelHomeAnimationTimer, 16, nullptr); InvalidateRect(window_, nullptr, FALSE); }
    void UpdateAnimatedModelHome() {
        if (!ModelActive()) { CancelAnimatedModelHome(); return; }
        const float progress = std::clamp(static_cast<float>(GetTickCount64() - modelHomeAnimationStartMs_) / static_cast<float>(modelAnimationDurationMs_), 0.0f, 1.0f);
        const float eased = progress * progress * (3.0f - 2.0f * progress);
        if (!modelViewport_.AdvanceAnimatedHome(eased)) KillTimer(window_, kModelHomeAnimationTimer);
        InvalidateRect(window_, nullptr, FALSE);
    }
    void BeginModelOrbit(POINT point) { if (ModelActive()) { ClearModelFaceSelection(); modelClickStart_=point; modelClickCandidate_=true; CancelAnimatedModelHome(); modelViewport_.BeginOrbit(point); } }
    void BeginModelPan(POINT point) { if (ModelActive()) { modelClickCandidate_=false; ClearModelFaceSelection(); CancelAnimatedModelHome(); modelViewport_.BeginPan(point); } }
    void ContinueModelDrag(POINT point) { if (!ModelActive()) return; const LONG dx=point.x-modelClickStart_.x,dy=point.y-modelClickStart_.y;if(dx*dx+dy*dy>16)modelClickCandidate_=false; CancelAnimatedModelHome(); const RECT bounds = ModelCanvasBounds(); modelViewport_.ContinueDrag(point, std::max(1L, bounds.right - bounds.left), std::max(1L, bounds.bottom - bounds.top)); InvalidateRect(window_, nullptr, FALSE); }
    void EndModelDrag() { modelClickCandidate_=false; modelViewport_.EndDrag(); }
    bool FinishModelSelectionClick(POINT point) { const bool click=modelClickCandidate_;modelClickCandidate_=false;modelViewport_.EndDrag();if(click) return SelectModelObject(point);return false; }
    void DollyModel(float steps) { if (ModelActive()) { ClearModelFaceSelection(); CancelAnimatedModelHome(); modelViewport_.Dolly(steps); InvalidateRect(window_, nullptr, FALSE); } }
    int ModelObjectRangeAt(POINT point) const {
        if (!ModelActive() || !modelViewport_.Document() || modelViewport_.Document()->geometries.empty()) return -1;
        const D3D11_VIEWPORT viewport = modelViewport_.LogicalViewport();
        const float localX = float(point.x) - viewport.TopLeftX, localY = float(point.y) - viewport.TopLeftY;
        if (viewport.Width <= 0 || viewport.Height <= 0 || localX < 0 || localX >= viewport.Width || localY < 0 || localY >= viewport.Height) return -1;
        const auto state = modelViewport_.Camera().NavLibState();
        const auto dot = [](Float3 a, Float3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; };
        const auto cross = [](Float3 a, Float3 b) { return Float3{a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; };
        const auto normalize = [](Float3 v) { const float length = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z); return length > 1e-6f ? Float3{v.x/length, v.y/length, v.z/length} : Float3{0,0,1}; };
        const Float3 forward = normalize(state.forward), right = normalize(cross(forward, state.up)), up = normalize(cross(right, forward));
        const float ndcX = 2.f * localX / viewport.Width - 1.f, ndcY = 1.f - 2.f * localY / viewport.Height;
        Float3 rayOrigin = state.position, ray{};
        if (modelViewport_.Camera().ProjectionMode() == ModelProjectionMode::Orthographic) {
            const float halfHeight = modelViewport_.Camera().ViewHalfHeight();
            rayOrigin = {state.position.x + right.x*ndcX*halfHeight*modelViewport_.Camera().AspectRatio() + up.x*ndcY*halfHeight,
                         state.position.y + right.y*ndcX*halfHeight*modelViewport_.Camera().AspectRatio() + up.y*ndcY*halfHeight,
                         state.position.z + right.z*ndcX*halfHeight*modelViewport_.Camera().AspectRatio() + up.z*ndcY*halfHeight};
            ray = forward;
        } else {
            const float tangent = std::tan(modelViewport_.Camera().FieldOfView() * .5f);
            ray = normalize({forward.x + right.x*ndcX*modelViewport_.Camera().AspectRatio()*tangent + up.x*ndcY*tangent,
                             forward.y + right.y*ndcX*modelViewport_.Camera().AspectRatio()*tangent + up.y*ndcY*tangent,
                             forward.z + right.z*ndcX*modelViewport_.Camera().AspectRatio()*tangent + up.z*ndcY*tangent});
        }
        const auto& document = *modelViewport_.Document();
        const auto& mesh = document.geometries.front();
        float nearest = FLT_MAX; ptrdiff_t selectedTriangle = -1;
        for (size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
            const Float3 a = mesh.positions[mesh.indices[index]], b = mesh.positions[mesh.indices[index + 1]], c = mesh.positions[mesh.indices[index + 2]];
            const Float3 edge1{b.x-a.x,b.y-a.y,b.z-a.z}, edge2{c.x-a.x,c.y-a.y,c.z-a.z}, perpendicular = cross(ray, edge2);
            const float determinant = dot(edge1, perpendicular); if (std::fabs(determinant) < 1e-7f) continue;
            const float inverse = 1.f / determinant; const Float3 offset{rayOrigin.x-a.x,rayOrigin.y-a.y,rayOrigin.z-a.z};
            const float u = dot(offset, perpendicular) * inverse; if (u < 0 || u > 1) continue;
            const Float3 q = cross(offset, edge1); const float v = dot(ray, q) * inverse; if (v < 0 || u + v > 1) continue;
            const float distance = dot(edge2, q) * inverse;
            if (distance > 0 && distance < nearest) { nearest = distance; selectedTriangle = static_cast<ptrdiff_t>(index / 3); }
        }
        if (selectedTriangle >= 0) {
            const uint32_t triangle = static_cast<uint32_t>(selectedTriangle);
            for (uint32_t range = 0; range < document.instanceRanges.size(); ++range) {
                const ModelInstanceRange& candidate = document.instanceRanges[range];
                if (triangle >= candidate.firstTriangle && triangle - candidate.firstTriangle < candidate.triangleCount) return static_cast<int>(range);
            }
        }
        return -1;
    }
    bool SelectModelObject(POINT point) {
        const int range = ModelObjectRangeAt(point);
        if (range >= 0 && modelViewport_.SetSelectedObjectRange(static_cast<uint32_t>(range))) { InvalidateRect(window_, nullptr, FALSE); return true; }
        modelViewport_.ClearSelectedObjectRange(); InvalidateRect(window_, nullptr, FALSE); return false;
    }
    void SelectAndFitModelObject(POINT point) {
        if (!SelectModelObject(point)) return;
        CancelAnimatedModelHome();
        BeginAnimatedFitSelection();
    }
    bool SelectModelFace(POINT point) { if(!ModelActive()||!modelViewport_.Document()||modelViewport_.Document()->geometries.empty())return false;const D3D11_VIEWPORT viewport=modelViewport_.LogicalViewport();const float localX=float(point.x)-viewport.TopLeftX,localY=float(point.y)-viewport.TopLeftY;if(viewport.Width<=0||viewport.Height<=0||localX<0||localX>=viewport.Width||localY<0||localY>=viewport.Height){ClearModelFaceSelection();TraceModelPick(point,viewport,localX,localY,0,0,{}, {},-1,-1);return false;}const auto state=modelViewport_.Camera().NavLibState();const auto dot=[](Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};const auto cross=[](Float3 a,Float3 b){return Float3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};const auto normalize=[&](Float3 v){const float l=std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);return l>1e-6f?Float3{v.x/l,v.y/l,v.z/l}:Float3{0,0,1};};const Float3 forward=normalize(state.forward),right=normalize(cross(forward,state.up)),up=normalize(cross(right,forward));const float ndcX=2.f*localX/viewport.Width-1.f,ndcY=1.f-2.f*localY/viewport.Height;Float3 rayOrigin=state.position,ray{};if(modelViewport_.Camera().ProjectionMode()==ModelProjectionMode::Orthographic){const float halfHeight=modelViewport_.Camera().ViewHalfHeight();rayOrigin={state.position.x+right.x*ndcX*halfHeight*modelViewport_.Camera().AspectRatio()+up.x*ndcY*halfHeight,state.position.y+right.y*ndcX*halfHeight*modelViewport_.Camera().AspectRatio()+up.y*ndcY*halfHeight,state.position.z+right.z*ndcX*halfHeight*modelViewport_.Camera().AspectRatio()+up.z*ndcY*halfHeight};ray=forward;}else{const float t=std::tan(modelViewport_.Camera().FieldOfView()*.5f),nx=ndcX*modelViewport_.Camera().AspectRatio()*t,ny=ndcY*t;ray=normalize({forward.x+right.x*nx+up.x*ny,forward.y+right.y*nx+up.y*ny,forward.z+right.z*nx+up.z*ny});}const auto& document=*modelViewport_.Document();const auto& mesh=document.geometries.front();float best=FLT_MAX;int selectedPlane=-1;ptrdiff_t selectedTriangle=-1;for(size_t i=0;i+2<mesh.indices.size();i+=3){const Float3 a=mesh.positions[mesh.indices[i]],b=mesh.positions[mesh.indices[i+1]],c=mesh.positions[mesh.indices[i+2]],e1{b.x-a.x,b.y-a.y,b.z-a.z},e2{c.x-a.x,c.y-a.y,c.z-a.z},p=cross(ray,e2);const float det=dot(e1,p);if(std::fabs(det)<1e-7f)continue;const float inv=1.f/det;const Float3 s{rayOrigin.x-a.x,rayOrigin.y-a.y,rayOrigin.z-a.z};const float u=dot(s,p)*inv;if(u<0||u>1)continue;const Float3 q=cross(s,e1);const float v=dot(ray,q)*inv;if(v<0||u+v>1)continue;const float d=dot(e2,q)*inv;if(d>0&&d<best){best=d;const size_t triangle=i/3;if(triangle<document.triangleSnapPlanes.size()){selectedTriangle=static_cast<ptrdiff_t>(triangle);selectedPlane=static_cast<int>(document.triangleSnapPlanes[triangle]);}}}TraceModelPick(point,viewport,localX,localY,ndcX,ndcY,rayOrigin,ray,selectedTriangle,selectedPlane);if(selectedPlane<0||!modelViewport_.SetSelectedSnapPlane(static_cast<uint32_t>(selectedPlane))){ClearModelFaceSelection();return false;}selectedFaceNormal_=document.snapPlanes[static_cast<size_t>(selectedPlane)].normal;selectedFaceHit_={rayOrigin.x+ray.x*best,rayOrigin.y+ray.y*best,rayOrigin.z+ray.z*best};selectedFacePlane_=selectedPlane;modelFaceSelected_=true;InvalidateRect(window_,nullptr,FALSE);return true; }
    bool ContextMenuOpen() const { return contextMenuOpen_; }
    void OpenContextMenu(POINT point) {
        if (WelcomeOpen() || TutorialActive()) return;
        if (!HasImage() && !(ModelActive() && modelFaceSelected_)) return;
        RefreshHeifShellRotationCapability();
        DismissTriangleCountTooltip(false);
        DismissDropdown();
        DismissOverlay();
        contextMenuAnchor_ = point;
        contextMenuOpen_ = true;
        heifRotationMenuLocked_ = IsHeifPath(currentPath_) && IsHeifRotationGateActive();
        if (heifRotationMenuLocked_) SetTimer(window_, kHeifRotationMenuRefreshTimer, 100, nullptr);
        contextHovered_ = ContextAction::None;
        contextPressed_ = ContextAction::None;
        InvalidateRect(window_, nullptr, FALSE);
    }
    void DismissContextMenu() {
        if (!contextMenuOpen_) return;
        KillTimer(window_, kHeifRotationMenuRefreshTimer);
        contextMenuOpen_ = false; if (ModelActive()) ClearModelFaceSelection();
        heifRotationMenuLocked_ = false;
        openWithSubmenuOpen_ = false;
        contextHovered_ = ContextAction::None;
        contextPressed_ = ContextAction::None;
        InvalidateRect(window_, nullptr, FALSE);
    }
    ContextAction ContextActionAt(POINT point) const {
        if (!contextMenuOpen_) return ContextAction::None;
        const RECT bounds = GetContextMenuBounds();
        if (!PtInRect(&bounds, point)) return ContextAction::None;
        if (ModelActive()) return point.y >= bounds.top && point.y < bounds.bottom ? ContextAction::SnapViewToFace : ContextAction::None; const int rowHeight = MulDiv(38, GetDpiForWindow(window_), 96);
        const int separatorGap = MulDiv(9, GetDpiForWindow(window_), 96);
        int top = bounds.top + MulDiv(kContextMenuPaddingDip, GetDpiForWindow(window_), 96);
        const auto hit = [&](ContextAction action) {
            const bool contains = point.y >= top && point.y < top + rowHeight;
            top += rowHeight;
            return contains ? action : ContextAction::None;
        };
        ContextAction action = hit(ContextAction::Fullscreen); if (action != ContextAction::None) return action;
        top += separatorGap;
        action = hit(ContextAction::RotateLeft); if (action != ContextAction::None) return action;
        action = hit(ContextAction::RotateRight); if (action != ContextAction::None) return action;
        top += separatorGap;
        action = hit(ContextAction::OpenWith); if (action != ContextAction::None) return action;
        action = hit(ContextAction::Copy); if (action != ContextAction::None) return action;
        action = hit(ContextAction::Print); if (action != ContextAction::None) return action;
        top += separatorGap;
        action = hit(ContextAction::SetBackground); if (action != ContextAction::None) return action;
        top += separatorGap;
        return hit(ContextAction::Delete);
    }
    bool ContextActionEnabled(ContextAction action) const {
        if (action == ContextAction::SnapViewToFace) return ModelActive() && modelFaceSelected_;
        if (tutorialStep_ == TutorialStep::ContextMenu && !HasImage()) return false;
        if (action == ContextAction::Copy || action == ContextAction::Print) return HasImage() && DisplayedImageMatchesTarget();
        if (action == ContextAction::Fullscreen || action == ContextAction::OpenWith ||
            action == ContextAction::SetBackground || action == ContextAction::Delete)
            return true;
        return (action == ContextAction::RotateLeft || action == ContextAction::RotateRight) &&
            DisplayedImageMatchesTarget() && (IsJpegPath(currentPath_) || IsPngPath(currentPath_) ||
                (!heifRotationMenuLocked_ && IsHeifPath(currentPath_) &&
                    (action == ContextAction::RotateLeft ? heifShellRotateLeftAvailable_ : heifShellRotateRightAvailable_)));
    }
    void SetContextHover(ContextAction action) {
        if (!ContextActionEnabled(action)) action = ContextAction::None;
        if (contextHovered_ == action) return;
        contextHovered_ = action;
        InvalidateRect(window_, nullptr, FALSE);
    }
    void SetContextPressed(ContextAction action) {
        if (contextPressed_ == action) return;
        contextPressed_ = action;
        InvalidateRect(window_, nullptr, FALSE);
    }
    ContextAction PressedContextAction() const { return contextPressed_; }
    void ClearContextPressed() { SetContextPressed(ContextAction::None); }
    void InvokeContextAction(ContextAction action) {
        if(action==ContextAction::SnapViewToFace){const Float3 normal=selectedFaceNormal_,hitPoint=selectedFaceHit_;const int plane=selectedFacePlane_;const OrbitCamera::State state=modelViewport_.Camera().NavLibState();const auto dot=[](Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};const auto normalize=[&](Float3 v){const float length=std::sqrt(dot(v,v));return length>1e-6f?Float3{v.x/length,v.y/length,v.z/length}:Float3{0,1,0};};const Float3 forward=dot(normal,state.forward)<0?Float3{-normal.x,-normal.y,-normal.z}:normal;const auto projectUp=[&](Float3 axis){return Float3{axis.x-forward.x*dot(axis,forward),axis.y-forward.y*dot(axis,forward),axis.z-forward.z*dot(axis,forward)};};Float3 chosenAxis=ModelUpVector(),up=projectUp(chosenAxis);if(dot(up,up)<1e-8f){const Float3 fallbacks[]={{0,0,1},{0,1,0},{1,0,0}};for(const Float3 axis:fallbacks){up=projectUp(axis);if(dot(up,up)>=1e-8f){chosenAxis=axis;break;}}}up=normalize(up);TraceSnapView(plane,normal,hitPoint,state,forward,up,chosenAxis);DismissContextMenu();BeginAnimatedModelSnapView(forward,up,hitPoint);return;}
        if (action == ContextAction::OpenWith) { ToggleOpenWithSubmenu(); return; }
        DismissContextMenu();
        if (action == ContextAction::Fullscreen) ToggleFullscreen();
        else if (action == ContextAction::Copy) CopyImage();
        else if (action == ContextAction::Print) PrintImage();
        else if (action == ContextAction::RotateLeft) RotateImage(false);
        else if (action == ContextAction::RotateRight) RotateImage(true);
        else if (action == ContextAction::SetBackground) SetDesktopBackground();
        else if (action == ContextAction::Delete) {
            if (confirmBeforeDeleting_) ShowOverlay(OverlayKind::DeleteConfirm);
            else DeleteImage();
        }
    }
    bool OpenWithSubmenuOpen() const { return openWithSubmenuOpen_; }
    void DismissOpenWithSubmenu() { openWithSubmenuOpen_ = false; openWithHovered_ = -1; InvalidateRect(window_, nullptr, FALSE); }
    bool OpenWithBridgeContains(POINT point) const {
        if (!openWithSubmenuOpen_) return false;
        const RECT parent = GetContextMenuBounds();
        const RECT child = GetOpenWithSubmenuBounds();
        const UINT dpi = GetDpiForWindow(window_);
        const LONG row = MulDiv(38, dpi, 96);
        const LONG gap = MulDiv(9, dpi, 96);
        const LONG top = parent.top + MulDiv(kContextMenuPaddingDip, dpi, 96) + row * 3 + gap * 2;
        const LONG bottom = top + row;
        const LONG left = std::min(parent.right, child.right);
        const LONG right = std::max(parent.left, child.left);
        return point.x >= left && point.x < right && point.y >= top && point.y < bottom;
    }
    int OpenWithItemAt(POINT point) const {
        if (!openWithSubmenuOpen_) return -1;
        const RECT bounds = GetOpenWithSubmenuBounds();
        if (!PtInRect(&bounds, point)) return -1;
        const int row = MulDiv(38, GetDpiForWindow(window_), 96);
        const int top = bounds.top + MulDiv(4, GetDpiForWindow(window_), 96);
        const int index = (point.y - top) / row;
        if (point.y < top || index < 0) return -1;
        if (index < static_cast<int>(openWithHandlers_.size())) return index;
        const int chooseTop = top + static_cast<int>(openWithHandlers_.size()) * row + MulDiv(9, GetDpiForWindow(window_), 96);
        return point.y >= chooseTop && point.y < chooseTop + row ? static_cast<int>(openWithHandlers_.size()) : -1;
    }
    void SetOpenWithHover(int index) { if (openWithHovered_ != index) { openWithHovered_ = index; InvalidateRect(window_, nullptr, FALSE); } }
    void InvokeOpenWithItem(int index) {
        if (index == static_cast<int>(openWithHandlers_.size())) { DismissContextMenu(); OpenWith(); return; }
        if (index < 0 || index >= static_cast<int>(openWithHandlers_.size())) return;
        ComPtr<IShellItem> item;
        ComPtr<IDataObject> data;
        if (FAILED(SHCreateItemFromParsingName(currentPath_.c_str(), nullptr, IID_PPV_ARGS(&item))) ||
            FAILED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data))) ||
            FAILED(openWithHandlers_[index].handler->Invoke(data.Get()))) ShowActionError(L"Windows could not open this image with the selected application.");
        DismissContextMenu();
    }
    void UpdateCopyFeedback() {
        if (!copyFeedbackActive_) return;
        if (GetTickCount64() - copyFeedbackStart_ >= 1000) {
            copyFeedbackActive_ = false;
            KillTimer(window_, kCopyFeedbackTimer);
        }
        InvalidateRect(window_, nullptr, FALSE);
    }
    bool HasOverlay() const { return overlay_ != OverlayKind::None; }
    bool TutorialActive() const { return tutorialStep_ != TutorialStep::None; }
    void StartTutorial() {
        if (TutorialActive()) return;
        BeginTutorialPresentation();
        SetTutorialStep(TutorialStep::OpenImages);
    }
    void ResumePendingTour() {
        if (tourPending_ && !TutorialActive() && !HasOverlay()) StartPendingTour();
    }
    void StopTutorial() {
        tutorialStep_ = TutorialStep::None;
        tutorialContextMenu_ = false;
        DismissDropdown(false);
        DismissContextMenu();
        DismissOverlay();
        ClearButtonPressed();
        SetButtonHover(ButtonKind::None);
        RestoreTutorialPresentation();
        InvalidateRect(window_, nullptr, FALSE);
    }
    void BeginTutorialPresentation() {
        tutorialPresentation_ = true;
        tutorialPlacementSuppressed_ = true;
        tutorialWasFullscreen_ = fullscreen_;
        tutorialWasMaximized_ = IsZoomed(window_);
        WINDOWPLACEMENT placement{ sizeof(placement) };
        GetWindowPlacement(window_, &placement);
        tutorialWindowRect_ = tutorialWasFullscreen_ ? fullscreenRect_ : placement.rcNormalPosition;
        tutorialZoom_ = zoom_;
        tutorialPan_ = pan_;
        tutorialFitToWindow_ = fitToWindow_;
        copyFeedbackActive_ = false;
        KillTimer(window_, kCopyFeedbackTimer);
        if (fullscreen_) ToggleFullscreen();
        if (IsZoomed(window_)) ShowWindow(window_, SW_RESTORE);
        RECT current{};
        GetWindowRect(window_, &current);
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromRect(&current, MONITOR_DEFAULTTONEAREST), &monitor);
        RECT tutorialBounds{ 0, 0, 800, 600 };
        AdjustWindowRectEx(&tutorialBounds, WS_OVERLAPPEDWINDOW, FALSE, 0);
        const int width = tutorialBounds.right - tutorialBounds.left;
        const int height = tutorialBounds.bottom - tutorialBounds.top;
        const int maxLeft = std::max(monitor.rcWork.left, monitor.rcWork.right - width);
        const int maxTop = std::max(monitor.rcWork.top, monitor.rcWork.bottom - height);
        const int left = std::clamp(static_cast<int>(current.left), static_cast<int>(monitor.rcWork.left), maxLeft);
        const int top = std::clamp(static_cast<int>(current.top), static_cast<int>(monitor.rcWork.top), maxTop);
        SetWindowPos(window_, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void RestoreTutorialPresentation() {
        if (!tutorialPresentation_) return;
        tutorialPresentation_ = false;
        zoom_ = tutorialZoom_;
        pan_ = tutorialPan_;
        fitToWindow_ = tutorialFitToWindow_;
        SetWindowPos(window_, nullptr, tutorialWindowRect_.left, tutorialWindowRect_.top,
            tutorialWindowRect_.right - tutorialWindowRect_.left, tutorialWindowRect_.bottom - tutorialWindowRect_.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        if (tutorialWasMaximized_) ShowWindow(window_, SW_MAXIMIZE);
        if (tutorialWasFullscreen_) ToggleFullscreen();
        tutorialPlacementSuppressed_ = false;
    }
    void AdvanceTutorial() {
        if (tutorialStep_ == TutorialStep::OpenImages) SetTutorialStep(TutorialStep::ResizeWindow);
        else if (tutorialStep_ == TutorialStep::ResizeWindow) SetTutorialStep(TutorialStep::MenuSettings);
        else if (tutorialStep_ == TutorialStep::MenuSettings) SetTutorialStep(TutorialStep::ImageDetails);
        else if (tutorialStep_ == TutorialStep::ImageDetails) SetTutorialStep(TutorialStep::ContextMenu);
        else if (tutorialStep_ == TutorialStep::ContextMenu) SetTutorialStep(TutorialStep::Shortcuts);
        else StopTutorial();
    }
    void ShowOverlay(OverlayKind overlay) {
        DismissTriangleCountTooltip(false);
        DismissDropdown();
        DismissContextMenu();
        if (overlay == OverlayKind::DeleteConfirm) deleteWarningSuppressOnConfirm_ = false;
        if (overlay == OverlayKind::Settings) {
            settingsPage_ = SettingsPage::General;
            settingsScroll_ = 0.0f;
        }
        overlay_ = overlay;
        EndPan();
        InvalidateRect(window_, nullptr, FALSE);
    }
    void DismissOverlay() {
        if (!HasOverlay()) return;
        overlay_ = OverlayKind::None;
        InvalidateRect(window_, nullptr, FALSE);
    }
    bool OverlayContains(POINT point) const {
        const RECT bounds = GetOverlayBounds();
        return HasOverlay() && PtInRect(&bounds, point);
    }
    float SettingsMaximumScroll() const {
        if (overlay_ != OverlayKind::Settings) return 0.0f;
        const RECT bounds = GetOverlayBounds();
        const UINT dpi = GetDpiForWindow(window_);
        const float contentBottom = static_cast<float>(SettingsContentBottom());
        const float viewportBottom = static_cast<float>(bounds.bottom - bounds.top - MulDiv(18, dpi, 96));
        return std::max(0.0f, contentBottom - viewportBottom);
    }
    bool SettingsContains(POINT point) const {
        const RECT bounds = GetOverlayBounds();
        return overlay_ == OverlayKind::Settings && PtInRect(&bounds, point);
    }
    void ScrollSettings(float delta) {
        if (overlay_ != OverlayKind::Settings) return;
        settingsScroll_ = std::clamp(settingsScroll_ + delta, 0.0f, SettingsMaximumScroll());
        InvalidateRect(window_, nullptr, FALSE);
    }
    bool SettingsScrollUpVisible() const { return SettingsMaximumScroll() > 0.5f && settingsScroll_ > 0.5f; }
    bool SettingsScrollDownVisible() const { return SettingsMaximumScroll() > 0.5f && settingsScroll_ < SettingsMaximumScroll() - 0.5f; }
    RECT GetSettingsScrollIndicatorBounds(bool up) const {
        const RECT bounds = GetOverlayBounds(); const int dpi = GetDpiForWindow(window_);
        const int width = MulDiv(32, dpi, 96), height = MulDiv(20, dpi, 96);
        const int left = SettingsContentLeft() + (SettingsContentRight() - SettingsContentLeft() - width) / 2;
        const int top = up ? bounds.top + MulDiv(64, dpi, 96) : bounds.bottom - MulDiv(18, dpi, 96) - height;
        return { left, top, left + width, top + height };
    }
    bool SettingsScrollIndicatorContains(POINT point, bool up) const { const RECT bounds = GetSettingsScrollIndicatorBounds(up); return PtInRect(&bounds, point) != FALSE; }
    RECT GetSettingsNavigationBounds(SettingsPage page) const {
        const RECT bounds = GetOverlayBounds();
        const UINT dpi = GetDpiForWindow(window_);
        const int left = bounds.left + MulDiv(18, dpi, 96);
        const int top = bounds.top + MulDiv(66 + static_cast<int>(page) * 38, dpi, 96);
        return { left, top, left + MulDiv(164, dpi, 96), top + MulDiv(32, dpi, 96) };
    }
    int SettingsContentLeft() const {
        const RECT bounds = GetOverlayBounds();
        const int dpi = GetDpiForWindow(window_);
        const LONG preferred = bounds.left + MulDiv(static_cast<int>(kSettingsContentLeftPaddingDips), dpi, 96);
        const LONG minimumContentWidth = MulDiv(140, dpi, 96);
        return static_cast<int>(std::min<LONG>(preferred, bounds.right - MulDiv(static_cast<int>(kSettingsContentRightPaddingDips), dpi, 96) - minimumContentWidth));
    }
    int MeasureSettingsTextHeight(const wchar_t* text, int width, float size, DWRITE_FONT_WEIGHT weight) const {
        if (!dwriteFactory_ || width <= 0) return MulDiv(20, GetDpiForWindow(window_), 96);
        ComPtr<IDWriteTextFormat> format;
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        if (FAILED(dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size * scale, L"", &format))) return MulDiv(20, GetDpiForWindow(window_), 96);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)), format.Get(), static_cast<float>(width), 4096.0f, &layout))) return MulDiv(20, GetDpiForWindow(window_), 96);
        DWRITE_TEXT_METRICS metrics{};
        return SUCCEEDED(layout->GetMetrics(&metrics)) ? std::max(MulDiv(20, GetDpiForWindow(window_), 96), static_cast<int>(std::ceil(metrics.height))) : MulDiv(20, GetDpiForWindow(window_), 96);
    }
    int MeasureSettingsTextWidth(const wchar_t* text, float size, DWRITE_FONT_WEIGHT weight) const {
        if (!dwriteFactory_) return MulDiv(static_cast<int>(wcslen(text) * size), GetDpiForWindow(window_), 96);
        ComPtr<IDWriteTextFormat> format;
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        if (FAILED(dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size * scale, L"", &format))) return MulDiv(static_cast<int>(wcslen(text) * size), GetDpiForWindow(window_), 96);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)), format.Get(), 4096.0f, 64.0f, &layout))) return MulDiv(static_cast<int>(wcslen(text) * size), GetDpiForWindow(window_), 96);
        DWRITE_TEXT_METRICS metrics{};
        return SUCCEEDED(layout->GetMetrics(&metrics)) ? static_cast<int>(std::ceil(metrics.widthIncludingTrailingWhitespace)) : MulDiv(static_cast<int>(wcslen(text) * size), GetDpiForWindow(window_), 96);
    }
    int SettingsStackGap() const { return MulDiv(static_cast<int>(kSettingsCheckboxRowGapDips), GetDpiForWindow(window_), 96); }
    int SettingsRowGap() const { return MulDiv(static_cast<int>(kSettingsRowGapDips), GetDpiForWindow(window_), 96); }
    int SettingsSectionGap() const { return MulDiv(static_cast<int>(kSettingsMajorSectionGapDips), GetDpiForWindow(window_), 96); }
    RECT GetSettingsSingleColumnBounds(int top, const wchar_t* label) const {
        const int left = SettingsContentLeft(), right = SettingsContentRight();
        const int checkboxWidth = MulDiv(30, GetDpiForWindow(window_), 96);
        const int height = std::max(MulDiv(static_cast<int>(kSettingsControlHeightDips), GetDpiForWindow(window_), 96), MeasureSettingsTextHeight(label, right - left - checkboxWidth, 16.0f, DWRITE_FONT_WEIGHT_NORMAL));
        return { left, top, right, top + height };
    }
    int SettingsContentBottom() const {
        if (settingsPage_ == SettingsPage::General) return GetSettingsResetButtonBounds().bottom - GetOverlayBounds().top;
        if (settingsPage_ == SettingsPage::Image2D) return GetSettingsZoomHudBounds().bottom - GetOverlayBounds().top;
        return GetSettingsSpaceMouseBounds().bottom - GetOverlayBounds().top;
    }
    RECT GetSettingsOptionBounds(int option) const {
        const RECT bounds = GetOverlayBounds();
        const int firstTop = bounds.top + MulDiv(static_cast<int>(kSettingsFirstRowTopDips), GetDpiForWindow(window_), 96);
        if (settingsPage_ == SettingsPage::General) {
            const RECT remember = GetSettingsSingleColumnBounds(firstTop, L"remember application position and size");
            return option == 0 ? remember : GetSettingsSingleColumnBounds(remember.bottom + SettingsStackGap(), L"confirm before deleting images");
        }
        if (settingsPage_ == SettingsPage::Image2D) {
            const RECT include = GetSettingsSingleColumnBounds(firstTop, L"include hidden images in folder");
            const RECT animations = GetSettingsSingleColumnBounds(include.bottom + SettingsStackGap(), L"animations and face effects");
            const RECT reverse = GetSettingsSingleColumnBounds(animations.bottom + SettingsStackGap(), L"reverse mouse wheel zoom direction");
            if (option == 1) return include;
            if (option == 4) return animations;
            if (option == 5) return reverse;
        }
        return GetSettingsSpaceMouseBounds();
    }
    int SettingsContentRight() const { const RECT bounds = GetOverlayBounds(); return bounds.right - MulDiv(static_cast<int>(kSettingsContentRightPaddingDips), GetDpiForWindow(window_), 96); }
    RECT GetSettingsGridCell(int column, float topDips) const {
        const UINT dpi = GetDpiForWindow(window_); const int left = SettingsContentLeft(), right = SettingsContentRight();
        const int gap = MulDiv(static_cast<int>(kSettingsColumnGapDips), dpi, 96); const int width = (right - left - gap) / 2;
        const int cellLeft = left + column * (width + gap); const int top = GetOverlayBounds().top + MulDiv(static_cast<int>(topDips), dpi, 96);
        return { cellLeft, top, cellLeft + width, top + MulDiv(static_cast<int>(kSettingsControlHeightDips), dpi, 96) };
    }
    template <size_t N>
    int DropdownWidth(const std::array<const wchar_t*, N>& options, float fontSize, DWRITE_FONT_WEIGHT weight) const {
        const UINT dpi = GetDpiForWindow(window_);
        const float scale = static_cast<float>(dpi) / 96.0f;
        float widest = 0.0f;
        ComPtr<IDWriteTextFormat> format;
        if (dwriteFactory_ && SUCCEEDED(dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, weight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize * scale, L"", &format))) {
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            for (const wchar_t* option : options) {
                ComPtr<IDWriteTextLayout> layout;
                if (SUCCEEDED(dwriteFactory_->CreateTextLayout(option, static_cast<UINT32>(wcslen(option)), format.Get(),
                        2048.0f * scale, 64.0f * scale, &layout))) {
                    DWRITE_TEXT_METRICS metrics{};
                    if (SUCCEEDED(layout->GetMetrics(&metrics))) widest = std::max(widest, metrics.widthIncludingTrailingWhitespace);
                }
            }
        }
        if (widest <= 0.0f) widest = 160.0f * scale;
        return static_cast<int>(std::ceil(widest)) + MulDiv(kDropdownLeftPaddingDips + kDropdownChevronReserveDips, dpi, 96);
    }
    int GraphicsAdapterDropdownWidth() const {
        std::vector<std::wstring> labels{ L"Auto (High Performance)" };
        for (const auto& adapter : graphicsAdapters_) labels.push_back(adapter.name);
        const UINT dpi = GetDpiForWindow(window_); const float scale = static_cast<float>(dpi) / 96.0f;
        float widest = 0.0f; ComPtr<IDWriteTextFormat> format;
        if (dwriteFactory_ && SUCCEEDED(dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f * scale, L"", &format))) {
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            for (const auto& label : labels) { ComPtr<IDWriteTextLayout> layout; if (SUCCEEDED(dwriteFactory_->CreateTextLayout(label.c_str(), static_cast<UINT32>(label.size()), format.Get(), 2048.0f * scale, 64.0f * scale, &layout))) { DWRITE_TEXT_METRICS metrics{}; if (SUCCEEDED(layout->GetMetrics(&metrics))) widest = std::max(widest, metrics.widthIncludingTrailingWhitespace); } }
        }
        if (widest <= 0.0f) widest = 200.0f * scale;
        return static_cast<int>(std::ceil(widest)) + MulDiv(kDropdownLeftPaddingDips + kDropdownChevronReserveDips, dpi, 96);
    }
    static bool SameGraphicsAdapterLuid(const LUID& left, const LUID& right) { return left.HighPart == right.HighPart && left.LowPart == right.LowPart; }
    std::wstring GraphicsAdapterLabel() const { if (graphicsAdapterAuto_) return L"Auto (High Performance)"; for (const auto& adapter : graphicsAdapters_) if (SameGraphicsAdapterLuid(adapter.luid, graphicsAdapterLuid_)) return adapter.name; return L"Saved adapter unavailable"; }
    RECT GetSettingsToggleBounds(int column, float topDips) const { return GetSettingsGridCell(column, topDips); }
    RECT GetSettingsThemeBounds(ThemePreference preference) const {
        const RECT confirm = GetSettingsOptionBounds(2);
        const int buttonWidth = MulDiv(76, GetDpiForWindow(window_), 96), gap = MulDiv(8, GetDpiForWindow(window_), 96);
        const int left = SettingsContentLeft() + static_cast<int>(preference) * (buttonWidth + gap);
        RECT result{ left, confirm.bottom + SettingsSectionGap() + MulDiv(static_cast<int>(kSettingsSectionHeadingHeightDips + kSettingsLabelToControlGapDips), GetDpiForWindow(window_), 96), left + buttonWidth, 0 };
        result.bottom = result.top + MulDiv(static_cast<int>(kSettingsControlHeightDips), GetDpiForWindow(window_), 96);
        return result;
    }
    RECT GetSettingsScalingBounds(ImageScaling scaling) const {
        const RECT reverse = GetSettingsOptionBounds(5);
        const int labelHeight = MeasureSettingsTextHeight(L"image scaling", SettingsContentRight() - SettingsContentLeft(), 16.0f, DWRITE_FONT_WEIGHT_NORMAL);
        const int top = reverse.bottom + SettingsStackGap() + labelHeight + MulDiv(static_cast<int>(kSettingsLabelToControlGapDips), GetDpiForWindow(window_), 96);
        const int width = MulDiv(scaling == ImageScaling::Quality ? 76 : 104, GetDpiForWindow(window_), 96), gap = MulDiv(8, GetDpiForWindow(window_), 96);
        const int left = SettingsContentLeft() + (scaling == ImageScaling::Quality ? 0 : width - MulDiv(28, GetDpiForWindow(window_), 96) + gap);
        return { left, top, left + width, top + MulDiv(static_cast<int>(kSettingsControlHeightDips), GetDpiForWindow(window_), 96) };
    }
    RECT GetSettingsZoomHudBounds() const {
        const RECT scaling = GetSettingsScalingBounds(ImageScaling::Quality);
        const int labelHeight = MeasureSettingsTextHeight(L"show zoom percentage", SettingsContentRight() - SettingsContentLeft(), 16.0f, DWRITE_FONT_WEIGHT_NORMAL);
        const int top = scaling.bottom + SettingsStackGap() + labelHeight + MulDiv(static_cast<int>(kSettingsLabelToControlGapDips), GetDpiForWindow(window_), 96);
        const int width = std::max(MulDiv(140, GetDpiForWindow(window_), 96), (SettingsContentRight() - SettingsContentLeft()) / 3);
        return { SettingsContentLeft(), top, SettingsContentLeft() + width, top + MulDiv(static_cast<int>(kSettingsControlHeightDips), GetDpiForWindow(window_), 96) };
    }
    int SettingsHeadingToControlGap() const { return MulDiv(10, GetDpiForWindow(window_), 96); }
    int SettingsLabelToControlGap() const { return MulDiv(static_cast<int>(kSettingsLabelToControlGapDips), GetDpiForWindow(window_), 96); }
    int SettingsControlHeight() const { return MulDiv(static_cast<int>(kSettingsControlHeightDips), GetDpiForWindow(window_), 96); }
    int SettingsSectionHeadingHeight() const { return MulDiv(static_cast<int>(kSettingsSectionHeadingHeightDips), GetDpiForWindow(window_), 96); }
    int SettingsViewHeadingTop() const { return GetOverlayBounds().top + MulDiv(76, GetDpiForWindow(window_), 96); }
    int SettingsViewFirstControlTop() const {
        const int labels = std::max(MeasureSettingsTextHeight(L"up axis", GetSettingsGridCell(0, 0).right - GetSettingsGridCell(0, 0).left, 16.0f, DWRITE_FONT_WEIGHT_NORMAL),
            MeasureSettingsTextHeight(L"build plate", GetSettingsGridCell(1, 0).right - GetSettingsGridCell(1, 0).left, 16.0f, DWRITE_FONT_WEIGHT_NORMAL));
        return SettingsViewHeadingTop() + SettingsSectionHeadingHeight() + SettingsHeadingToControlGap() + labels + SettingsLabelToControlGap();
    }
    RECT GetSettingsGridCellAtTop(int column, int top) const {
        RECT cell = GetSettingsGridCell(column, 0.0f);
        cell.top = top;
        cell.bottom = top + SettingsControlHeight();
        return cell;
    }
    int SettingsNextControlTop(const RECT& previous, const wchar_t* label, int column) const {
        const RECT cell = GetSettingsGridCellAtTop(column, 0);
        return previous.bottom + SettingsRowGap() + MeasureSettingsTextHeight(label, cell.right - cell.left, 16.0f, DWRITE_FONT_WEIGHT_NORMAL) + SettingsLabelToControlGap();
    }
    RECT GetSettingsUpAxisBounds() const { return GetSettingsGridCellAtTop(0, SettingsViewFirstControlTop()); }
    RECT GetSettingsBuildPlateBounds() const { return GetSettingsGridCellAtTop(1, SettingsViewFirstControlTop()); }
    RECT GetSettingsProjectionBounds() const { return GetSettingsGridCellAtTop(0, SettingsNextControlTop(GetSettingsUpAxisBounds(), L"projection", 0)); }
    RECT GetSettingsAxisIndicatorPositionBounds() const { return GetSettingsGridCellAtTop(1, SettingsNextControlTop(GetSettingsBuildPlateBounds(), L"axis indicator position", 1)); }
    int GetSettingsViewBottom() const { return std::max(GetSettingsProjectionBounds().bottom, GetSettingsAxisIndicatorPositionBounds().bottom); }
    int GetSettingsRenderHeadingTop() const { return GetSettingsViewBottom() + SettingsSectionGap(); }
    int GetSettingsRenderControlTop() const {
        const RECT left = GetSettingsGridCellAtTop(0, 0), right = GetSettingsGridCellAtTop(1, 0);
        const int labels = std::max(MeasureSettingsTextHeight(L"graphics adapter", left.right - left.left, 16.0f, DWRITE_FONT_WEIGHT_NORMAL),
            MeasureSettingsTextHeight(L"anti-aliasing", right.right - right.left, 16.0f, DWRITE_FONT_WEIGHT_NORMAL));
        return GetSettingsRenderHeadingTop() + SettingsSectionHeadingHeight() + SettingsHeadingToControlGap() + labels + SettingsLabelToControlGap() + MulDiv(2, GetDpiForWindow(window_), 96);
    }
    RECT GetSettingsGraphicsAdapterBounds() const { return GetSettingsGridCellAtTop(0, GetSettingsRenderControlTop()); }
    RECT GetSettingsAntiAliasingBounds() const { return GetSettingsGridCellAtTop(1, GetSettingsRenderControlTop()); }
    int GetSettingsGraphicsAdapterInfoBottom() const {
        const RECT adapter = GetSettingsGraphicsAdapterBounds();
        const int width = adapter.right - adapter.left;
        const int activeLabelWidth = MulDiv(46, GetDpiForWindow(window_), 96);
        int top = adapter.bottom + MulDiv(6, GetDpiForWindow(window_), 96);
        top += std::max(MulDiv(18, GetDpiForWindow(window_), 96), MeasureSettingsTextHeight(graphicsHost_.ActiveAdapterName().empty() ? L"Unavailable" : graphicsHost_.ActiveAdapterName().c_str(), width - activeLabelWidth, 12.0f, DWRITE_FONT_WEIGHT_NORMAL));
        top += MeasureSettingsTextHeight(L"rendering API    Direct3D 11", width, 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
        top += MeasureSettingsTextHeight(L"restart to apply GPU selection changes", width, 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
        return top;
    }
    int GetSettingsInputHeadingTop() const { return static_cast<int>(std::max<LONG>(GetSettingsAntiAliasingBounds().bottom, GetSettingsGraphicsAdapterInfoBottom())) + SettingsSectionGap(); }
    RECT GetSettingsSpaceMouseBounds() const { const int top = GetSettingsInputHeadingTop() + SettingsSectionHeadingHeight() + SettingsLabelToControlGap(); return { SettingsContentLeft(), top, SettingsContentRight(), top + SettingsControlHeight() }; }
    RECT GetSettingsGraphicsAdapterMenuBounds() const { return GetSettingsDropdownMenuBounds(GetSettingsGraphicsAdapterBounds(), static_cast<int>(graphicsAdapters_.size() + 1)); }
    RECT GetSettingsDropdownMenuBounds(RECT control, int itemCount) const {
        const int row = MulDiv(30, GetDpiForWindow(window_), 96), gap = MulDiv(4, GetDpiForWindow(window_), 96), height = row * itemCount;
        const RECT bounds = GetOverlayBounds();
        const int viewportBottom = bounds.bottom - MulDiv(18, GetDpiForWindow(window_), 96) + static_cast<int>(std::lround(settingsScroll_));
        const int top = control.bottom + gap + height > viewportBottom ? control.top - gap - height : control.bottom + gap;
        return { control.left, top, control.right, top + height };
    }
    RECT GetSettingsZoomHudMenuBounds() const { return GetSettingsDropdownMenuBounds(GetSettingsZoomHudBounds(), 4); }
    bool SettingsImageDropdownMenuOpen() const { return overlay_ == OverlayKind::Settings && settingsPage_ == SettingsPage::Image2D && zoomHudPositionMenuOpen_; }
    bool SettingsImageDropdownMenuContains(POINT point) const { point.y += static_cast<LONG>(std::lround(settingsScroll_)); const RECT menu=GetSettingsZoomHudMenuBounds(); return PtInRect(&menu,point) != FALSE; }
    void DismissSettingsImageDropdownMenu() { if (zoomHudPositionMenuOpen_) { zoomHudPositionMenuOpen_=false; InvalidateRect(window_,nullptr,FALSE); } }
    RECT GetSettingsUpAxisMenuBounds() const { return GetSettingsDropdownMenuBounds(GetSettingsUpAxisBounds(), 3); }
    RECT GetSettingsBuildPlateMenuBounds() const { return GetSettingsDropdownMenuBounds(GetSettingsBuildPlateBounds(), 3); }
    RECT GetSettingsAxisIndicatorPositionMenuBounds() const { return GetSettingsDropdownMenuBounds(GetSettingsAxisIndicatorPositionBounds(), 4); }
    RECT GetSettingsProjectionMenuBounds() const { return GetSettingsDropdownMenuBounds(GetSettingsProjectionBounds(), 2); }
    bool SettingsGraphicsAdapterMenuOpen() const { return overlay_ == OverlayKind::Settings && settingsPage_ == SettingsPage::Model3D && graphicsAdapterMenuOpen_; }
    bool SettingsGraphicsAdapterMenuContains(POINT point) const { point.y += static_cast<LONG>(std::lround(settingsScroll_)); const RECT menu=GetSettingsGraphicsAdapterMenuBounds(); return PtInRect(&menu,point) != FALSE; }
    void DismissSettingsGraphicsAdapterMenu() { if (graphicsAdapterMenuOpen_) { graphicsAdapterMenuOpen_=false; InvalidateRect(window_,nullptr,FALSE); } }
    RECT GetSettingsAntiAliasingMenuBounds() const { return GetSettingsDropdownMenuBounds(GetSettingsAntiAliasingBounds(), 6); }
    bool SettingsAntiAliasingMenuOpen() const { return overlay_ == OverlayKind::Settings && settingsPage_ == SettingsPage::Model3D && antiAliasingMenuOpen_; }
    bool SettingsAntiAliasingMenuContains(POINT point) const { point.y += static_cast<LONG>(std::lround(settingsScroll_)); const RECT menu=GetSettingsAntiAliasingMenuBounds(); return PtInRect(&menu,point) != FALSE; }
    void DismissSettingsAntiAliasingMenu() { if (antiAliasingMenuOpen_) { antiAliasingMenuOpen_=false; InvalidateRect(window_,nullptr,FALSE); } }
    bool SettingsSimpleDropdownMenuOpen() const { return overlay_ == OverlayKind::Settings && settingsPage_ == SettingsPage::Model3D && (upAxisMenuOpen_ || buildPlateMenuOpen_ || axisIndicatorPositionMenuOpen_ || projectionMenuOpen_); }
    bool SettingsSimpleDropdownMenuContains(POINT point) const { point.y += static_cast<LONG>(std::lround(settingsScroll_)); const RECT menu=upAxisMenuOpen_?GetSettingsUpAxisMenuBounds():buildPlateMenuOpen_?GetSettingsBuildPlateMenuBounds():axisIndicatorPositionMenuOpen_?GetSettingsAxisIndicatorPositionMenuBounds():GetSettingsProjectionMenuBounds(); return PtInRect(&menu,point) != FALSE; }
    void DismissSettingsSimpleDropdownMenu() { if (upAxisMenuOpen_ || buildPlateMenuOpen_ || axisIndicatorPositionMenuOpen_ || projectionMenuOpen_) { upAxisMenuOpen_=buildPlateMenuOpen_=axisIndicatorPositionMenuOpen_=projectionMenuOpen_=false; InvalidateRect(window_,nullptr,FALSE); } }
    bool SettingsDropdownControlContains(POINT point) const {
        point.y += static_cast<LONG>(std::lround(settingsScroll_));
        const auto contains = [&point](const RECT& bounds) { return PtInRect(&bounds, point) != FALSE; };
        if (settingsPage_ == SettingsPage::Image2D) return contains(GetSettingsZoomHudBounds()) || contains(GetSettingsScalingBounds(ImageScaling::Quality));
        if (settingsPage_ == SettingsPage::Model3D) return contains(GetSettingsUpAxisBounds()) || contains(GetSettingsBuildPlateBounds()) || contains(GetSettingsAxisIndicatorPositionBounds()) || contains(GetSettingsProjectionBounds()) || contains(GetSettingsGraphicsAdapterBounds()) || contains(GetSettingsAntiAliasingBounds());
        return false;
    }
    int ModelViewBarProjectionWidth() const { return DropdownWidth(kProjectionOptions, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD); }
    int ModelViewBarStyleWidth() const { return DropdownWidth(kVisualStyleOptions, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD); }
    RECT GetModelViewBarBounds() const {
        const RECT canvas = ModelCanvasBounds(); const int dpi = GetDpiForWindow(window_);
        const int width = ModelViewBarProjectionWidth() + 1 + ModelViewBarStyleWidth(), height = MulDiv(32, dpi, 96), top = canvas.top + MulDiv(12, dpi, 96);
        const int left = canvas.left + ((canvas.right - canvas.left) - width) / 2;
        return { left, top, left + width, top + height };
    }
    RECT GetModelViewBarProjectionBounds() const { RECT result=GetModelViewBarBounds(); result.right=result.left+ModelViewBarProjectionWidth(); return result; }
    RECT GetModelViewBarStyleBounds() const { RECT result=GetModelViewBarBounds(); result.left=GetModelViewBarProjectionBounds().right+1; return result; }
    RECT GetModelViewBarProjectionMenuBounds() const { const RECT bar=GetModelViewBarProjectionBounds();const int row=MulDiv(32,GetDpiForWindow(window_),96),gap=MulDiv(4,GetDpiForWindow(window_),96);return {bar.left,bar.bottom+gap,bar.right,bar.bottom+gap+row*2}; }
    RECT GetModelViewBarStyleMenuBounds() const { const RECT bar=GetModelViewBarStyleBounds();const int row=MulDiv(32,GetDpiForWindow(window_),96),gap=MulDiv(4,GetDpiForWindow(window_),96);return {bar.left,bar.bottom+gap,bar.right,bar.bottom+gap+row*3}; }
    void DismissModelViewBarMenu() { if (viewBarProjectionMenuOpen_||viewBarVisualStyleMenuOpen_) { viewBarProjectionMenuOpen_=false;viewBarVisualStyleMenuOpen_=false;InvalidateRect(window_,nullptr,FALSE); } }
    bool ModelViewBarMenuOpen() const { return viewBarProjectionMenuOpen_||viewBarVisualStyleMenuOpen_; }
    void ToggleIncludeHiddenImages() {
        includeHiddenImages_ = !includeHiddenImages_;
        WriteSetting(L"IncludeHiddenImages", includeHiddenImages_ ? 1 : 0);
        navigationFiles_.clear();
        navigationBuilt_ = false;
        navigationBuildQueued_ = false;
        InvalidateRect(window_, nullptr, FALSE);
    }
    void ToggleRememberWindowPlacement() {
        rememberWindowPlacement_ = !rememberWindowPlacement_;
        WriteSetting(L"RememberWindowPlacement", rememberWindowPlacement_ ? 1 : 0);
        InvalidateRect(window_, nullptr, FALSE);
    }
    void ToggleConfirmBeforeDeleting() {
        confirmBeforeDeleting_ = !confirmBeforeDeleting_;
        WriteSetting(L"ConfirmBeforeDeleting", confirmBeforeDeleting_ ? 1 : 0);
        InvalidateRect(window_, nullptr, FALSE);
    }
    void ToggleShowZoomPercentage() {
        showZoomPercentage_ = !showZoomPercentage_;
        WriteSetting(L"ShowZoomPercentage", showZoomPercentage_ ? 1 : 0);
        InvalidateRect(window_, nullptr, FALSE);
    }
    void ToggleAnimationsAndFadeEffects() {
        animationsEnabled_ = !animationsEnabled_;
        WriteSetting(L"AnimationsAndFadeEffects", animationsEnabled_ ? 1 : 0);
        UpdateCanvasNavigationOpacity();
        InvalidateRect(window_, nullptr, FALSE);
    }
    void ToggleReverseMouseWheelZoom() {
        reverseMouseWheelZoom_ = !reverseMouseWheelZoom_;
        WriteSetting(L"ReverseMouseWheelZoom", reverseMouseWheelZoom_ ? 1 : 0);
        InvalidateRect(window_, nullptr, FALSE);
    }
    void ToggleSpaceMouse() {
        if (!spaceMouseRuntimeAvailable_ || !spaceMouse_) return;
        spaceMouseEnabled_ = !spaceMouseEnabled_;
        WriteSetting(L"EnableSpaceMouse", spaceMouseEnabled_ ? 1 : 0);
        std::error_code error;
        spaceMouse_->EnableNavigation(spaceMouseEnabled_, error);
        if (error) { spaceMouseEnabled_ = false; WriteSetting(L"EnableSpaceMouse", 0); }
        InvalidateRect(window_, nullptr, FALSE);
    }
    void SetThemePreference(ThemePreference preference) {
        themePreference_ = preference;
        WriteSetting(L"Theme", static_cast<DWORD>(preference));
        ApplyTitleBarTheme(window_);
        InvalidateRect(window_, nullptr, FALSE);
    }
    void SetImageScaling(ImageScaling scaling) {
        if (imageScaling_ == scaling) return;
        imageScaling_ = scaling;
        WriteSetting(L"ImageScaling", static_cast<DWORD>(scaling));
        lanczosSelected_ = imageScaling_ == ImageScaling::Quality;
        InvalidateLanczosVariant(true);
        if (imageScaling_ == ImageScaling::Quality) QueueLanczosRefinement();
        InvalidateRect(window_, nullptr, FALSE);
    }
    void SetModelProjectionMode(ModelProjectionMode mode) {
        if (modelProjectionMode_ == mode) return;
        modelProjectionMode_ = mode;
        WriteSetting(L"ModelProjectionMode", static_cast<DWORD>(mode));
        if (ModelActive()) modelViewport_.SetProjectionMode(mode);
        InvalidateRect(window_, nullptr, FALSE);
    }
    void SetModelUpAxis(ModelUpAxis axis) { if (modelUpAxis_ == axis) return; modelUpAxis_ = axis; WriteSetting(L"ModelUpAxis", static_cast<DWORD>(axis)); if (ModelActive()) modelViewport_.SetBuildPlate(BuildPlateVisible(), ModelUpVector()); InvalidateRect(window_, nullptr, FALSE); }
    void SetModelBuildPlate(ModelBuildPlate mode) { if (modelBuildPlate_ == mode) return; modelBuildPlate_ = mode; WriteSetting(L"ModelBuildPlate", static_cast<DWORD>(mode)); if (ModelActive()) modelViewport_.SetBuildPlate(BuildPlateVisible(), ModelUpVector()); InvalidateRect(window_, nullptr, FALSE); }
    void SetAxisIndicatorPosition(AxisIndicatorPosition position) { if (axisIndicatorPosition_ == position) { axisIndicatorPositionMenuOpen_ = false; InvalidateRect(window_, nullptr, FALSE); return; } axisIndicatorPosition_ = position; WriteSetting(L"AxisIndicatorPosition", static_cast<DWORD>(position)); axisIndicatorPositionMenuOpen_ = false; InvalidateRect(window_, nullptr, FALSE); }
    void SetModelVisualStyle(ModelVisualStyle style) { if(modelVisualStyle_==style)return;modelVisualStyle_=style;WriteSetting(L"ModelVisualStyle",static_cast<DWORD>(style));if(ModelActive())modelViewport_.SetVisualStyle(style);InvalidateRect(window_,nullptr,FALSE); }
    void SetGraphicsAdapterPreference(int option) { if(option==0){graphicsAdapterAuto_=true;}else if(option>0&&option<=static_cast<int>(graphicsAdapters_.size())){graphicsAdapterAuto_=false;graphicsAdapterLuid_=graphicsAdapters_[option-1].luid;}else return;WriteSetting(L"GraphicsAdapterAuto",graphicsAdapterAuto_?1:0);WriteSetting(L"GraphicsAdapterLuidLow",graphicsAdapterLuid_.LowPart);WriteSetting(L"GraphicsAdapterLuidHigh",static_cast<DWORD>(graphicsAdapterLuid_.HighPart));graphicsAdapterMenuOpen_=false;InvalidateRect(window_,nullptr,FALSE); }
    void SetModelAntiAliasing(ModelAntiAliasing mode) { if(modelAntiAliasing_==mode){antiAliasingMenuOpen_=false;InvalidateRect(window_,nullptr,FALSE);return;}modelAntiAliasing_=mode;WriteSetting(L"ModelAntiAliasing",static_cast<DWORD>(mode));if(ModelActive())modelViewport_.SetAntiAliasing(mode);antiAliasingMenuOpen_=false;InvalidateRect(window_,nullptr,FALSE); }
    RECT GetSettingsResetButtonBounds() const {
        const RECT bounds = GetOverlayBounds();
        const UINT dpi = GetDpiForWindow(window_);
        const RECT defaults = GetSettingsDefaultAppsButtonBounds();
        const int description = MeasureSettingsTextHeight(L"removes preferences and app-owned data", SettingsContentRight() - SettingsContentLeft(), 16.0f, DWRITE_FONT_WEIGHT_NORMAL);
        const int top = defaults.bottom + SettingsSectionGap() + MulDiv(20, dpi, 96) + description + MulDiv(8, dpi, 96);
        return { SettingsContentLeft(), top, SettingsContentLeft() + MulDiv(100, dpi, 96), top + MulDiv(36, dpi, 96) };
    }
    void SetZoomHudPosition(ZoomHudPosition position) { zoomHudPosition_ = position; WriteSetting(L"ZoomHudPosition", static_cast<DWORD>(position)); zoomHudPositionMenuOpen_ = false; InvalidateRect(window_, nullptr, FALSE); }
    RECT GetSettingsDefaultAppsButtonBounds() const {
        const UINT dpi = GetDpiForWindow(window_);
        const RECT theme = GetSettingsThemeBounds(ThemePreference::System);
        const int description = MeasureSettingsTextHeight(L"choose which image types open with viewtrious", SettingsContentRight() - SettingsContentLeft(), 16.0f, DWRITE_FONT_WEIGHT_NORMAL);
        const int top = theme.bottom + SettingsSectionGap() + MulDiv(20, dpi, 96) + description + MulDiv(8, dpi, 96);
        return { SettingsContentLeft(), top, SettingsContentLeft() + MulDiv(210, dpi, 96), top + MulDiv(36, dpi, 96) };
    }
    bool SettingsDefaultAppsButtonContains(POINT point) const {
        const RECT button = GetSettingsDefaultAppsButtonBounds();
        point.y += static_cast<LONG>(std::lround(settingsScroll_));
        return overlay_ == OverlayKind::Settings && PtInRect(&button, point);
    }
    bool SettingsResetButtonContains(POINT point) const {
        const RECT button = GetSettingsResetButtonBounds();
        point.y += static_cast<LONG>(std::lround(settingsScroll_));
        return overlay_ == OverlayKind::Settings && PtInRect(&button, point);
    }
    RECT GetResetConfirmationButtonBounds(bool reset) const {
        const RECT bounds = GetOverlayBounds();
        const UINT dpi = GetDpiForWindow(window_);
        const int width = MulDiv(90, dpi, 96), height = MulDiv(36, dpi, 96), gap = MulDiv(10, dpi, 96);
        const int top = bounds.bottom - MulDiv(24, dpi, 96) - height;
        const int resetLeft = bounds.right - MulDiv(24, dpi, 96) - width;
        return reset ? RECT{ resetLeft, top, resetLeft + width, top + height } :
            RECT{ resetLeft - gap - width, top, resetLeft - gap, top + height };
    }
    bool ResetConfirmationButtonContains(POINT point, bool reset) const {
        const RECT button = GetResetConfirmationButtonBounds(reset);
        return overlay_ == OverlayKind::ResetConfirm && PtInRect(&button, point);
    }
    RECT GetDeleteConfirmationButtonBounds(bool confirm) const {
        const RECT bounds = GetOverlayBounds();
        const UINT dpi = GetDpiForWindow(window_);
        const int width = MulDiv(96, dpi, 96), height = MulDiv(36, dpi, 96), gap = MulDiv(10, dpi, 96);
        const int top = bounds.bottom - MulDiv(22, dpi, 96) - height;
        const int confirmLeft = bounds.right - MulDiv(24, dpi, 96) - width;
        return confirm ? RECT{ confirmLeft, top, confirmLeft + width, top + height } :
            RECT{ confirmLeft - gap - width, top, confirmLeft - gap, top + height };
    }
    bool DeleteConfirmationButtonContains(POINT point, bool confirm) const {
        const RECT button = GetDeleteConfirmationButtonBounds(confirm);
        return overlay_ == OverlayKind::DeleteConfirm && PtInRect(&button, point);
    }
    RECT GetDeleteWarningCheckboxBounds() const {
        const RECT bounds = GetOverlayBounds();
        const UINT dpi = GetDpiForWindow(window_);
        const int padding = MulDiv(24, dpi, 96);
        const int top = bounds.top + MulDiv(150, dpi, 96);
        return { bounds.left + padding, top, bounds.right - padding, top + MulDiv(32, dpi, 96) };
    }
    bool DeleteWarningCheckboxContains(POINT point) const {
        const RECT checkbox = GetDeleteWarningCheckboxBounds();
        return overlay_ == OverlayKind::DeleteConfirm && PtInRect(&checkbox, point);
    }
    RECT GetWelcomeButtonBounds(bool primary) const {
        const RECT bounds = GetOverlayBounds();
        const UINT dpi = GetDpiForWindow(window_);
        const int primaryWidth = MulDiv(132, dpi, 96);
        const int secondaryWidth = MulDiv(92, dpi, 96);
        const int height = MulDiv(36, dpi, 96);
        const int gap = MulDiv(10, dpi, 96);
        const int groupWidth = secondaryWidth + gap + primaryWidth;
        const int left = bounds.left + (bounds.right - bounds.left - groupWidth) / 2;
        const int top = bounds.bottom - MulDiv(32, dpi, 96) - height;
        if (primary) return { left + secondaryWidth + gap, top, left + groupWidth, top + height };
        return { left, top, left + secondaryWidth, top + height };
    }
    bool WelcomeButtonContains(POINT point, bool primary) const {
        const RECT button = GetWelcomeButtonBounds(primary);
        return overlay_ == OverlayKind::Welcome && PtInRect(&button, point);
    }
    RECT GetDefaultAppsHelperButtonBounds(bool open) const {
        const RECT bounds = GetOverlayBounds();
        const UINT dpi = GetDpiForWindow(window_);
        const int openWidth = MulDiv(212, dpi, 96), cancelWidth = MulDiv(92, dpi, 96);
        const int height = MulDiv(36, dpi, 96), gap = MulDiv(10, dpi, 96);
        const int groupWidth = cancelWidth + gap + openWidth;
        const int left = bounds.left + (bounds.right - bounds.left - groupWidth) / 2;
        const int top = bounds.bottom - MulDiv(24, dpi, 96) - height;
        return open ? RECT{ left + cancelWidth + gap, top, left + groupWidth, top + height } : RECT{ left, top, left + cancelWidth, top + height };
    }
    bool DefaultAppsHelperButtonContains(POINT point, bool open) const {
        const RECT button = GetDefaultAppsHelperButtonBounds(open);
        return overlay_ == OverlayKind::DefaultAppsHelper && PtInRect(&button, point);
    }
    RECT GetFeedbackActionBounds(bool feature) const {
        const RECT bounds = GetOverlayBounds();
        const UINT dpi = GetDpiForWindow(window_);
        const int padding = MulDiv(24, dpi, 96);
        const int top = bounds.top + padding + MulDiv(feature ? 174 : 92, dpi, 96);
        return { bounds.left + padding, top, bounds.right - padding, top + MulDiv(64, dpi, 96) };
    }
    bool FeedbackActionContains(POINT point, bool feature) const {
        const RECT button = GetFeedbackActionBounds(feature);
        return overlay_ == OverlayKind::Feedback && PtInRect(&button, point);
    }
    bool CanvasNavigationButtonsVisible() const {
        return source_ && navigationBuilt_ && navigationFiles_.size() > 1 &&
            !HasOverlay() && !TutorialActive() && !dropdownOpen_ && !contextMenuOpen_;
    }
    RECT GetCanvasNavigationZoneBounds(bool next) const {
        RECT client{};
        GetClientRect(window_, &client);
        const UINT dpi = GetDpiForWindow(window_);
        const int canvasTop = fullscreen_ ? 0 : GetFrameMetrics(window_).titleBarHeight;
        const int desiredWidth = MulDiv(112, dpi, 96);
        const int minimumCenterWidth = MulDiv(160, dpi, 96);
        const int zoneWidth = std::min(desiredWidth, static_cast<int>(std::max(0L, (client.right - minimumCenterWidth) / 2)));
        return next ? RECT{ client.right - zoneWidth, canvasTop, client.right, client.bottom } :
            RECT{ 0, canvasTop, zoneWidth, client.bottom };
    }
    ButtonKind CanvasNavigationZoneAt(POINT point) const {
        if (!CanvasNavigationButtonsVisible()) return ButtonKind::None;
        const RECT previous = GetCanvasNavigationZoneBounds(false);
        if (PtInRect(&previous, point)) return ButtonKind::CanvasPrevious;
        const RECT next = GetCanvasNavigationZoneBounds(true);
        return PtInRect(&next, point) ? ButtonKind::CanvasNext : ButtonKind::None;
    }
    bool EmptyStateActive() const {
        return contentKind_ != ContentKind::Model3D && contentKind_ != ContentKind::Video2D && source_ == nullptr;
    }
    ButtonKind ButtonAt(POINT point) const {
        const auto contains = [&point](RECT bounds) { return PtInRect(&bounds, point) != FALSE; };
        if (TutorialButtonContains(point, false)) return ButtonKind::TutorialSkip;
        if (TutorialButtonContains(point, true)) return ButtonKind::TutorialNext;
        if (EmptyStateActive() && EmptyOpenFileButtonContains(point)) return ButtonKind::EmptyOpenFile;
        if (overlay_ == OverlayKind::Settings) {
            const auto containsNavigation = [&point, this](SettingsPage page) {
                const RECT navigation = GetSettingsNavigationBounds(page);
                return PtInRect(&navigation, point) != FALSE;
            };
            if (containsNavigation(SettingsPage::General)) return ButtonKind::SettingsGeneralPage;
            if (containsNavigation(SettingsPage::Image2D)) return ButtonKind::SettingsImage2DPage;
            if (containsNavigation(SettingsPage::Model3D)) return ButtonKind::SettingsModel3DPage;
            POINT settingsPoint = point;
            settingsPoint.y += static_cast<LONG>(std::lround(settingsScroll_));
            const auto settingsContains = [&settingsPoint](RECT bounds) { return PtInRect(&bounds, settingsPoint) != FALSE; };
            if (settingsPage_ == SettingsPage::General) {
                if (settingsContains(GetSettingsToggleBounds(0, kSettingsFirstRowTopDips))) return ButtonKind::SettingsRememberPlacement;
                if (settingsContains(GetSettingsToggleBounds(1, kSettingsFirstRowTopDips))) return ButtonKind::SettingsConfirmDelete;
                if (settingsContains(GetSettingsThemeBounds(ThemePreference::System))) return ButtonKind::SettingsThemeSystem;
                if (settingsContains(GetSettingsThemeBounds(ThemePreference::Light))) return ButtonKind::SettingsThemeLight;
                if (settingsContains(GetSettingsThemeBounds(ThemePreference::Dark))) return ButtonKind::SettingsThemeDark;
                if (SettingsDefaultAppsButtonContains(point)) return ButtonKind::SettingsDefaultApps;
            } else if (settingsPage_ == SettingsPage::Image2D) {
                if (zoomHudPositionMenuOpen_) { const RECT menu = GetSettingsZoomHudMenuBounds(); if (PtInRect(&menu, settingsPoint)) { const int row = MulDiv(30, GetDpiForWindow(window_), 96); return settingsPoint.y < menu.top + row ? ButtonKind::SettingsZoomHudBottomLeft : settingsPoint.y < menu.top + row * 2 ? ButtonKind::SettingsZoomHudBottomRight : settingsPoint.y < menu.top + row * 3 ? ButtonKind::SettingsZoomHudTopLeft : ButtonKind::SettingsZoomHudTopRight; } }
                if (SettingsScrollUpVisible() && SettingsScrollIndicatorContains(point, true)) return ButtonKind::SettingsScrollUp;
                if (SettingsScrollDownVisible() && SettingsScrollIndicatorContains(point, false)) return ButtonKind::SettingsScrollDown;
                if (settingsContains(GetSettingsOptionBounds(1))) return ButtonKind::SettingsIncludeHidden;
                if (settingsContains(GetSettingsOptionBounds(4))) return ButtonKind::SettingsAnimations;
                if (settingsContains(GetSettingsOptionBounds(5))) return ButtonKind::SettingsReverseWheelZoom;
                if (settingsContains(GetSettingsZoomHudBounds())) return ButtonKind::SettingsZoomHudPositionToggle;
                if (settingsContains(GetSettingsScalingBounds(ImageScaling::Quality))) return ButtonKind::SettingsScalingQuality;
                if (settingsContains(GetSettingsScalingBounds(ImageScaling::Performance))) return ButtonKind::SettingsScalingPerformance;
            } else if (settingsPage_ == SettingsPage::Model3D) {
                const int row=MulDiv(30,GetDpiForWindow(window_),96);
                if (upAxisMenuOpen_) { const RECT menu=GetSettingsUpAxisMenuBounds(); if (PtInRect(&menu,settingsPoint)) return settingsPoint.y < menu.top+row ? ButtonKind::SettingsUpAxisZ : settingsPoint.y < menu.top+row*2 ? ButtonKind::SettingsUpAxisY : ButtonKind::SettingsUpAxisX; }
                if (buildPlateMenuOpen_) { const RECT menu=GetSettingsBuildPlateMenuBounds(); if (PtInRect(&menu,settingsPoint)) return settingsPoint.y < menu.top+row ? ButtonKind::SettingsBuildPlateAuto : settingsPoint.y < menu.top+row*2 ? ButtonKind::SettingsBuildPlateOn : ButtonKind::SettingsBuildPlateOff; }
                if (axisIndicatorPositionMenuOpen_) { const RECT menu=GetSettingsAxisIndicatorPositionMenuBounds(); if (PtInRect(&menu,settingsPoint)) return settingsPoint.y < menu.top+row ? ButtonKind::SettingsAxisIndicatorBottomLeft : settingsPoint.y < menu.top+row*2 ? ButtonKind::SettingsAxisIndicatorBottomRight : settingsPoint.y < menu.top+row*3 ? ButtonKind::SettingsAxisIndicatorTopLeft : ButtonKind::SettingsAxisIndicatorTopRight; }
                if (projectionMenuOpen_) { const RECT menu=GetSettingsProjectionMenuBounds(); if (PtInRect(&menu,settingsPoint)) return settingsPoint.y < menu.top+row ? ButtonKind::SettingsProjectionPerspective : ButtonKind::SettingsProjectionOrthographic; }
                if (graphicsAdapterMenuOpen_) { const RECT menu=GetSettingsGraphicsAdapterMenuBounds();if(PtInRect(&menu,settingsPoint)){const int option=(settingsPoint.y-menu.top)/row;if(graphicsAdapterMenuOption_!=option){graphicsAdapterMenuOption_=option;InvalidateRect(window_,nullptr,FALSE);}return ButtonKind::SettingsGraphicsAdapterOption;} }
                if (antiAliasingMenuOpen_) { const RECT menu=GetSettingsAntiAliasingMenuBounds();if(PtInRect(&menu,settingsPoint)) { const int index=(settingsPoint.y-menu.top)/row;if(modelVisualStyle_==ModelVisualStyle::Wireframe&&index>=4)return ButtonKind::None;return index==0?ButtonKind::SettingsAntiAliasingOff:index==1?ButtonKind::SettingsAntiAliasing2x:index==2?ButtonKind::SettingsAntiAliasing4x:index==3?ButtonKind::SettingsAntiAliasing8x:index==4?ButtonKind::SettingsAntiAliasingSsaa1_5x:ButtonKind::SettingsAntiAliasingSsaa2x; } }
                if (settingsContains(GetSettingsUpAxisBounds())) return ButtonKind::SettingsUpAxisToggle;
                if (settingsContains(GetSettingsBuildPlateBounds())) return ButtonKind::SettingsBuildPlateToggle;
                if (settingsContains(GetSettingsAxisIndicatorPositionBounds())) return ButtonKind::SettingsAxisIndicatorPositionToggle;
                if (settingsContains(GetSettingsProjectionBounds())) return ButtonKind::SettingsProjectionToggle;
                if (spaceMouseRuntimeAvailable_ && settingsContains(GetSettingsSpaceMouseBounds())) return ButtonKind::SettingsSpaceMouse;
                if (settingsContains(GetSettingsGraphicsAdapterBounds())) return ButtonKind::SettingsGraphicsAdapterToggle;
                if (settingsContains(GetSettingsAntiAliasingBounds())) return ButtonKind::SettingsAntiAliasingToggle;
            }
            if (SettingsScrollUpVisible() && SettingsScrollIndicatorContains(point, true)) return ButtonKind::SettingsScrollUp;
            if (SettingsScrollDownVisible() && SettingsScrollIndicatorContains(point, false)) return ButtonKind::SettingsScrollDown;
        }
        if (ModelActive()) {
            if (OffscreenModelIndicatorContains(point)) return ButtonKind::ModelOffscreenIndicator;
            if (viewBarProjectionMenuOpen_) { const RECT menu = GetModelViewBarProjectionMenuBounds(); const int row = MulDiv(32, GetDpiForWindow(window_), 96); if (PtInRect(&menu, point)) return point.y < menu.top + row ? ButtonKind::ViewBarProjectionPerspective : ButtonKind::ViewBarProjectionOrthographic; }
            if (viewBarVisualStyleMenuOpen_) { const RECT menu = GetModelViewBarStyleMenuBounds(); const int row = MulDiv(32, GetDpiForWindow(window_), 96); if (PtInRect(&menu, point)) return point.y < menu.top+row ? ButtonKind::ViewBarVisualStyleShaded : point.y < menu.top+row*2 ? ButtonKind::ViewBarVisualStyleVisibleEdges : ButtonKind::ViewBarVisualStyleWireframe; }
            const RECT projection=GetModelViewBarProjectionBounds(),style=GetModelViewBarStyleBounds();if(PtInRect(&projection,point))return ButtonKind::ViewBarProjectionToggle;if(PtInRect(&style,point))return ButtonKind::ViewBarVisualStyleToggle;
        }
        if (settingsPage_ == SettingsPage::General && SettingsResetButtonContains(point)) return ButtonKind::SettingsReset;
        if (ResetConfirmationButtonContains(point, false)) return ButtonKind::ResetCancel;
        if (ResetConfirmationButtonContains(point, true)) return ButtonKind::ResetConfirm;
        if (DeleteWarningCheckboxContains(point)) return ButtonKind::DeleteWarningSuppress;
        if (DeleteConfirmationButtonContains(point, false)) return ButtonKind::DeleteCancel;
        if (DeleteConfirmationButtonContains(point, true)) return ButtonKind::DeleteConfirm;
        if (WelcomeButtonContains(point, false)) return ButtonKind::WelcomeSecondary;
        if (WelcomeButtonContains(point, true)) return ButtonKind::WelcomePrimary;
        if (DefaultAppsHelperButtonContains(point, false)) return ButtonKind::DefaultAppsHelperCancel;
        if (DefaultAppsHelperButtonContains(point, true)) return ButtonKind::DefaultAppsHelperOpen;
        if (FeedbackActionContains(point, false)) return ButtonKind::FeedbackBug;
        if (FeedbackActionContains(point, true)) return ButtonKind::FeedbackFeature;
        return ButtonKind::None;
    }
    void SetButtonHover(ButtonKind button) {
        if (hoveredButton_ == button) return;
        hoveredButton_ = button;
        InvalidateRect(window_, nullptr, FALSE);
    }
    void AdvanceCanvasNavigationFade() {
        if (!canvasNavigationFadeActive_) return;
        const ULONGLONG elapsed = GetTickCount64() - canvasNavigationFadeStart_;
        const auto advance = [elapsed](float& value, float start, float target) {
            const float duration = target > start ? 100.0f : 180.0f;
            const float progress = std::min(1.0f, static_cast<float>(elapsed) / duration);
            value = start + (target - start) * progress;
            return progress < 1.0f;
        };
        const bool previousActive = advance(canvasPreviousOpacity_, canvasPreviousFadeStartOpacity_, canvasPreviousTargetOpacity_);
        const bool nextActive = advance(canvasNextOpacity_, canvasNextFadeStartOpacity_, canvasNextTargetOpacity_);
        canvasNavigationFadeActive_ = previousActive || nextActive;
        if (!canvasNavigationFadeActive_) KillTimer(window_, kCanvasNavigationFadeTimer);
    }
    void UpdateCanvasNavigationOpacity() {
        AdvanceCanvasNavigationFade();
        const auto targetFor = [this](ButtonKind button) {
            if (canvasNavigationPressed_ == button) return 0.86f;
            if (canvasNavigationHovered_ == button) return 0.72f;
            return 0.08f;
        };
        const float previousTarget = targetFor(ButtonKind::CanvasPrevious);
        const float nextTarget = targetFor(ButtonKind::CanvasNext);
        if (!animationsEnabled_) {
            KillTimer(window_, kCanvasNavigationFadeTimer);
            canvasNavigationFadeActive_ = false;
            canvasPreviousOpacity_ = canvasPreviousTargetOpacity_ = previousTarget;
            canvasNextOpacity_ = canvasNextTargetOpacity_ = nextTarget;
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (std::abs(previousTarget - canvasPreviousOpacity_) < 0.001f && std::abs(nextTarget - canvasNextOpacity_) < 0.001f) return;
        canvasPreviousFadeStartOpacity_ = canvasPreviousOpacity_;
        canvasNextFadeStartOpacity_ = canvasNextOpacity_;
        canvasPreviousTargetOpacity_ = previousTarget;
        canvasNextTargetOpacity_ = nextTarget;
        canvasNavigationFadeStart_ = GetTickCount64();
        canvasNavigationFadeActive_ = true;
        SetTimer(window_, kCanvasNavigationFadeTimer, 16, nullptr);
        InvalidateRect(window_, nullptr, FALSE);
    }
    void UpdateCanvasNavigationFade() {
        AdvanceCanvasNavigationFade();
        if (canvasNavigationFadeActive_) InvalidateRect(window_, nullptr, FALSE);
    }
    void SetCanvasNavigationHover(ButtonKind button) {
        if (button != ButtonKind::CanvasPrevious && button != ButtonKind::CanvasNext) button = ButtonKind::None;
        if (canvasNavigationHovered_ == button) return;
        canvasNavigationHovered_ = button;
        UpdateCanvasNavigationOpacity();
    }
    void BeginCanvasNavigationClick(ButtonKind button, POINT point) {
        canvasNavigationPressed_ = button;
        canvasNavigationPressPoint_ = point;
        UpdateCanvasNavigationOpacity();
    }
    bool CanvasNavigationPressed() const { return canvasNavigationPressed_ != ButtonKind::None; }
    bool ContinueCanvasNavigationClick(POINT point) {
        if (!CanvasNavigationPressed()) return false;
        const int dragX = GetSystemMetrics(SM_CXDRAG);
        const int dragY = GetSystemMetrics(SM_CYDRAG);
        if (std::abs(point.x - canvasNavigationPressPoint_.x) <= dragX && std::abs(point.y - canvasNavigationPressPoint_.y) <= dragY) return false;
        BeginPan(canvasNavigationPressPoint_);
        if (!dragging_) return false;
        canvasNavigationPressed_ = ButtonKind::None;
        UpdateCanvasNavigationOpacity();
        PanTo(point);
        return true;
    }
    ButtonKind FinishCanvasNavigationClick(POINT point) {
        const ButtonKind pressed = canvasNavigationPressed_;
        canvasNavigationPressed_ = ButtonKind::None;
        UpdateCanvasNavigationOpacity();
        return pressed != ButtonKind::None && pressed == CanvasNavigationZoneAt(point) ? pressed : ButtonKind::None;
    }
    void CancelCanvasNavigationClick() {
        if (!CanvasNavigationPressed()) return;
        canvasNavigationPressed_ = ButtonKind::None;
        UpdateCanvasNavigationOpacity();
    }
    void SetButtonPressed(ButtonKind button) {
        if (pressedButton_ == button) return;
        pressedButton_ = button;
        InvalidateRect(window_, nullptr, FALSE);
    }
    ButtonKind PressedButton() const { return pressedButton_; }
    void ClearButtonPressed(bool invalidate = true) {
        if (pressedButton_ == ButtonKind::None) return;
        pressedButton_ = ButtonKind::None;
        if (invalidate) InvalidateRect(window_, nullptr, FALSE);
    }
    void InvokeButton(ButtonKind button) {
        if (button == ButtonKind::EmptyOpenFile) OpenFile();
        else if (button == ButtonKind::CanvasPrevious) Navigate(-1);
        else if (button == ButtonKind::CanvasNext) Navigate(1);
        else if (button == ButtonKind::SettingsGeneralPage) { settingsPage_ = SettingsPage::General; settingsScroll_ = 0.0f; InvalidateRect(window_, nullptr, FALSE); }
        else if (button == ButtonKind::SettingsImage2DPage) { settingsPage_ = SettingsPage::Image2D; settingsScroll_ = 0.0f; InvalidateRect(window_, nullptr, FALSE); }
        else if (button == ButtonKind::SettingsModel3DPage) { settingsPage_ = SettingsPage::Model3D; settingsScroll_ = 0.0f; InvalidateRect(window_, nullptr, FALSE); }
        else if (button == ButtonKind::SettingsRememberPlacement) ToggleRememberWindowPlacement();
        else if (button == ButtonKind::SettingsIncludeHidden) ToggleIncludeHiddenImages();
        else if (button == ButtonKind::SettingsConfirmDelete) ToggleConfirmBeforeDeleting();
        else if (button == ButtonKind::SettingsShowZoomHud) ToggleShowZoomPercentage();
        else if (button == ButtonKind::SettingsZoomHudPositionToggle) { zoomHudPositionMenuOpen_ = !zoomHudPositionMenuOpen_; InvalidateRect(window_, nullptr, FALSE); }
        else if (button == ButtonKind::SettingsZoomHudBottomLeft) SetZoomHudPosition(ZoomHudPosition::BottomLeft);
        else if (button == ButtonKind::SettingsZoomHudBottomRight) SetZoomHudPosition(ZoomHudPosition::BottomRight);
        else if (button == ButtonKind::SettingsZoomHudTopLeft) SetZoomHudPosition(ZoomHudPosition::TopLeft);
        else if (button == ButtonKind::SettingsZoomHudTopRight) SetZoomHudPosition(ZoomHudPosition::TopRight);
        else if (button == ButtonKind::SettingsScrollUp) ScrollSettings(static_cast<float>(-MulDiv(180, GetDpiForWindow(window_), 96)));
        else if (button == ButtonKind::SettingsScrollDown) ScrollSettings(static_cast<float>(MulDiv(180, GetDpiForWindow(window_), 96)));
        else if (button == ButtonKind::SettingsAnimations) ToggleAnimationsAndFadeEffects();
        else if (button == ButtonKind::SettingsReverseWheelZoom) ToggleReverseMouseWheelZoom();
        else if (button == ButtonKind::SettingsSpaceMouse) ToggleSpaceMouse();
        else if (button == ButtonKind::SettingsUpAxisToggle) { upAxisMenuOpen_=!upAxisMenuOpen_; buildPlateMenuOpen_=axisIndicatorPositionMenuOpen_=projectionMenuOpen_=graphicsAdapterMenuOpen_=antiAliasingMenuOpen_=false; InvalidateRect(window_,nullptr,FALSE); }
        else if (button == ButtonKind::SettingsUpAxisZ) { SetModelUpAxis(ModelUpAxis::ZUp); upAxisMenuOpen_=false; }
        else if (button == ButtonKind::SettingsUpAxisY) { SetModelUpAxis(ModelUpAxis::YUp); upAxisMenuOpen_=false; }
        else if (button == ButtonKind::SettingsUpAxisX) { SetModelUpAxis(ModelUpAxis::XUp); upAxisMenuOpen_=false; }
        else if (button == ButtonKind::SettingsBuildPlateToggle) { buildPlateMenuOpen_=!buildPlateMenuOpen_; upAxisMenuOpen_=axisIndicatorPositionMenuOpen_=projectionMenuOpen_=graphicsAdapterMenuOpen_=antiAliasingMenuOpen_=false; InvalidateRect(window_,nullptr,FALSE); }
        else if (button == ButtonKind::SettingsBuildPlateAuto) { SetModelBuildPlate(ModelBuildPlate::Auto); buildPlateMenuOpen_=false; }
        else if (button == ButtonKind::SettingsBuildPlateOn) { SetModelBuildPlate(ModelBuildPlate::On); buildPlateMenuOpen_=false; }
        else if (button == ButtonKind::SettingsBuildPlateOff) { SetModelBuildPlate(ModelBuildPlate::Off); buildPlateMenuOpen_=false; }
        else if (button == ButtonKind::SettingsAxisIndicatorPositionToggle) { axisIndicatorPositionMenuOpen_=!axisIndicatorPositionMenuOpen_; upAxisMenuOpen_=buildPlateMenuOpen_=projectionMenuOpen_=graphicsAdapterMenuOpen_=antiAliasingMenuOpen_=false; InvalidateRect(window_,nullptr,FALSE); }
        else if (button == ButtonKind::SettingsAxisIndicatorBottomLeft) SetAxisIndicatorPosition(AxisIndicatorPosition::BottomLeft);
        else if (button == ButtonKind::SettingsAxisIndicatorBottomRight) SetAxisIndicatorPosition(AxisIndicatorPosition::BottomRight);
        else if (button == ButtonKind::SettingsAxisIndicatorTopLeft) SetAxisIndicatorPosition(AxisIndicatorPosition::TopLeft);
        else if (button == ButtonKind::SettingsAxisIndicatorTopRight) SetAxisIndicatorPosition(AxisIndicatorPosition::TopRight);
        else if (button == ButtonKind::SettingsProjectionToggle) { projectionMenuOpen_=!projectionMenuOpen_; upAxisMenuOpen_=buildPlateMenuOpen_=axisIndicatorPositionMenuOpen_=graphicsAdapterMenuOpen_=antiAliasingMenuOpen_=false; InvalidateRect(window_,nullptr,FALSE); }
        else if (button == ButtonKind::SettingsProjectionPerspective) { SetModelProjectionMode(ModelProjectionMode::Perspective); projectionMenuOpen_=false; }
        else if (button == ButtonKind::SettingsProjectionOrthographic) { SetModelProjectionMode(ModelProjectionMode::Orthographic); projectionMenuOpen_=false; }
        else if (button == ButtonKind::SettingsGraphicsAdapterToggle) { graphicsAdapters_=GraphicsHost::EnumerateHardwareAdapters();graphicsAdapterMenuOpen_=!graphicsAdapterMenuOpen_;upAxisMenuOpen_=buildPlateMenuOpen_=axisIndicatorPositionMenuOpen_=projectionMenuOpen_=antiAliasingMenuOpen_=false;InvalidateRect(window_,nullptr,FALSE); }
        else if (button == ButtonKind::SettingsGraphicsAdapterOption) SetGraphicsAdapterPreference(graphicsAdapterMenuOption_);
        else if (button == ButtonKind::SettingsAntiAliasingToggle) { antiAliasingMenuOpen_=!antiAliasingMenuOpen_;upAxisMenuOpen_=buildPlateMenuOpen_=axisIndicatorPositionMenuOpen_=projectionMenuOpen_=graphicsAdapterMenuOpen_=false; InvalidateRect(window_,nullptr,FALSE); }
        else if (button == ButtonKind::SettingsAntiAliasingOff) SetModelAntiAliasing(ModelAntiAliasing::Off);
        else if (button == ButtonKind::SettingsAntiAliasing2x) SetModelAntiAliasing(ModelAntiAliasing::Msaa2x);
        else if (button == ButtonKind::SettingsAntiAliasing4x) SetModelAntiAliasing(ModelAntiAliasing::Msaa4x);
        else if (button == ButtonKind::SettingsAntiAliasing8x) SetModelAntiAliasing(ModelAntiAliasing::Msaa8x);
        else if (button == ButtonKind::SettingsAntiAliasingSsaa1_5x && modelVisualStyle_ != ModelVisualStyle::Wireframe) SetModelAntiAliasing(ModelAntiAliasing::Ssaa1_5x);
        else if (button == ButtonKind::SettingsAntiAliasingSsaa2x && modelVisualStyle_ != ModelVisualStyle::Wireframe) SetModelAntiAliasing(ModelAntiAliasing::Ssaa2x);
        else if (button == ButtonKind::ModelOffscreenIndicator) BeginAnimatedModelFramingRecovery();
        else if (button == ButtonKind::ViewBarProjectionToggle) { viewBarProjectionMenuOpen_ = !viewBarProjectionMenuOpen_; viewBarVisualStyleMenuOpen_=false; InvalidateRect(window_, nullptr, FALSE); }
        else if (button == ButtonKind::ViewBarProjectionPerspective) { SetModelProjectionMode(ModelProjectionMode::Perspective); DismissModelViewBarMenu(); }
        else if (button == ButtonKind::ViewBarProjectionOrthographic) { SetModelProjectionMode(ModelProjectionMode::Orthographic); DismissModelViewBarMenu(); }
        else if (button == ButtonKind::ViewBarVisualStyleToggle) { viewBarVisualStyleMenuOpen_=!viewBarVisualStyleMenuOpen_;viewBarProjectionMenuOpen_=false;InvalidateRect(window_,nullptr,FALSE); }
        else if (button == ButtonKind::ViewBarVisualStyleShaded) { SetModelVisualStyle(ModelVisualStyle::Shaded);DismissModelViewBarMenu(); }
        else if (button == ButtonKind::ViewBarVisualStyleVisibleEdges) { SetModelVisualStyle(ModelVisualStyle::ShadedWithVisibleEdges);DismissModelViewBarMenu(); }
        else if (button == ButtonKind::ViewBarVisualStyleWireframe) { SetModelVisualStyle(ModelVisualStyle::Wireframe);DismissModelViewBarMenu(); }
        else if (button == ButtonKind::SettingsThemeSystem) SetThemePreference(ThemePreference::System);
        else if (button == ButtonKind::SettingsThemeLight) SetThemePreference(ThemePreference::Light);
        else if (button == ButtonKind::SettingsThemeDark) SetThemePreference(ThemePreference::Dark);
        else if (button == ButtonKind::SettingsScalingPerformance) SetImageScaling(ImageScaling::Performance);
        else if (button == ButtonKind::SettingsScalingQuality) SetImageScaling(ImageScaling::Quality);
        else if (button == ButtonKind::SettingsDefaultApps) OpenRegisteredDefaultApps();
        else if (button == ButtonKind::SettingsReset) ShowOverlay(OverlayKind::ResetConfirm);
        else if (button == ButtonKind::ResetCancel) DismissOverlay();
        else if (button == ButtonKind::ResetConfirm) ResetToDefaults();
        else if (button == ButtonKind::DeleteWarningSuppress) { deleteWarningSuppressOnConfirm_ = !deleteWarningSuppressOnConfirm_; InvalidateRect(window_, nullptr, FALSE); }
        else if (button == ButtonKind::DeleteCancel) DismissOverlay();
        else if (button == ButtonKind::DeleteConfirm) {
            if (deleteWarningSuppressOnConfirm_) {
                confirmBeforeDeleting_ = false;
                WriteSetting(L"ConfirmBeforeDeleting", 0);
            }
            DismissOverlay();
            DeleteImage();
        }
        else if (button == ButtonKind::WelcomeSecondary) CompleteWelcome(false);
        else if (button == ButtonKind::WelcomePrimary) ShowOverlay(OverlayKind::DefaultAppsHelper);
        else if (button == ButtonKind::DefaultAppsHelperCancel) {
            overlay_ = OverlayKind::Welcome;
            CompleteWelcome(false);
        }
        else if (button == ButtonKind::DefaultAppsHelperOpen) {
            overlay_ = OverlayKind::Welcome;
            CompleteWelcome(true);
            OpenRegisteredDefaultApps();
        }
        else if (button == ButtonKind::FeedbackBug || button == ButtonKind::FeedbackFeature) {
            DismissOverlay();
            const wchar_t* url = button == ButtonKind::FeedbackBug ? kBugReportUrl : kFeatureRequestUrl;
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", url, nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                ShowActionError(L"Viewtrious couldn't open the feedback page.");
        }
        else if (button == ButtonKind::TutorialSkip) StopTutorial();
        else if (button == ButtonKind::TutorialNext) AdvanceTutorial();
    }
    void CompleteWelcome(bool waitForActivation) {
        if (overlay_ != OverlayKind::Welcome) return;
        WriteSetting(L"OnboardingVersion", 1);
        WriteSetting(L"TourPending", 1);
        onboardingRequired_ = false;
        tourPending_ = true;
        DismissOverlay();
        if (!waitForActivation) StartPendingTour();
    }
    void StartPendingTour() {
        if (!tourPending_) return;
        tourPending_ = false;
        WriteSetting(L"TourPending", 0);
        StartTutorial();
    }
    void ResetToDefaults() {
        resetInProgress_ = true;
        RegDeleteTreeW(HKEY_CURRENT_USER, kSettingsKey);
        CleanupWallpaperStaging();
        rememberWindowPlacement_ = true;
        includeHiddenImages_ = true;
        confirmBeforeDeleting_ = true;
        showZoomPercentage_ = true;
        animationsEnabled_ = true;
        reverseMouseWheelZoom_ = false;
        themePreference_ = ThemePreference::System;
        graphicsAdapterAuto_ = true;
        graphicsAdapterLuid_ = {};
        wchar_t modulePath[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath))) { DestroyWindow(window_); return; }
        std::wstring command = L"\"" + std::wstring(modulePath) + L"\"";
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
            resetInProgress_ = false;
            ShowActionError(L"Viewtrious preferences were reset. Please close and reopen Viewtrious to continue.");
            return;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        DestroyWindow(window_);
    }
    bool EmptyOpenFileButtonContains(POINT point) const {
        if (!EmptyStateActive() || tutorialPresentation_ || HasOverlay() || dropdownOpen_ || contextMenuOpen_) return false;
        const RECT bounds = GetEmptyOpenFileButtonBounds();
        return PtInRect(&bounds, point);
    }
    void ToggleFullscreen() {
        DismissContextMenu();
        if (!fullscreen_) DismissOverlay();
        if (!fullscreen_) {
            fullscreenStyle_ = GetWindowLongPtrW(window_, GWL_STYLE);
            GetWindowRect(window_, &fullscreenRect_);
            MONITORINFO monitor{ sizeof(monitor) };
            GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
            SetWindowLongPtrW(window_, GWL_STYLE, fullscreenStyle_ & ~WS_OVERLAPPEDWINDOW);
            fullscreen_ = true;
            SetWindowPos(window_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            ApplyWindowCornerPreference(window_, false);
        } else {
            SetWindowLongPtrW(window_, GWL_STYLE, fullscreenStyle_);
            fullscreen_ = false;
            SetWindowPos(window_, HWND_NOTOPMOST, fullscreenRect_.left, fullscreenRect_.top,
                fullscreenRect_.right - fullscreenRect_.left, fullscreenRect_.bottom - fullscreenRect_.top,
                SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            ApplyWindowCornerPreference(window_, !IsZoomed(window_));
        }
    }
    void Paint() {
        PAINTSTRUCT paint{};
        BeginPaint(window_, &paint);
        const bool videoPaint = VideoActive();
        if (videoPaint) VideoPlayer::Trace(window_, L"Video2D paint begin");
        EnsureRenderTarget();
        if (renderTarget_ && graphicsHost_.Ready()) {
            if (ModelActive() && !TutorialActive()) modelViewport_.Render(graphicsHost_, ModelCanvasBounds());
            graphicsHost_.BeginDraw();
            if (contentKind_ != ContentKind::Model3D || !ModelActive() || TutorialActive()) renderTarget_->Clear(kViewerBackground);
            if (VideoActive() && !tutorialPresentation_) videoPlayer_.Draw(renderTarget_.Get(), ModelCanvasBounds());
            if (VideoActive() && !tutorialPresentation_) DrawVideoPlaybackControls();
            if (source_ && !tutorialPresentation_) {
                EnsureBitmap();
                if (bitmap_) { DrawImage(); DrawZoomHud(); DrawCanvasNavigationButtons(); }
            } else if (EmptyStateActive()) DrawEmptyState();
            if (ModelActive() && !tutorialPresentation_) { DrawModelAxisIndicator(); TraceOffscreenModelIndicatorState(); DrawOffscreenModelIndicator(); DrawModelViewBar(); }
            if (!tutorialPresentation_) DrawModelLoadingOverlay();
            if (!tutorialPresentation_) DrawRevisionLabel();
            if (videoPaint) VideoPlayer::Trace(window_, L"Video2D overlay drawing begin");
            DrawTitleBar();
            DrawTriangleCountTooltip();
            DrawDropdown();
            DrawContextMenu();
            DrawOpenWithSubmenu();
            DrawOverlay();
            if (!tutorialPresentation_) DrawCopyFeedback();
            DrawTutorial();
            if (videoPaint) VideoPlayer::Trace(window_, L"Video2D overlay drawing end");
            if (videoPaint) VideoPlayer::Trace(window_, L"Video2D EndDraw begin");
            const HRESULT hr = graphicsHost_.EndDraw();
            if (videoPaint) VideoPlayer::Trace(window_, L"Video2D EndDraw end", hr);
            if (SUCCEEDED(hr) && bitmap_ && !tutorialPresentation_) MarkFirstPresentation();
            if (hr == D2DERR_RECREATE_TARGET) DiscardRenderResources();
            else if (SUCCEEDED(hr)) {
                if (videoPaint) VideoPlayer::Trace(window_, L"Video2D Present begin");
                const HRESULT present = graphicsHost_.Present();
                if (videoPaint) VideoPlayer::Trace(window_, L"Video2D Present end", present);
            }
        }
        EndPaint(window_, &paint);
        if (videoPaint) VideoPlayer::Trace(window_, L"Video2D paint end/return");
    }

    void Resize() {
        if (graphicsHost_.Ready()) {
            RECT client{};
            GetClientRect(window_, &client);
            std::wstring error;
            if (!graphicsHost_.Resize(std::max(1L, client.right - client.left), std::max(1L, client.bottom - client.top), static_cast<float>(GetDpiForWindow(window_)), error)) error_ = error;
            renderTarget_ = graphicsHost_.D2DContext();
            bitmap_.Reset(); lanczosBitmap_.Reset(); aboutLogo_.Reset(); aboutLogoWidth_ = 0; aboutLogoHeight_ = 0;
            topBarLogo_.Reset(); topBarLogoWidth_ = 0; topBarLogoHeight_ = 0; checkerboardBrush_.Reset(); checkerboardBitmap_.Reset();
            if (VideoActive()) videoPlayer_.HandleRenderTargetResize();
        }
        if (!tutorialPresentation_ && !fitToWindow_ && zoom_ < BaseScale()) FitToWindow();
        settingsScroll_ = std::min(settingsScroll_, SettingsMaximumScroll());
        ClampPan();
        if (lanczosSelected_ && source_) {
            InvalidateLanczosVariant(true);
            QueueLanczosRefinement();
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    SIZE SuggestedClientSize() const {
        if (!source_) return { 800, 600 };
        constexpr double maxWidth = 1280.0;
        constexpr double maxHeight = 900.0;
        const double scale = std::min({ 1.0, maxWidth / imageWidth_, maxHeight / imageHeight_ });
        return { static_cast<LONG>(std::lround(imageWidth_ * scale)),
                 static_cast<LONG>(std::lround(imageHeight_ * scale)) };
    }

    void DropFile(HDROP drop) {
        if (WelcomeOpen() || TutorialActive()) { DragFinish(drop); return; }
        const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
        if (length > 0) {
            std::wstring path(length + 1, L'\0');
            DragQueryFileW(drop, 0, path.data(), length + 1);
            path.resize(length);
            LoadContent(path);
            InvalidateRect(window_, nullptr, FALSE);
        }
        DragFinish(drop);
    }

    void BuildNavigation(bool refresh = false) {
        navigationBuildQueued_ = false;
        if ((navigationBuilt_ && !refresh) || currentPath_.empty()) return;

        fs::path current(currentPath_);
        std::vector<fs::path> scannedFiles;
        std::error_code error;
        fs::directory_iterator iterator(current.parent_path(), error);
        for (; !error && iterator != fs::directory_iterator(); iterator.increment(error)) {
            std::error_code typeError;
            const DWORD attributes = GetFileAttributesW(iterator->path().c_str());
            const bool hidden = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
            if (iterator->is_regular_file(typeError) && !typeError && IsTwoDimensionalMediaPath(iterator->path()) &&
                (includeHiddenImages_ || !hidden)) {
                scannedFiles.push_back(iterator->path());
            }
        }
        const auto sortNaturally = [](std::vector<fs::path>& files) {
            std::sort(files.begin(), files.end(), [](const fs::path& left, const fs::path& right) {
                const int comparison = StrCmpLogicalW(left.filename().c_str(), right.filename().c_str());
                return comparison == 0 ? left.wstring() < right.wstring() : comparison < 0;
            });
        };
        sortNaturally(scannedFiles);
        bool currentRenamed = false;
        if (currentFileIdentity_.valid) {
            for (const fs::path& candidate : scannedFiles) {
                if (SameFileIdentity(currentFileIdentity_, ReadFileIdentity(candidate))) {
                    if (!PathsEqual(candidate, current)) {
                        current = candidate;
                        currentPath_ = candidate.wstring();
                        filenameText_ = candidate.filename().wstring();
                        fileSizeText_ = FormatFileSize(currentPath_);
                        currentRenamed = true;
                    }
                    break;
                }
            }
        }
        std::error_code currentError;
        if (!error && !fs::exists(current, currentError)) {
            ClearDeletedImage();
            return;
        }
        if (IsTwoDimensionalMediaPath(current) && fs::exists(current, currentError) && std::none_of(scannedFiles.begin(), scannedFiles.end(),
                [&current](const fs::path& path) { return PathsEqual(path, current); })) {
            scannedFiles.push_back(current);
            sortNaturally(scannedFiles);
        }
        const bool changed = scannedFiles.size() != navigationFiles_.size() || !std::equal(scannedFiles.begin(), scannedFiles.end(), navigationFiles_.begin(),
            [](const fs::path& left, const fs::path& right) { return PathsEqual(left, right); });
        if (changed) {
            ++navigationFolderGeneration_;
            navigationFiles_ = std::move(scannedFiles);
        }
        if ((changed || currentRenamed) && imageDecodePending_) {
            ++decodeRequestGeneration_;
            pendingFullDecode_ = DecodeRequest{ currentPath_, decodeRequestGeneration_, navigationFolderGeneration_ };
            QueueLatestFullDecode();
        }
        navigationBuilt_ = true;
        InvalidateRect(window_, nullptr, FALSE);
    }

    void RefreshNavigationFromFileSystem() {
        if (source_ && !TutorialActive()) BuildNavigation(true);
    }

    void Navigate(int direction, bool immediatePaint = true) {
        if (currentPath_.empty() || ModelActive()) return;
        BuildNavigation(true);
        if (navigationFiles_.size() < 2) return;

        const fs::path current(currentPath_);
        auto currentIt = std::find_if(navigationFiles_.begin(), navigationFiles_.end(),
            [&current](const fs::path& path) { return PathsEqual(path, current); });
        const ptrdiff_t count = static_cast<ptrdiff_t>(navigationFiles_.size());
        const bool currentMissing = currentIt == navigationFiles_.end();
        const ptrdiff_t start = currentMissing ? (direction > 0 ? -1 : 0) : std::distance(navigationFiles_.begin(), currentIt);
        ptrdiff_t index = (start + direction) % count;
        if (index < 0) index += count;
        const std::wstring path = navigationFiles_[index].wstring();
        LoadContent(path, false);
        if (immediatePaint) UpdateWindow(window_);
    }

    void SetScaleAt(POINT cursor, float requestedScale) {
        if (!source_) return;
        const float oldScale = CurrentScale();
        const float baseScale = BaseScale();
        const float newScale = std::clamp(requestedScale, baseScale, kMaximumZoom);
        if (newScale <= baseScale + 0.0001f) {
            FitToWindow();
            return;
        }
        if (std::abs(newScale - oldScale) < 0.0001f) return;

        const D2D1_SIZE_F target = ImageCanvasSize();
        const D2D1_POINT_2F oldTopLeft = ImageTopLeft(oldScale, target);
        const float ratio = newScale / oldScale;
        pan_.x = static_cast<float>(cursor.x) - (static_cast<float>(cursor.x) - oldTopLeft.x) * ratio +
            imageWidth_ * newScale / 2.0f - target.width / 2.0f;
        pan_.y = static_cast<float>(cursor.y) - (static_cast<float>(cursor.y) - oldTopLeft.y) * ratio +
            imageHeight_ * newScale / 2.0f - target.height / 2.0f;
        fitToWindow_ = false;
        zoom_ = newScale;
        ClampPan();
        if (lanczosSelected_) {
            InvalidateLanczosVariant(true);
            QueueLanczosRefinement();
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void ZoomAt(POINT cursor, float factor) { SetScaleAt(cursor, CurrentScale() * factor); }
    float WheelZoomFactor(float wheelUnits) const { return std::pow(kWheelZoomStep, reverseMouseWheelZoom_ ? -wheelUnits : wheelUnits); }

    float RenderTargetDpi() const {
        if (!renderTarget_) return 96.0f;
        FLOAT dpiX = 96.0f, dpiY = 96.0f;
        renderTarget_->GetDpi(&dpiX, &dpiY);
        return dpiX;
    }

    float PhysicalPixelScale() const { return CurrentScale() * RenderTargetDpi() / 96.0f; }

    void ZoomToActualPixels(POINT cursor) { SetScaleAt(cursor, 96.0f / RenderTargetDpi()); }

    void ToggleFitActualPixels(POINT cursor) {
        if (fitToWindow_) ZoomToActualPixels(cursor);
        else FitToWindow();
    }

    void ZoomCentered(float factor) {
        const D2D1_SIZE_F client = ImageCanvasSize();
        ZoomAt({ static_cast<LONG>(client.width / 2.0f), static_cast<LONG>(client.height / 2.0f) }, factor);
    }

    void FitToWindow() {
        if (!source_) return;
        const float oldScale = CurrentScale();
        fitToWindow_ = true;
        pan_ = D2D1::Point2F();
        if (lanczosSelected_ && std::abs(CurrentScale() - oldScale) >= 0.0001f) {
            InvalidateLanczosVariant(true);
            QueueLanczosRefinement();
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    bool ImageContains(POINT point) const {
        if (!source_) return false;
        const D2D1_SIZE_F target = ImageCanvasSize();
        const float scale = CurrentScale();
        const D2D1_POINT_2F topLeft = ImageTopLeft(scale, target);
        return point.x >= topLeft.x && point.x < topLeft.x + imageWidth_ * scale &&
            point.y >= topLeft.y && point.y < topLeft.y + imageHeight_ * scale;
    }

    void BeginPan(POINT point) {
        if (!CanPan()) return;
        dragging_ = true;
        lastDragPoint_ = point;
        SetCapture(window_);
    }

    void PanTo(POINT point) {
        if (!dragging_) return;
        pan_.x += static_cast<float>(point.x - lastDragPoint_.x);
        pan_.y += static_cast<float>(point.y - lastDragPoint_.y);
        lastDragPoint_ = point;
        ClampPan();
        if (lanczosSelected_) {
            InvalidateLanczosVariant(true);
            QueueLanczosRefinement();
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void EndPan() {
        if (!dragging_) return;
        dragging_ = false;
        if (GetCapture() == window_) ReleaseCapture();
    }

    bool CanAcceptSpaceMouseInput() const {
        return spaceMouseRuntimeAvailable_ && spaceMouseEnabled_ && (source_ || ModelActive()) && !TutorialActive() &&
            !HasOverlay() && !dropdownOpen_ && !contextMenuOpen_;
    }
    navlib::matrix_t SpaceMouseCameraMatrix() const {
        if (ModelActive()) {
            return NavLibCameraToWorld(modelViewport_.NavLibCameraState());
        }
        const float scale = CurrentScale();
        return { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
            source_ ? pan_.x / scale : 0.0f, source_ ? -pan_.y / scale : 0.0f, 0, 1 };
    }
    navlib::box_t SpaceMouseViewExtents() const {
        if (ModelActive()) {
            const OrbitCamera& camera = modelViewport_.Camera();
            const double halfHeight = camera.ViewHalfHeight();
            const double halfWidth = halfHeight * camera.AspectRatio();
            const double depth = std::max<double>(camera.Distance() + camera.Radius() * 8.0f, 1.0e-5);
            return { { -halfWidth, -halfHeight, -depth }, { halfWidth, halfHeight, depth } };
        }
        const D2D1_SIZE_F canvas = ImageCanvasSize();
        const double scale = std::max(0.0001f, CurrentScale());
        const double halfWidth = canvas.width / scale / 2.0;
        const double halfHeight = canvas.height / scale / 2.0;
        return { -halfWidth, -halfHeight, -1.0, halfWidth, halfHeight, 1.0 };
    }
    void TraceSpaceMouseDiagnostic(const wchar_t* event, uint64_t generation = 0, UINT width = 0, UINT height = 0) const {
#if defined(_DEBUG)
        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        wchar_t message[448]{};
        const Float3 pivot = ModelActive() ? modelViewport_.Camera().Pivot() : Float3{};
        const OrbitCamera::State state = ModelActive() ? modelViewport_.Camera().NavLibState() : OrbitCamera::State{};
        const float distance = ModelActive() ? modelViewport_.Camera().Distance() : 0.0f;
        swprintf_s(message, L"Viewtrious SpaceMouse: %s qpc=%lld (%.3f ms) generation=%llu size=%ux%u motion=%d eye=(%.4f,%.4f,%.4f) forward=(%.4f,%.4f,%.4f) pivot=(%.4f,%.4f,%.4f) distance=%.4f\\n",
            event, now.QuadPart, 1000.0 * static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart),
            static_cast<unsigned long long>(generation), width, height, spaceMouseMotionActive_ ? 1 : 0,
            state.position.x, state.position.y, state.position.z, state.forward.x, state.forward.y, state.forward.z,
            pivot.x, pivot.y, pivot.z, distance);
        OutputDebugStringW(message);
#else
        (void)event;
        (void)generation;
        (void)width;
        (void)height;
#endif
    }
    void TraceModelSpaceMouseState(const navlib::matrix_t& input, bool accepted) {
#if defined(_DEBUG)
        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        if (now.QuadPart - lastModelNavLibTraceQpc_ < frequency.QuadPart / 10) return;
        lastModelNavLibTraceQpc_ = now.QuadPart;
        const OrbitCamera& camera = modelViewport_.Camera();
        const OrbitCamera::State state = camera.NavLibState();
        const Float3 pivot = camera.Pivot();
        const ModelBounds bounds = modelViewport_.ModelBoundsForNavLib();
        const Float3 center{ (bounds.minimum.x + bounds.maximum.x) * 0.5f, (bounds.minimum.y + bounds.maximum.y) * 0.5f, (bounds.minimum.z + bounds.maximum.z) * 0.5f };
        const OrbitCamera::ClipPlanes clips = camera.CurrentClipPlanes();
        const double halfHeight = camera.ViewHalfHeight();
        const double halfWidth = halfHeight * camera.AspectRatio();
        const Matrix4& projection = camera.ViewProjection();
        float minimumDepth = 0.0f, maximumDepth = 0.0f, minimumW = 0.0f, maximumW = 0.0f;
        unsigned visibleCorners = 0;
        bool haveCorner = false;
        for (float x : { bounds.minimum.x, bounds.maximum.x }) for (float y : { bounds.minimum.y, bounds.maximum.y }) for (float z : { bounds.minimum.z, bounds.maximum.z }) {
            const float clipX = x * projection.m[0] + y * projection.m[4] + z * projection.m[8] + projection.m[12];
            const float clipY = x * projection.m[1] + y * projection.m[5] + z * projection.m[9] + projection.m[13];
            const float clipZ = x * projection.m[2] + y * projection.m[6] + z * projection.m[10] + projection.m[14];
            const float clipW = x * projection.m[3] + y * projection.m[7] + z * projection.m[11] + projection.m[15];
            if (!haveCorner) { minimumDepth = maximumDepth = clipZ; minimumW = maximumW = clipW; haveCorner = true; }
            else { minimumDepth = std::min(minimumDepth, clipZ); maximumDepth = std::max(maximumDepth, clipZ); minimumW = std::min(minimumW, clipW); maximumW = std::max(maximumW, clipW); }
            if (clipW > 0.0f && clipX >= -clipW && clipX <= clipW && clipY >= -clipW && clipY <= clipW && clipZ >= 0.0f && clipZ <= clipW) ++visibleCorners;
        }
        wchar_t message[1152]{};
        swprintf_s(message, L"Viewtrious model NavLib accepted=%d inputEye=(%.4f,%.4f,%.4f) inputForward=(%.4f,%.4f,%.4f) eye=(%.4f,%.4f,%.4f) target/pivot=(%.4f,%.4f,%.4f) distance=%.4f fov=%.4f aspect=%.4f extents=(%.4f,%.4f) clip=(%.6f,%.4f) radius=%.4f modelCenter=(%.4f,%.4f,%.4f) vp=(%.4f,%.4f,%.4f,%.4f) boundsClip=(corners=%u z=%.4f..%.4f w=%.4f..%.4f)\\n",
            accepted ? 1 : 0, input.m30, input.m31, input.m32, -input.m20, -input.m21, -input.m22,
            state.position.x, state.position.y, state.position.z, pivot.x, pivot.y, pivot.z, camera.Distance(),
            camera.FieldOfView(), camera.AspectRatio(), halfWidth, halfHeight, clips.nearPlane, clips.farPlane, camera.Radius(),
            center.x, center.y, center.z, projection.m[0], projection.m[5], projection.m[10], projection.m[14],
            visibleCorners, minimumDepth, maximumDepth, minimumW, maximumW);
        OutputDebugStringW(message);
#else
        (void)input;
        (void)accepted;
#endif
    }
    void SetSpaceMouseCameraMatrix(const navlib::matrix_t& matrix) {
        if (!CanAcceptSpaceMouseInput()) return;
        if (ModelActive()) {
            const bool accepted = modelViewport_.SetNavLibCameraState(OrbitStateFromNavLibCameraToWorld(matrix));
            TraceModelSpaceMouseState(matrix, accepted);
            if (accepted) InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        const navlib::matrix_t current = SpaceMouseCameraMatrix();
        const double dx = matrix.m30 - current.m30, dy = matrix.m31 - current.m31;
        // NavLib supplies calibrated, time-integrated motion.  Ignore sub-millipixel camera
        // changes to suppress neutral-device noise without adding a second acceleration model.
        if (std::hypot(dx, dy) < 0.002) return;
        const float scale = CurrentScale();
        pan_.x += static_cast<float>(dx * scale);
        pan_.y -= static_cast<float>(dy * scale);
        ClampPan();
        if (lanczosSelected_) { InvalidateLanczosVariant(true); QueueLanczosRefinement(); }
        InvalidateRect(window_, nullptr, FALSE);
    }
    void SetSpaceMouseViewExtents(const navlib::box_t& extents) {
        if (!CanAcceptSpaceMouseInput()) return;
        if (ModelActive()) { if (modelViewport_.Camera().ProjectionMode() == ModelProjectionMode::Orthographic) { const double requestedHeight = extents.max.y - extents.min.y; if (requestedHeight > 0.0) {
#if defined(_DEBUG)
                const float previousExtent=modelViewport_.Camera().ViewHalfHeight();
#endif
                modelViewport_.SetOrthographicHalfHeight(static_cast<float>(requestedHeight * 0.5));
#if defined(_DEBUG)
                wchar_t message[256]{};swprintf_s(message,L"Viewtrious orthographic NavLib extent: %.6f->%.6f requested=%.6f aspect=%.6f\\n",previousExtent,modelViewport_.Camera().ViewHalfHeight(),requestedHeight,modelViewport_.Camera().AspectRatio());OutputDebugStringW(message);
#endif
                InvalidateRect(window_, nullptr, FALSE); } } return; }
        const double requestedWidth = extents.max.x - extents.min.x;
        const D2D1_SIZE_F canvas = ImageCanvasSize();
        if (requestedWidth <= 0.0 || canvas.width <= 0.0f) return;
        const float requestedScale = static_cast<float>(canvas.width / requestedWidth);
        const D2D1_SIZE_F center = ImageCanvasSize();
        SetScaleAt({ static_cast<LONG>(center.width / 2.0f), static_cast<LONG>(center.height / 2.0f) }, requestedScale);
    }
    void SetSpaceMouseMotion(bool motion) {
        spaceMouseMotionActive_ = motion;
        if (motion) CancelAnimatedModelHome();
        TraceSpaceMouseDiagnostic(motion ? L"SpaceMouse motion begin" : L"SpaceMouse motion end");
        if (motion) InvalidateLanczosVariant(true);
        if (!motion && CanAcceptSpaceMouseInput() && lanczosSelected_) QueueLanczosRefinement();
    }

    void MarkFirstPresentation() {
        if (!presented_) {
            presented_ = true;
            timer_.Log(L"first successful image presentation");
        }
        if (!currentPath_.empty() && !navigationBuilt_ && !navigationBuildQueued_) {
            navigationBuildQueued_ = true;
            PostMessageW(window_, kBuildNavigationMessage, 0, 0);
        }
    }

    void SaveWindowPlacement() const {
        if (resetInProgress_ || tutorialPlacementSuppressed_ || !rememberWindowPlacement_ || IsLikelySnappedWindow(window_)) return;
        WINDOWPLACEMENT placement{ sizeof(placement) };
        if (!GetWindowPlacement(window_, &placement)) return;
        const RECT& rect = placement.rcNormalPosition;
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
        const auto write = [key](const wchar_t* name, DWORD value) {
            RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        };
        write(L"WindowLeft", static_cast<DWORD>(rect.left));
        write(L"WindowTop", static_cast<DWORD>(rect.top));
        write(L"WindowWidth", static_cast<DWORD>(rect.right - rect.left));
        write(L"WindowHeight", static_cast<DWORD>(rect.bottom - rect.top));
        write(L"WindowMaximized", placement.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
        RegCloseKey(key);
    }

    void Shutdown() {
        shuttingDown_ = true;
        KillTimer(window_, kModelHomeAnimationTimer);
        KillTimer(window_, kTriangleCountTooltipTimer);
        StopModelLoadingAnimation();
        ++modelLoadGeneration_;
        for (ModelLoadWorker& worker : modelLoadWorkers_) if (worker.thread.joinable()) worker.thread.join();
        modelLoadWorkers_.clear();
        MSG modelLoadMessage{};
        while (PeekMessageW(&modelLoadMessage, window_, kModelLoadCompleteMessage, kModelLoadCompleteMessage, PM_REMOVE))
            delete reinterpret_cast<ModelLoadResult*>(modelLoadMessage.lParam);
        DeactivateModel();
        DeactivateVideo();
        graphicsHost_.Destroy();
        if (spaceMouse_) {
            std::error_code error;
            spaceMouse_->EnableNavigation(false, error);
            spaceMouse_.reset();
        }
        StopGifPlayback();
        StopDirectoryWatcher();
        KillTimer(window_, kShellRotationCheckTimer);
        KillTimer(window_, kHeifRotationMenuRefreshTimer);
        decodeShuttingDown_ = true;
        KillTimer(window_, kNavigationDecodeDebounceTimer);
        ++decodeRequestGeneration_;
        pendingFullDecode_.reset();
        for (std::thread& thread : fullDecodeThreads_) if (thread.joinable()) thread.join();
        fullDecodeThreads_.clear();
        ++lanczosGeneration_;
        pendingLanczosRequest_.reset();
        if (lanczosThread_.joinable()) lanczosThread_.join();
        MSG lanczosMessage{};
        while (PeekMessageW(&lanczosMessage, window_, kLanczosCompleteMessage, kLanczosCompleteMessage, PM_REMOVE))
            delete reinterpret_cast<LanczosResult*>(lanczosMessage.lParam);
    }
    void NavigationDecodeTimer() { StartPendingFullDecode(); }
    void GifPlaybackTimerMessage() { GifPlaybackTimer(); }
    void GifPlaybackVisibilityChanged(bool visible) { SetGifPlaybackVisible(visible); }
    void LanczosRefinementTimer() { RequestLanczosVariant(); }
    void ShellRotationTimer() { UpdateShellRotation(); }
    void HeifRotationMenuRefreshTimer() { RefreshHeifRotationContextMenu(); }
    void ModelLoadingAnimationTimerMessage() { UpdateModelLoadingAnimation(); }
    void FullDecodeCompleteMessage(FullDecodeResult* result) { HandleFullDecodeResult(result); }
    void LanczosCompleteMessage(LanczosResult* result) { HandleLanczosResult(result); }
    void DecodeWorkerFinishedMessage(DecodeWorkerFinished* finished) { HandleDecodeWorkerFinished(finished); }
    void DrainQueuedFullDecodeResults() {
        MSG message{};
        bool drained = false;
        while (PeekMessageW(&message, window_, kDecodeWorkerFinishedMessage, kDecodeWorkerFinishedMessage, PM_REMOVE)) {
            HandleDecodeWorkerFinished(reinterpret_cast<DecodeWorkerFinished*>(message.lParam));
            drained = true;
        }
        while (PeekMessageW(&message, window_, kFullDecodeCompleteMessage, kFullDecodeCompleteMessage, PM_REMOVE)) {
            HandleFullDecodeResult(reinterpret_cast<FullDecodeResult*>(message.lParam));
            drained = true;
        }
        if (drained) UpdateWindow(window_);
    }
    void QueueDirectoryRefreshFromWatcher() { QueueDirectoryRefresh(); }

private:
    struct OffscreenModelIndicator { bool visible=false; D2D1_POINT_2F position{}, direction{}; RECT hit{}; };
    OffscreenModelIndicator GetOffscreenModelIndicator() const {
        if (!ModelActive() || HasOverlay()) return {};
        const RECT canvas=ModelCanvasBounds(); const ModelBounds bounds=modelViewport_.ModelBoundsForNavLib(); const Matrix4& matrix=modelViewport_.Camera().ViewProjection();
        const auto project=[&](Float3 p,float& x,float& y,float& w){x=p.x*matrix.m[0]+p.y*matrix.m[4]+p.z*matrix.m[8]+matrix.m[12];y=p.x*matrix.m[1]+p.y*matrix.m[5]+p.z*matrix.m[9]+matrix.m[13];w=p.x*matrix.m[3]+p.y*matrix.m[7]+p.z*matrix.m[11]+matrix.m[15];};
        float minX=FLT_MAX,minY=FLT_MAX,maxX=-FLT_MAX,maxY=-FLT_MAX; bool front=false,behind=false;
        for(int i=0;i<8;++i){Float3 p{(i&1)?bounds.maximum.x:bounds.minimum.x,(i&2)?bounds.maximum.y:bounds.minimum.y,(i&4)?bounds.maximum.z:bounds.minimum.z};float x,y,w;project(p,x,y,w);if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(w)||w<=1e-5f){behind=true;continue;}front=true;x/=w;y/=w;minX=std::min(minX,x);maxX=std::max(maxX,x);minY=std::min(minY,y);maxY=std::max(maxY,y);}
        if(front&&behind)return {};
        if(front&&!behind&&minX<=1&&maxX>=-1&&minY<=1&&maxY>=-1)return {};
        const Float3 center{(bounds.minimum.x+bounds.maximum.x)*.5f,(bounds.minimum.y+bounds.maximum.y)*.5f,(bounds.minimum.z+bounds.maximum.z)*.5f};float cx,cy,cw;project(center,cx,cy,cw);float dx=0,dy=0;
        if(front&&!behind&&std::isfinite(cw)&&cw>1e-5f){dx=cx/cw;dy=-cy/cw;}else{const OrbitCamera::State state=modelViewport_.Camera().NavLibState();const auto dot=[](Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};const auto cross=[](Float3 a,Float3 b){return Float3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};Float3 forward=state.forward;const float fl=std::sqrt(dot(forward,forward));if(fl<1e-5f)return {};forward={forward.x/fl,forward.y/fl,forward.z/fl};Float3 right=cross(forward,state.up);const float rl=std::sqrt(dot(right,right));if(rl<1e-5f)return {};right={right.x/rl,right.y/rl,right.z/rl};const Float3 up=cross(right,forward),delta{center.x-modelViewport_.Camera().CameraTarget().x,center.y-modelViewport_.Camera().CameraTarget().y,center.z-modelViewport_.Camera().CameraTarget().z};dx=dot(delta,right);dy=-dot(delta,up);}
        const float length=std::sqrt(dx*dx+dy*dy);if(!std::isfinite(length)||length<1e-5f)return {};dx/=length;dy/=length;
        const float dpi=GetDpiForWindow(window_)/96.f,inset=18*dpi,halfW=std::max(1.f,(canvas.right-canvas.left)*.5f-inset),halfH=std::max(1.f,(canvas.bottom-canvas.top)*.5f-inset),t=std::min(halfW/std::max(std::fabs(dx),1e-5f),halfH/std::max(std::fabs(dy),1e-5f));
        const float mx=(canvas.left+canvas.right)*.5f,my=(canvas.top+canvas.bottom)*.5f;D2D1_POINT_2F point=D2D1::Point2F(mx+dx*t,my+dy*t);
        const float compassRadius=46*dpi,compassMargin=12*dpi,compassLegendHeight=16*dpi,compassX=canvas.right-compassMargin-compassRadius,compassY=canvas.bottom-compassMargin-compassLegendHeight-compassRadius,safeX=compassX-compassRadius-inset,safeY=compassY-compassRadius-inset;
        if(dx>0&&dy>0&&std::hypot(point.x-compassX,point.y-compassY)<compassRadius+18*dpi){if(std::fabs(point.y-safeY)<std::fabs(point.x-safeX))point.y=std::min(point.y,safeY);else point.x=std::min(point.x,safeX);}
        const int hit=MulDiv(56,GetDpiForWindow(window_),96);return {true,point,D2D1::Point2F(dx,dy),{(LONG)point.x-hit,(LONG)point.y-hit,(LONG)point.x+hit,(LONG)point.y+hit}};
    }
    bool OffscreenModelIndicatorContains(POINT point) const { const OffscreenModelIndicator indicator=GetOffscreenModelIndicator();return indicator.visible&&PtInRect(&indicator.hit,point); }
    void TraceOffscreenModelIndicatorState() {
#if defined(_DEBUG)
        const OffscreenModelIndicator indicator=GetOffscreenModelIndicator();
        if(!indicator.visible){if(offscreenIndicatorWasVisible_)OutputDebugStringW(L"Viewtrious offscreen indicator: visible=0\n");offscreenIndicatorWasVisible_=false;offscreenIndicatorSector_=-1;return;}
        const int sector=static_cast<int>(std::floor((std::atan2(indicator.direction.y,indicator.direction.x)+3.14159265f)*4.f/3.14159265f))%8;
        if(!offscreenIndicatorWasVisible_||sector!=offscreenIndicatorSector_){wchar_t message[256]{};swprintf_s(message,L"Viewtrious offscreen indicator: visible=1 sector=%d direction=(%.3f,%.3f) position=(%.1f,%.1f)\n",sector,indicator.direction.x,indicator.direction.y,indicator.position.x,indicator.position.y);OutputDebugStringW(message);offscreenIndicatorSector_=sector;}
        offscreenIndicatorWasVisible_=true;
#endif
    }
    void DrawOffscreenModelIndicator() { const OffscreenModelIndicator indicator=GetOffscreenModelIndicator(); if(!indicator.visible)return;const float dpi=GetDpiForWindow(window_)/96.f;ComPtr<ID2D1SolidColorBrush> brush;if(FAILED(renderTarget_->CreateSolidColorBrush(hoveredButton_==ButtonKind::ModelOffscreenIndicator?D2D1::ColorF(0.f/255,120.f/255,212.f/255):D2D1::ColorF(.9f,.92f,.96f,.9f),&brush)))return;const D2D1_POINT_2F perp=D2D1::Point2F(-indicator.direction.y,indicator.direction.x),tip=D2D1::Point2F(indicator.position.x+indicator.direction.x*20*dpi,indicator.position.y+indicator.direction.y*20*dpi),left=D2D1::Point2F(indicator.position.x-indicator.direction.x*16*dpi+perp.x*14*dpi,indicator.position.y-indicator.direction.y*16*dpi+perp.y*14*dpi),right=D2D1::Point2F(indicator.position.x-indicator.direction.x*16*dpi-perp.x*14*dpi,indicator.position.y-indicator.direction.y*16*dpi-perp.y*14*dpi);ComPtr<ID2D1PathGeometry> geometry;ComPtr<ID2D1GeometrySink> sink;if(SUCCEEDED(d2dFactory_->CreatePathGeometry(&geometry))&&SUCCEEDED(geometry->Open(&sink))){sink->BeginFigure(tip,D2D1_FIGURE_BEGIN_FILLED);sink->AddLine(left);sink->AddLine(right);sink->EndFigure(D2D1_FIGURE_END_CLOSED);sink->Close();renderTarget_->FillGeometry(geometry.Get(),brush.Get());}
#if defined(_DEBUG)
        const int sector=static_cast<int>(std::floor((std::atan2(indicator.direction.y,indicator.direction.x)+3.14159265f)*4.f/3.14159265f))%8;if(!offscreenIndicatorWasVisible_||sector!=offscreenIndicatorSector_){wchar_t message[256]{};swprintf_s(message,L"Viewtrious offscreen indicator: visible=1 sector=%d direction=(%.3f,%.3f) position=(%.1f,%.1f)\\n",sector,indicator.direction.x,indicator.direction.y,indicator.position.x,indicator.position.y);OutputDebugStringW(message);offscreenIndicatorSector_=sector;}offscreenIndicatorWasVisible_=true;
#endif
    }
    void DrawModelLoadingOverlay() {
        if (!ModelLoadingOverlayVisible()) return;
        const RECT canvas = ModelCanvasBounds();
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        const float width = std::min(380.0f * scale, std::max(220.0f * scale, float(canvas.right - canvas.left) - 32.0f * scale));
        const float height = 156.0f * scale;
        const float left = (canvas.left + canvas.right - width) * 0.5f;
        const float top = (canvas.top + canvas.bottom - height) * 0.5f;
        const bool dark = UseDarkAppMode();
        ComPtr<ID2D1SolidColorBrush> panel, border, primary, secondary, spinner;
        if (FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(41.f / 255, 44.f / 255, 52.f / 255, .97f) : D2D1::ColorF(250.f / 255, 250.f / 255, 250.f / 255, .97f), &panel)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(83.f / 255, 88.f / 255, 102.f / 255) : D2D1::ColorF(190.f / 255, 190.f / 255, 190.f / 255), &border)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(.95f, .96f, .98f) : D2D1::ColorF(.12f, .12f, .12f), &primary)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(.70f, .73f, .79f) : D2D1::ColorF(.36f, .36f, .36f), &secondary)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f / 255, 120.f / 255, 212.f / 255), &spinner))) return;
        const D2D1_ROUNDED_RECT bounds = D2D1::RoundedRect(D2D1::RectF(left, top, left + width, top + height), 8.0f * scale, 8.0f * scale);
        renderTarget_->FillRoundedRectangle(bounds, panel.Get());
        renderTarget_->DrawRoundedRectangle(bounds, border.Get(), 1.0f);
        DrawOverlayText(L"Opening model...", left + 20.0f * scale, top + 20.0f * scale, width - 40.0f * scale, 28.0f * scale, 18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primary.Get(), true);
        DrawOverlayText(filenameText_.c_str(), left + 20.0f * scale, top + 51.0f * scale, width - 40.0f * scale, 24.0f * scale, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, secondary.Get(), true);
        const D2D1_POINT_2F center = D2D1::Point2F(left + width * .5f, top + 113.0f * scale);
        const unsigned phase = static_cast<unsigned>((GetTickCount64() - modelLoadingStartedAtMs_) / 80) % 12;
        for (unsigned index = 0; index < 12; ++index) {
            const float angle = (static_cast<float>(index) / 12.0f) * 6.2831853f - 1.5707963f;
            const unsigned distance = (index + 12 - phase) % 12;
            const float opacity = .20f + .80f * (1.0f - static_cast<float>(distance) / 12.0f);
            spinner->SetOpacity(opacity);
            renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x + std::cos(angle) * 16.0f * scale, center.y + std::sin(angle) * 16.0f * scale), 3.0f * scale, 3.0f * scale), spinner.Get());
        }
        spinner->SetOpacity(1.0f);
    }
    void DrawModelAxisIndicator() {
        const RECT canvas = ModelCanvasBounds(); const float dpi = GetDpiForWindow(window_) / 96.0f;
        const float radius = 46.0f * dpi, margin = 12.0f * dpi, legendHeight = 16.0f * dpi;
        const float horizontal = radius + margin;
        const float vertical = radius + margin + legendHeight;
        const D2D1_POINT_2F origin = D2D1::Point2F(
            axisIndicatorPosition_ == AxisIndicatorPosition::BottomLeft || axisIndicatorPosition_ == AxisIndicatorPosition::TopLeft ? canvas.left + horizontal : canvas.right - horizontal,
            axisIndicatorPosition_ == AxisIndicatorPosition::TopLeft || axisIndicatorPosition_ == AxisIndicatorPosition::TopRight ? canvas.top + vertical : canvas.bottom - vertical);
        const OrbitCamera::State state = modelViewport_.Camera().NavLibState();
        const auto dot=[](Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}; const auto cross=[](Float3 a,Float3 b){return Float3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
        const float forwardLength=std::sqrt(dot(state.forward,state.forward)); if(forwardLength<1e-5f)return; const Float3 forward{state.forward.x/forwardLength,state.forward.y/forwardLength,state.forward.z/forwardLength}; Float3 right=cross(forward,state.up); const float rightLength=std::sqrt(dot(right,right)); if(rightLength<1e-5f)return; right={right.x/rightLength,right.y/rightLength,right.z/rightLength}; const Float3 up=cross(right,forward);
        ComPtr<ID2D1SolidColorBrush> x,y,z,ring,junction; if(FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.88f,.30f,.30f),&x))||FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.35f,.78f,.42f),&y))||FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.35f,.55f,.95f),&z))||FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.90f,.92f,.96f,.26f),&ring))||FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.90f,.92f,.96f,.9f),&junction)))return;
        renderTarget_->DrawEllipse(D2D1::Ellipse(origin,radius,radius),ring.Get(),1.0f*dpi);
        renderTarget_->FillEllipse(D2D1::Ellipse(origin,3.5f*dpi,3.5f*dpi),junction.Get());
        struct AxisPresentation { Float3 world; ID2D1Brush* brush; bool depthAxis; bool towardViewer; };
        std::array<AxisPresentation, 3> axes{{
            {{1,0,0}, x.Get(), false, false}, {{0,1,0}, y.Get(), false, false}, {{0,0,1}, z.Get(), false, false}
        }};
        constexpr float kDepthAxisThreshold = .94f;
        for (AxisPresentation& axis : axes) {
            const float screenX=dot(axis.world,right), screenY=-dot(axis.world,up), viewAlignment=dot(axis.world,forward), depth=-viewAlignment;
            axis.depthAxis = std::fabs(viewAlignment) >= kDepthAxisThreshold; axis.towardViewer = viewAlignment < 0.0f;
            if (axis.depthAxis) continue;
            const D2D1_POINT_2F end=D2D1::Point2F(origin.x+screenX*(radius-24*dpi),origin.y+screenY*(radius-24*dpi)); const float marker=std::clamp(2.9f+1.6f*(depth+1.f)*.5f,2.9f,4.5f)*dpi;
            renderTarget_->DrawLine(origin,end,axis.brush,2.4f*dpi); renderTarget_->FillEllipse(D2D1::Ellipse(end,marker,marker),axis.brush);
        }
        for (const AxisPresentation& axis : axes) if (axis.depthAxis) {
            const float markerRadius=5.5f*dpi, arm=2.5f*dpi; renderTarget_->DrawEllipse(D2D1::Ellipse(origin,markerRadius,markerRadius),axis.brush,1.8f*dpi);
            if (axis.towardViewer) renderTarget_->FillEllipse(D2D1::Ellipse(origin,1.8f*dpi,1.8f*dpi),axis.brush);
            else { renderTarget_->DrawLine(D2D1::Point2F(origin.x-arm,origin.y-arm),D2D1::Point2F(origin.x+arm,origin.y+arm),axis.brush,1.6f*dpi); renderTarget_->DrawLine(D2D1::Point2F(origin.x-arm,origin.y+arm),D2D1::Point2F(origin.x+arm,origin.y-arm),axis.brush,1.6f*dpi); }
        }
        const bool topPosition = axisIndicatorPosition_ == AxisIndicatorPosition::TopLeft || axisIndicatorPosition_ == AxisIndicatorPosition::TopRight;
        const float legendTop = topPosition ? origin.y - radius - 1.0f * dpi - 15.0f * dpi : origin.y + radius + 1.0f * dpi;
        DrawOverlayText(L"X",origin.x-24*dpi,legendTop,16*dpi,15*dpi,12,DWRITE_FONT_WEIGHT_SEMI_BOLD,x.Get(),true,false,true);
        DrawOverlayText(L"Y",origin.x-8*dpi,legendTop,16*dpi,15*dpi,12,DWRITE_FONT_WEIGHT_SEMI_BOLD,y.Get(),true,false,true);
        DrawOverlayText(L"Z",origin.x+8*dpi,legendTop,16*dpi,15*dpi,12,DWRITE_FONT_WEIGHT_SEMI_BOLD,z.Get(),true,false,true);
    }
    RECT ModelCanvasBounds() const {
        RECT client{}; GetClientRect(window_, &client);
        const LONG top = fullscreen_ ? 0 : GetFrameMetrics(window_).titleBarHeight;
        return { 0, top, client.right, std::max(top + 1L, client.bottom) };
    }
    void ClearModelFaceSelection() { modelFaceSelected_=false; selectedFaceNormal_={}; selectedFaceHit_={}; selectedFacePlane_=-1; modelViewport_.ClearSelectedSnapPlane(); }
    void TraceSnapView(int plane, Float3 normal, Float3 hitPoint, const OrbitCamera::State& current, Float3 forward, Float3 up, Float3 chosenAxis) const {
#if defined(_DEBUG)
        const auto dot=[](Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};const Float3 right{forward.y*up.z-forward.z*up.y,forward.z*up.x-forward.x*up.z,forward.x*up.y-forward.y*up.x};wchar_t message[640]{};swprintf_s(message,L"Viewtrious Snap request: plane=%d normal=(%.5f,%.5f,%.5f) hit=(%.5f,%.5f,%.5f) currentForward=(%.5f,%.5f,%.5f) currentUp=(%.5f,%.5f,%.5f) targetForward=(%.5f,%.5f,%.5f) targetUp=(%.5f,%.5f,%.5f) targetRight=(%.5f,%.5f,%.5f) axis=(%.0f,%.0f,%.0f) distance=%.5f normalDot=%.6f basisDots=(%.6f,%.6f,%.6f)\\n",plane,normal.x,normal.y,normal.z,hitPoint.x,hitPoint.y,hitPoint.z,current.forward.x,current.forward.y,current.forward.z,current.up.x,current.up.y,current.up.z,forward.x,forward.y,forward.z,up.x,up.y,up.z,right.x,right.y,right.z,chosenAxis.x,chosenAxis.y,chosenAxis.z,modelViewport_.Camera().Distance(),dot(forward,normal),dot(forward,right),dot(forward,up),dot(right,up));OutputDebugStringW(message);
#else
        (void)plane;(void)normal;(void)hitPoint;(void)current;(void)forward;(void)up;(void)chosenAxis;
#endif
    }
    void TraceModelPick(POINT point, const D3D11_VIEWPORT& viewport, float localX, float localY, float ndcX, float ndcY, Float3 origin, Float3 direction, ptrdiff_t triangle, int plane) const {
#if defined(_DEBUG)
        RECT client{}; GetClientRect(window_, &client); const D3D11_VIEWPORT renderViewport=modelViewport_.RenderViewport(); wchar_t message[800]{};
        swprintf_s(message, L"Viewtrious model pick: client=(%ld,%ld) dpi=%.3f clientSize=%ldx%ld logicalViewport=(%.1f,%.1f %.1fx%.1f) renderViewport=(%.1f,%.1f %.1fx%.1f) local=(%.2f,%.2f) ndc=(%.4f,%.4f) rayOrigin=(%.4f,%.4f,%.4f) rayDirection=(%.4f,%.4f,%.4f) triangle=%td plane=%d\\n", point.x, point.y, GetDpiForWindow(window_) / 96.0f, client.right-client.left, client.bottom-client.top, viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height, renderViewport.TopLeftX, renderViewport.TopLeftY, renderViewport.Width, renderViewport.Height, localX, localY, ndcX, ndcY, origin.x, origin.y, origin.z, direction.x, direction.y, direction.z, triangle, plane); OutputDebugStringW(message);
#else
        (void)point; (void)viewport; (void)localX; (void)localY; (void)ndcX; (void)ndcY; (void)origin; (void)direction; (void)triangle; (void)plane;
#endif
    }
    navlib::point_t SpaceMouseModelPivot() const {
        if (!ModelActive()) return {};
        const Float3 pivot = modelViewport_.Camera().Pivot();
        return { pivot.x, pivot.y, pivot.z };
    }
    navlib::point_t SpaceMouseModelCameraTarget() const {
        if (!ModelActive()) return {};
        const Float3 target = modelViewport_.Camera().CameraTarget();
        return { target.x, target.y, target.z };
    }
    static navlib::matrix_t NavLibCameraToWorld(const OrbitCamera::State& state) {
        // NavLib consumes a right-handed, row-major camera-to-world matrix.  The camera's
        // local forward axis is -Z, so row 2 is the negated view forward vector.
        const Float3 right{ state.forward.y * state.up.z - state.forward.z * state.up.y,
            state.forward.z * state.up.x - state.forward.x * state.up.z,
            state.forward.x * state.up.y - state.forward.y * state.up.x };
        return { right.x, right.y, right.z, 0, state.up.x, state.up.y, state.up.z, 0,
            -state.forward.x, -state.forward.y, -state.forward.z, 0,
            state.position.x, state.position.y, state.position.z, 1 };
    }
    static OrbitCamera::State OrbitStateFromNavLibCameraToWorld(const navlib::matrix_t& matrix) {
        // This is the inverse of NavLibCameraToWorld: translation is the eye, row 1 is up,
        // and negated row 2 is the world-space view forward vector.
        return {
            { static_cast<float>(matrix.m30), static_cast<float>(matrix.m31), static_cast<float>(matrix.m32) },
            { static_cast<float>(-matrix.m20), static_cast<float>(-matrix.m21), static_cast<float>(-matrix.m22) },
            { static_cast<float>(matrix.m10), static_cast<float>(matrix.m11), static_cast<float>(matrix.m12) },
        };
    }
    navlib::box_t SpaceMouseModelExtents() const {
        if (!ModelActive()) return {};
        const ModelBounds bounds = modelViewport_.ModelBoundsForNavLib();
        return { { bounds.minimum.x, bounds.minimum.y, bounds.minimum.z }, { bounds.maximum.x, bounds.maximum.y, bounds.maximum.z } };
    }
    void SetSpaceMouseModelPivot(const navlib::point_t& point) {
        // Late NavLib pivot echoes must not create a state that differs from the rendered camera.
        (void)point;
    }
    void SetSpaceMouseModelCameraTarget(const navlib::point_t& point) {
        if (!spaceMouseMotionActive_ || !ModelActive()) return;
        if (modelViewport_.SetNavLibCameraTarget({ static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z) })) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
    void ReapModelLoadWorker(uint64_t generation) {
        const auto worker = std::find_if(modelLoadWorkers_.begin(), modelLoadWorkers_.end(), [generation](const ModelLoadWorker& entry) {
            return entry.generation == generation;
        });
        if (worker == modelLoadWorkers_.end()) return;
        if (worker->thread.joinable()) worker->thread.join();
        modelLoadWorkers_.erase(worker);
    }
    void StopModelLoadingAnimation() { KillTimer(window_, kModelLoadingAnimationTimer); }
    bool ModelLoadingOverlayVisible() const {
        return modelLoading_ && GetTickCount64() - modelLoadingStartedAtMs_ >= kModelLoadingOverlayDelayMs;
    }
    void UpdateModelLoadingAnimation() {
        if (!modelLoading_) { StopModelLoadingAnimation(); return; }
        if (ModelLoadingOverlayVisible()) InvalidateRect(window_, nullptr, FALSE);
    }
    void BeginModelLoad(const std::wstring& path) {
        DeactivateVideo(); DeactivateModel(); StopGifPlayback(); StopDirectoryWatcher(); InvalidateLanczosVariant(false);
        ++decodeRequestGeneration_; ++modelLoadGeneration_; const uint64_t generation = modelLoadGeneration_;
        currentPath_ = path; displayedPath_.clear(); source_.Reset(); bitmap_.Reset(); displayedPixels_.reset(); imageWidth_ = imageHeight_ = 0;
        filenameText_ = fs::path(path).filename().wstring(); fileSizeText_ = FormatFileSize(path); resolutionText_ = L"3D"; error_.clear();
        navigationFiles_.clear(); navigationBuilt_ = false; modelLoading_ = true; modelLoadingStartedAtMs_ = GetTickCount64(); contentKind_ = ContentKind::Model3D;
        ClearModelFaceSelection(); modelClickCandidate_ = false;
        SetTimer(window_, kModelLoadingAnimationTimer, 16, nullptr);
        const HWND window = window_;
        modelLoadWorkers_.push_back({ generation, std::thread([path, generation, window, shuttingDown = &shuttingDown_] {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ModelLoadResult loaded{ path, generation };
            if (SUCCEEDED(com) || com == RPC_E_CHANGED_MODE) {
                if (IsThreeMfPath(path)) {
                    ThreeMfLoadResult result = LoadThreeMfDocument(path);
                    loaded.document = std::move(result.document); loaded.error = std::move(result.error);
                } else if (IsStepPath(path)) {
                    loaded.document = LoadStepDocumentFromAddon(path, loaded.error);
                } else {
                    StlLoadResult result = LoadStlDocument(path);
                    loaded.document = std::move(result.document); loaded.error = std::move(result.error);
                }
            } else {
                loaded.error = L"Viewtrious could not initialize the model loading worker.";
            }
            if (SUCCEEDED(com)) CoUninitialize();
            auto* result = new ModelLoadResult(std::move(loaded));
            if (shuttingDown->load() || !PostMessageW(window, kModelLoadCompleteMessage, 0, reinterpret_cast<LPARAM>(result))) delete result;
        }) });
        InvalidateRect(window_, nullptr, FALSE);
    }
    void DeactivateModel() {
        CancelAnimatedModelHome();
        StopModelLoadingAnimation(); ClearModelFaceSelection(); modelClickCandidate_ = false;
        modelViewport_.Destroy(); modelDocument_.reset(); modelLoading_ = false;
        modelTriangleCount_ = 0;
        DismissTriangleCountTooltip(false);
        if (contentKind_ == ContentKind::Model3D) contentKind_ = ContentKind::None;
    }
    void BeginVideoLoad(const std::wstring& path) {
        VideoPlayer::Trace(window_, L"Viewer entering Video2D open");
        DeactivateModel(); DeactivateVideo(); StopGifPlayback(); StopDirectoryWatcher(); InvalidateLanczosVariant(false);
        ++decodeRequestGeneration_; ++modelLoadGeneration_; pendingFullDecode_.reset(); imageDecodePending_ = false;
        source_.Reset(); bitmap_.Reset(); displayedPixels_.reset(); imageWidth_ = imageHeight_ = 0;
        currentPath_ = path; displayedPath_.clear(); filenameText_ = fs::path(path).filename().wstring();
        currentFileIdentity_ = ReadFileIdentity(fs::path(path));
        fileSizeText_ = FormatFileSize(path); resolutionText_.clear(); videoFramesPerSecondText_.clear(); error_.clear();
        navigationFiles_.clear(); navigationBuilt_ = false; navigationBuildQueued_ = false; contentKind_ = ContentKind::Video2D;
        ResetVideoControls();
        EnsureRenderTarget();
        std::wstring videoError;
        if (!graphicsHost_.Ready() || !videoPlayer_.Open(window_, graphicsHost_.Device(), path, videoError)) {
            contentKind_ = ContentKind::None;
            error_ = videoError.empty() ? L"Viewtrious could not open this MP4." : videoError;
        }
        VideoPlayer::Trace(window_, L"Video2D render invalidation after open");
        InvalidateRect(window_, nullptr, FALSE);
    }
    void DeactivateVideo() {
        VideoPlayer::Trace(window_, L"Viewer Video2D teardown");
        KillTimer(window_, kVideoPlaybackTimer);
        StopVideoControls();
        videoPlayer_.Shutdown();
        videoFramesPerSecondText_.clear();
        if (contentKind_ == ContentKind::Video2D) { resolutionText_.clear(); contentKind_ = ContentKind::None; }
    }
public:
    void VideoMediaEngineEvent(DWORD event) {
        if (!VideoActive()) return;
        VideoPlayer::Trace(window_, L"Video2D event received by UI", S_OK, event);
        std::wstring videoError;
        videoPlayer_.HandleMediaEvent(event, videoError);
        UpdateVideoTitleMetadata();
        if (!videoError.empty()) error_ = videoError;
        if (videoPlayer_.Failed()) { DeactivateVideo(); VideoPlayer::Trace(window_, L"Video2D render invalidation after failure", S_OK, event); InvalidateRect(window_, nullptr, FALSE); return; }
        if (videoPlayer_.Playing()) SetTimer(window_, kVideoPlaybackTimer, 16, nullptr);
        else KillTimer(window_, kVideoPlaybackTimer);
        if (event == MF_MEDIA_ENGINE_EVENT_ENDED || event == MF_MEDIA_ENGINE_EVENT_CANPLAY || event == MF_MEDIA_ENGINE_EVENT_PLAYING) ShowVideoControls();
        VideoPlayer::Trace(window_, L"Video2D render invalidation after event", S_OK, event);
        InvalidateRect(window_, nullptr, FALSE);
    }
    void UpdateVideoTitleMetadata() {
        if (!VideoActive()) return;
        DWORD width = 0, height = 0;
        if (videoPlayer_.GetNativeVideoSize(width, height)) resolutionText_ = std::to_wstring(width) + L" x " + std::to_wstring(height);
        float framesPerSecond = 0.0f;
        if (videoPlayer_.TryGetFramesPerSecond(framesPerSecond)) {
            videoFramesPerSecondText_ = FormatFramesPerSecond(framesPerSecond);
            if (!resolutionText_.empty()) resolutionText_ += L"  \x2022  " + videoFramesPerSecondText_;
        }
    }
    void VideoPlaybackTimerMessage() {
        if (!VideoActive() || !videoPlayer_.Playing()) { KillTimer(window_, kVideoPlaybackTimer); return; }
        VideoPlayer::Trace(window_, L"Video2D playback timer tick");
        VideoPlayer::Trace(window_, L"Video2D render invalidation from timer");
        InvalidateRect(window_, nullptr, FALSE);
    }
private:
    void StopDirectoryWatcher() {
        directoryWatcherStopping_ = true;
        if (directoryWatcherHandle_ != INVALID_HANDLE_VALUE) CancelIoEx(directoryWatcherHandle_, nullptr);
        if (directoryWatcherThread_.joinable()) {
            CancelSynchronousIo(directoryWatcherThread_.native_handle());
            directoryWatcherThread_.join();
        }
        if (directoryWatcherHandle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(directoryWatcherHandle_);
            directoryWatcherHandle_ = INVALID_HANDLE_VALUE;
        }
        directoryWatcherFolder_.clear();
        KillTimer(window_, kDirectoryChangeDebounceTimer);
    }

    void DirectoryWatcherLoop() {
        std::array<BYTE, 4096> buffer{};
        while (!directoryWatcherStopping_) {
            DWORD bytes = 0;
            const BOOL changed = ReadDirectoryChangesW(directoryWatcherHandle_, buffer.data(), static_cast<DWORD>(buffer.size()), FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE, &bytes, nullptr, nullptr);
            if (!changed) break;
            if (bytes != 0 && !directoryWatcherStopping_) PostMessageW(window_, kDirectoryChangedMessage, 0, 0);
        }
    }

    void StartDirectoryWatcher(const fs::path& folder) {
        const std::wstring normalized = folder.lexically_normal().wstring();
        if (PathsEqual(fs::path(directoryWatcherFolder_), folder)) return;
        StopDirectoryWatcher();
        const HANDLE handle = CreateFileW(folder.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return;
        directoryWatcherStopping_ = false;
        directoryWatcherFolder_ = normalized;
        directoryWatcherHandle_ = handle;
        directoryWatcherThread_ = std::thread([this] { DirectoryWatcherLoop(); });
    }

    void QueueDirectoryRefresh() {
        if (!source_ || TutorialActive()) return;
        SetTimer(window_, kDirectoryChangeDebounceTimer, 150, nullptr);
    }

    void RegisterDefaultAppCapabilities() {
        wchar_t modulePath[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath))) return;
        const std::wstring executable(modulePath);
        const std::wstring command = L"\"" + executable + L"\" \"%1\"";
        const struct Association { const wchar_t* extension; const wchar_t* progId; const wchar_t* description; } associations[] = {
            { L".jpg", L"Viewtrious.jpg", L"Viewtrious JPG Image" },
            { L".jpeg", L"Viewtrious.jpeg", L"Viewtrious JPEG Image" },
            { L".png", L"Viewtrious.png", L"Viewtrious PNG Image" },
            { L".bmp", L"Viewtrious.bmp", L"Viewtrious BMP Image" },
            { L".gif", L"Viewtrious.gif", L"Viewtrious GIF Image" },
            { L".heic", L"Viewtrious.heic", L"Viewtrious HEIC Image" },
            { L".heif", L"Viewtrious.heif", L"Viewtrious HEIF Image" },
            { L".dng", L"Viewtrious.dng", L"Viewtrious DNG Image" },
            { L".stl", L"Viewtrious.stl", L"Viewtrious STL Model" },
            { L".3mf", L"Viewtrious.3mf", L"Viewtrious 3MF Model" },
        };
        if (StepAddonPresent()) {
            const Association stepAssociations[] = { { L".step", L"Viewtrious.step", L"Viewtrious STEP Model" }, { L".stp", L"Viewtrious.stp", L"Viewtrious STP Model" } };
            for (const Association& association : stepAssociations) {
                const std::wstring progIdPath = std::wstring(L"Software\\Classes\\") + association.progId;
                WriteRegistryString(HKEY_CURRENT_USER, progIdPath.c_str(), L"", association.description); WriteRegistryString(HKEY_CURRENT_USER, (progIdPath + L"\\DefaultIcon").c_str(), L"", executable + L",0"); WriteRegistryString(HKEY_CURRENT_USER, (progIdPath + L"\\shell\\open\\command").c_str(), L"", command); WriteRegistryString(HKEY_CURRENT_USER, (std::wstring(kCapabilitiesPath) + L"\\FileAssociations").c_str(), association.extension, association.progId);
            }
        }
        for (const Association& association : associations) {
            const std::wstring progIdPath = std::wstring(L"Software\\Classes\\") + association.progId;
            WriteRegistryString(HKEY_CURRENT_USER, progIdPath.c_str(), L"", association.description);
            WriteRegistryString(HKEY_CURRENT_USER, (progIdPath + L"\\DefaultIcon").c_str(), L"", executable + L",0");
            WriteRegistryString(HKEY_CURRENT_USER, (progIdPath + L"\\shell\\open\\command").c_str(), L"", command);
            WriteRegistryString(HKEY_CURRENT_USER, (std::wstring(kCapabilitiesPath) + L"\\FileAssociations").c_str(), association.extension, association.progId);
        }
        WriteRegistryString(HKEY_CURRENT_USER, kCapabilitiesPath, L"ApplicationName", kRegisteredApplicationName);
        WriteRegistryString(HKEY_CURRENT_USER, kCapabilitiesPath, L"ApplicationDescription", L"Viewtrious image viewer");
        WriteRegistryString(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", kRegisteredApplicationName, kCapabilitiesPath);
    }

    void OpenRegisteredDefaultApps() {
        INT_PTR result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open",
            L"ms-settings:defaultapps?registeredAppUser=Viewtrious", nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) ShowActionError(L"Windows could not open Default Apps settings.");
    }

    void ToggleOpenWithSubmenu() {
        if (openWithSubmenuOpen_) {
            DismissOpenWithSubmenu();
            return;
        }
        const std::wstring extension = fs::path(currentPath_).extension().wstring();
        if (openWithExtension_ != extension) {
            openWithHandlers_.clear(); openWithExtension_ = extension;
            ComPtr<IEnumAssocHandlers> enumerator;
            if (SUCCEEDED(SHAssocEnumHandlers(extension.c_str(), ASSOC_FILTER_RECOMMENDED, &enumerator))) for (;;) {
                ComPtr<IAssocHandler> handler; ULONG fetched = 0;
                if (enumerator->Next(1, &handler, &fetched) != S_OK || !handler) break;
                LPWSTR name = nullptr;
                if (SUCCEEDED(handler->GetUIName(&name)) && name && *name) {
                    const std::wstring uiName(name); CoTaskMemFree(name);
                    if (std::none_of(openWithHandlers_.begin(), openWithHandlers_.end(), [&](const OpenWithHandler& value) { return value.name == uiName; })) openWithHandlers_.push_back({ uiName, handler });
                } else if (name) CoTaskMemFree(name);
            }
        }
        openWithSubmenuOpen_ = true; openWithHovered_ = -1; InvalidateRect(window_, nullptr, FALSE);
    }

    RECT GetOpenWithSubmenuBounds() const {
        const RECT parent = GetContextMenuBounds(); RECT client{}; GetClientRect(window_, &client);
        const UINT dpi = GetDpiForWindow(window_); const LONG margin = MulDiv(4, dpi, 96), row = MulDiv(38, dpi, 96), gap = MulDiv(9, dpi, 96);
        const LONG width = std::min<LONG>(MulDiv(250, dpi, 96), std::max<LONG>(1, client.right - margin * 2));
        const LONG height = margin * 2 + row * (static_cast<LONG>(openWithHandlers_.size()) + 1) + gap;
        const LONG rightX = parent.right + margin; const LONG left = rightX + width <= client.right - margin ? rightX : std::max<LONG>(margin, parent.left - margin - width);
        const LONG top = std::clamp<LONG>(parent.top + MulDiv(kContextMenuPaddingDip, dpi, 96) + row * 3 + gap * 2, margin, std::max<LONG>(margin, client.bottom - height - margin));
        return { left, top, left + width, top + height };
    }

    RECT GetContextMenuBounds() const {
        RECT client{};
        GetClientRect(window_, &client);
        const UINT dpi = GetDpiForWindow(window_);
        const LONG margin = MulDiv(4, dpi, 96);
        const LONG width = std::min<LONG>(MulDiv(260, dpi, 96), std::max<LONG>(1, client.right - margin * 2));
        const LONG rowHeight = MulDiv(38, dpi, 96);
        const LONG separatorGap = MulDiv(9, dpi, 96);
        const LONG padding = MulDiv(kContextMenuPaddingDip, dpi, 96);
        const LONG height = ModelActive() ? padding * 2 + rowHeight : padding * 2 + rowHeight * kContextMenuRowCount + separatorGap * kContextMenuSeparatorCount;
        if (tutorialContextMenu_) {
            const LONG canvasTop = fullscreen_ ? 0 : GetFrameMetrics(window_).titleBarHeight;
            const LONG rightInset = MulDiv(24, dpi, 96);
            const LONG left = std::max(margin, client.right - width - rightInset);
            const LONG top = std::clamp<LONG>(canvasTop + MulDiv(24, dpi, 96), margin,
                std::max<LONG>(margin, client.bottom - height - margin));
            return { left, top, left + width, top + height };
        }
        const LONG left = std::clamp<LONG>(contextMenuAnchor_.x, margin, std::max<LONG>(margin, client.right - width - margin));
        const LONG top = std::clamp<LONG>(contextMenuAnchor_.y, margin, std::max<LONG>(margin, client.bottom - height - margin));
        return { left, top, left + width, top + height };
    }

    int MeasureTutorialButtonWidth(const wchar_t* label) const {
        const UINT dpi = GetDpiForWindow(window_);
        const float scale = static_cast<float>(dpi) / 96.0f;
        ComPtr<IDWriteTextFormat> format;
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * scale, L"", &format)) &&
            SUCCEEDED(dwriteFactory_->CreateTextLayout(label, static_cast<UINT32>(wcslen(label)), format.Get(), 200.0f * scale, 36.0f * scale, &layout))) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) return static_cast<int>(std::ceil(metrics.width)) + MulDiv(32, dpi, 96);
        }
        return MulDiv(82, dpi, 96);
    }

    RECT GetTutorialButtonBounds(bool next) const {
        RECT client{};
        GetClientRect(window_, &client);
        const UINT dpi = GetDpiForWindow(window_);
        const int nextWidth = MeasureTutorialButtonWidth(tutorialStep_ == TutorialStep::Shortcuts ? L"Finish" : L"Next");
        const int skipWidth = MeasureTutorialButtonWidth(L"Skip");
        const int height = MulDiv(36, dpi, 96);
        const int gap = MulDiv(10, dpi, 96);
        const int right = client.right - MulDiv(24, dpi, 96);
        const int top = client.bottom - MulDiv(22, dpi, 96) - height;
        if (next) return { right - nextWidth, top, right, top + height };
        return { right - nextWidth - gap - skipWidth, top, right - nextWidth - gap, top + height };
    }

    bool TutorialButtonContains(POINT point, bool next) const {
        const RECT button = GetTutorialButtonBounds(next);
        return TutorialActive() && PtInRect(&button, point);
    }

    void SetTutorialStep(TutorialStep step) {
        DismissDropdown(false);
        DismissContextMenu();
        DismissOverlay();
        tutorialContextMenu_ = false;
        tutorialStep_ = step;
        if (step == TutorialStep::MenuSettings) {
            dropdownOpen_ = true;
        } else if (step == TutorialStep::ContextMenu) {
            RECT client{};
            GetClientRect(window_, &client);
            contextMenuAnchor_ = { std::max(0L, client.right - MulDiv(290, GetDpiForWindow(window_), 96)),
                std::max(0L, client.bottom / 2 - MulDiv(110, GetDpiForWindow(window_), 96)) };
            contextMenuOpen_ = true;
            tutorialContextMenu_ = true;
        } else if (step == TutorialStep::Shortcuts) {
            overlay_ = OverlayKind::KeyboardShortcuts;
        }
        ClearButtonPressed();
        SetButtonHover(ButtonKind::None);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void ShowActionError(const wchar_t* message) const { MessageBoxW(window_, message, kWindowTitle, MB_OK | MB_ICONWARNING); }

    void OpenWith() {
        if (currentPath_.empty()) return;
        OPENASINFO info{};
        info.pcszFile = currentPath_.c_str();
        info.oaifInFlags = OAIF_EXEC;
        if (FAILED(SHOpenWithDialog(window_, &info))) ShowActionError(L"Windows could not open the Open With chooser for this image.");
    }

    void StartCopyFeedback(const wchar_t* text = L"Copied to Clipboard", bool wallpaper = false) {
        feedbackText_ = text;
        feedbackIsWallpaper_ = wallpaper;
        copyFeedbackStart_ = GetTickCount64();
        copyFeedbackActive_ = true;
        SetTimer(window_, kCopyFeedbackTimer, animationsEnabled_ ? 16 : 100, nullptr);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void CopyImage() {
        if (!source_ || imageWidth_ == 0 || imageHeight_ == 0) return;
        const UINT stride = imageWidth_ * 4;
        const size_t pixelBytes = static_cast<size_t>(stride) * imageHeight_;
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + pixelBytes);
        if (!memory) { ShowActionError(L"Viewtrious could not allocate clipboard memory."); return; }
        auto* header = static_cast<BITMAPV5HEADER*>(GlobalLock(memory));
        if (!header) { GlobalFree(memory); ShowActionError(L"Viewtrious could not access clipboard memory."); return; }
        *header = {};
        header->bV5Size = sizeof(BITMAPV5HEADER);
        header->bV5Width = static_cast<LONG>(imageWidth_);
        header->bV5Height = -static_cast<LONG>(imageHeight_);
        header->bV5Planes = 1; header->bV5BitCount = 32; header->bV5Compression = BI_BITFIELDS;
        header->bV5SizeImage = static_cast<DWORD>(pixelBytes);
        header->bV5RedMask = 0x00FF0000; header->bV5GreenMask = 0x0000FF00;
        header->bV5BlueMask = 0x000000FF; header->bV5AlphaMask = 0xFF000000; header->bV5CSType = LCS_sRGB;
        const HRESULT copy = source_->CopyPixels(nullptr, stride, static_cast<UINT>(pixelBytes), reinterpret_cast<BYTE*>(header + 1));
        GlobalUnlock(memory);
        if (FAILED(copy)) { GlobalFree(memory); ShowActionError(L"Viewtrious could not copy this image to the clipboard."); return; }
        const size_t dropBytes = sizeof(DROPFILES) + (currentPath_.size() + 2) * sizeof(wchar_t);
        HGLOBAL fileDrop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, dropBytes);
        if (fileDrop) {
            auto* drop = static_cast<DROPFILES*>(GlobalLock(fileDrop));
            if (!drop) { GlobalFree(fileDrop); fileDrop = nullptr; }
            else {
                drop->pFiles = sizeof(DROPFILES);
                drop->fWide = TRUE;
                wchar_t* paths = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES));
                memcpy(paths, currentPath_.c_str(), (currentPath_.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(fileDrop);
            }
        }
        if (!OpenClipboard(window_)) { GlobalFree(memory); if (fileDrop) GlobalFree(fileDrop); ShowActionError(L"The clipboard is currently unavailable."); return; }
        EmptyClipboard();
        if (!SetClipboardData(CF_DIBV5, memory)) { CloseClipboard(); GlobalFree(memory); if (fileDrop) GlobalFree(fileDrop); ShowActionError(L"Viewtrious could not publish the image to the clipboard."); return; }
        if (fileDrop && !SetClipboardData(CF_HDROP, fileDrop)) GlobalFree(fileDrop);
        CloseClipboard();
        StartCopyFeedback();
    }

    static std::wstring DescribeWallpaperFailure(HRESULT hr) {
        wchar_t value[16]{};
        swprintf_s(value, L"0x%08X", static_cast<unsigned int>(hr));
        LPWSTR message = nullptr;
        const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
        std::wstring result = value;
        if (length && message) {
            std::wstring description(message, length);
            while (!description.empty() && (description.back() == L'\r' || description.back() == L'\n')) description.pop_back();
            result += L" — " + description;
        }
        if (message) LocalFree(message);
        return result;
    }

    void ShowWallpaperFailure(const wchar_t* stage, HRESULT hr) const {
        const std::wstring message = std::wstring(stage) + L"\n\n" + DescribeWallpaperFailure(hr);
        MessageBoxW(window_, message.c_str(), L"Viewtrious", MB_OK | MB_ICONWARNING);
    }

    static bool SourcePathRejectedByWallpaper(HRESULT hr) {
        return hr == E_INVALIDARG || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
            hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) ||
            hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    void SetDesktopBackground() {
        if (currentPath_.empty()) return;
        ComPtr<IDesktopWallpaper> wallpaper;
        const HRESULT createResult = CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wallpaper));
        if (FAILED(createResult)) { ShowWallpaperFailure(L"Windows could not create the desktop wallpaper service.", createResult); return; }
        const HRESULT directResult = wallpaper->SetWallpaper(nullptr, currentPath_.c_str());
        if (SUCCEEDED(directResult)) { StartCopyFeedback(L"Desktop background updated", true); return; }
        if (!SourcePathRejectedByWallpaper(directResult)) {
            ShowWallpaperFailure(L"Windows could not apply this image as the desktop background.", directResult);
            return;
        }

        std::wstring wallpaperPath;
        const HRESULT stagingResult = ExportDesktopWallpaper(wallpaperPath);
        if (FAILED(stagingResult)) { ShowWallpaperFailure(L"Viewtrious could not create a compatible desktop background.", stagingResult); return; }
        const HRESULT fallbackResult = wallpaper->SetWallpaper(nullptr, wallpaperPath.c_str());
        if (FAILED(fallbackResult)) { ShowWallpaperFailure(L"Windows could not apply the compatible desktop background.", fallbackResult); return; }
        StartCopyFeedback(L"Desktop background updated", true);
    }

    HRESULT GetWallpaperStagingDirectory(fs::path& directory) const {
        PWSTR localAppData = nullptr;
        const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData);
        if (FAILED(result)) return result;
        directory = fs::path(localAppData) / L"Viewtrious" / L"Wallpaper";
        CoTaskMemFree(localAppData);
        return S_OK;
    }

    void CleanupWallpaperStaging() {
        fs::path directory;
        if (FAILED(GetWallpaperStagingDirectory(directory))) return;
        const fs::path wallpaperPath = directory / L"current.bmp";
        ComPtr<IDesktopWallpaper> wallpaper;
        HRESULT wallpaperResult = CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wallpaper));
        bool preserveActiveFile = FAILED(wallpaperResult);
        UINT monitorCount = 0;
        if (SUCCEEDED(wallpaperResult)) wallpaperResult = wallpaper->GetMonitorDevicePathCount(&monitorCount);
        for (UINT index = 0; SUCCEEDED(wallpaperResult) && index < monitorCount; ++index) {
            LPWSTR monitorId = nullptr, activePath = nullptr;
            wallpaperResult = wallpaper->GetMonitorDevicePathAt(index, &monitorId);
            if (SUCCEEDED(wallpaperResult)) wallpaperResult = wallpaper->GetWallpaper(monitorId, &activePath);
            if (activePath && PathsEqual(wallpaperPath, fs::path(activePath))) preserveActiveFile = true;
            if (monitorId) CoTaskMemFree(monitorId);
            if (activePath) CoTaskMemFree(activePath);
        }
        if (FAILED(wallpaperResult)) preserveActiveFile = true;
        if (preserveActiveFile) return;
        std::error_code error;
        fs::remove_all(directory, error);
    }

    HRESULT ExportDesktopWallpaper(std::wstring& wallpaperPath) {
        if (!source_) return E_FAIL;
        fs::path directory;
        HRESULT hr = GetWallpaperStagingDirectory(directory);
        if (FAILED(hr)) return hr;
        std::error_code error;
        fs::create_directories(directory, error);
        if (error) return HRESULT_FROM_WIN32(static_cast<DWORD>(error.value()));
        wallpaperPath = (directory / L"current.bmp").wstring();
        wchar_t temporaryPath[MAX_PATH]{};
        if (!GetTempFileNameW(directory.c_str(), L"VTR", 0, temporaryPath)) return HRESULT_FROM_WIN32(GetLastError());
        const std::wstring temporary = temporaryPath;
        const auto discardTemporary = [&] { DeleteFileW(temporary.c_str()); };

        ComPtr<IWICFormatConverter> converter;
        hr = wicFactory_->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) hr = converter->Initialize(source_.Get(), GUID_WICPixelFormat32bppBGR,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        ComPtr<IWICStream> stream;
        if (SUCCEEDED(hr)) hr = wicFactory_->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE);
        ComPtr<IWICBitmapEncoder> encoder;
        if (SUCCEEDED(hr)) hr = wicFactory_->CreateEncoder(GUID_ContainerFormatBmp, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        ComPtr<IWICBitmapFrameEncode> frame;
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
        if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
        UINT width = 0, height = 0;
        if (SUCCEEDED(hr)) hr = converter->GetSize(&width, &height);
        if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGR;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&pixelFormat);
        if (SUCCEEDED(hr)) hr = frame->WriteSource(converter.Get(), nullptr);
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        frame.Reset(); encoder.Reset(); stream.Reset(); converter.Reset();
        if (FAILED(hr)) { discardTemporary(); return hr; }

        ComPtr<IWICBitmapDecoder> validationDecoder;
        hr = wicFactory_->CreateDecoderFromFilename(temporary.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &validationDecoder);
        ComPtr<IWICBitmapFrameDecode> validationFrame;
        if (SUCCEEDED(hr)) hr = validationDecoder->GetFrame(0, &validationFrame);
        UINT validatedWidth = 0, validatedHeight = 0;
        if (SUCCEEDED(hr)) hr = validationFrame->GetSize(&validatedWidth, &validatedHeight);
        if (SUCCEEDED(hr) && (validatedWidth != imageWidth_ || validatedHeight != imageHeight_)) hr = E_FAIL;
        validationFrame.Reset(); validationDecoder.Reset();
        if (FAILED(hr)) { discardTemporary(); return hr; }
        if (!MoveFileExW(temporary.c_str(), wallpaperPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            discardTemporary();
        }
        return hr;
    }

    void PrintImage() {
        if (currentPath_.empty()) return;
        SHELLEXECUTEINFOW execute{ sizeof(execute) };
        execute.fMask = SEE_MASK_FLAG_NO_UI; execute.hwnd = window_; execute.lpVerb = L"print";
        execute.lpFile = currentPath_.c_str(); execute.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&execute)) ShowActionError(L"Windows could not find a print handler for this image.");
    }

    static UINT RotatedOrientation(UINT orientation, bool clockwise) {
        static constexpr UINT clockwiseMap[] = { 0, 6, 7, 8, 5, 2, 3, 4, 1 };
        static constexpr UINT counterClockwiseMap[] = { 0, 8, 5, 6, 7, 4, 1, 2, 3 };
        if (orientation < 1 || orientation > 8) orientation = 1;
        return clockwise ? clockwiseMap[orientation] : counterClockwiseMap[orientation];
    }

    UINT ReadPhotoOrientation(IWICBitmapFrameDecode* frame) const {
        ComPtr<IWICMetadataQueryReader> metadata;
        UINT orientation = 1;
        if (SUCCEEDED(frame->GetMetadataQueryReader(&metadata))) {
            for (const wchar_t* query : { L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}", L"/{ushort=274}" }) {
                PROPVARIANT value{};
                PropVariantInit(&value);
                const HRESULT result = metadata->GetMetadataByName(query, &value);
                if (SUCCEEDED(result)) {
                    if (value.vt == VT_UI2) orientation = value.uiVal;
                    else if (value.vt == VT_UI4) orientation = value.ulVal;
                }
                PropVariantClear(&value);
                if (orientation >= 1 && orientation <= 8 && SUCCEEDED(result)) break;
            }
        }
        return orientation >= 1 && orientation <= 8 ? orientation : 1;
    }

    WICBitmapTransformOptions TransformForOrientation(UINT orientation) const {
        switch (orientation) {
        case 2: return WICBitmapTransformFlipHorizontal;
        case 3: return WICBitmapTransformRotate180;
        case 4: return WICBitmapTransformFlipVertical;
        case 5: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
        case 6: return WICBitmapTransformRotate90;
        case 7: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
        case 8: return WICBitmapTransformRotate270;
        default: return WICBitmapTransformRotate0;
        }
    }

    void LogRotationStage(const wchar_t* stage, HRESULT hr, DWORD win32Error = ERROR_SUCCESS) const {
        if (SUCCEEDED(hr)) return;
        wchar_t text[320]{};
        swprintf_s(text, L"Viewtrious image rotation [%s]: HRESULT=0x%08X, Win32=%lu\n", stage,
            static_cast<unsigned int>(hr), win32Error);
        OutputDebugStringW(text);
    }

    void ShowRotationFailure(const wchar_t* stage, HRESULT hr, DWORD win32Error) const {
        wchar_t text[512]{};
        swprintf_s(text, L"Image rotation failed at %s.\n\nHRESULT: 0x%08X\nWin32 error: %lu%s%s\n\nThe original file was left unchanged.",
            stage, static_cast<unsigned int>(hr), win32Error, rotationDiagnosticDetail_.empty() ? L"" : L"\n",
            rotationDiagnosticDetail_.c_str());
        MessageBoxW(window_, text, kWindowTitle, MB_OK | MB_ICONWARNING);
    }

    void DiagnoseRotationSharingViolation() const {
        HANDLE probe = CreateFileW(currentPath_.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (probe == INVALID_HANDLE_VALUE) {
            LogRotationStage(L"sharing probe after Viewtrious WIC release: external incompatible handle", HRESULT_FROM_WIN32(GetLastError()), GetLastError());
            return;
        }
        CloseHandle(probe);
        LogRotationStage(L"sharing probe after Viewtrious WIC release: source is externally writable; Shell property-store-specific conflict", S_OK);
    }

    HRESULT GetShellContextMenu(const std::wstring& path, ComPtr<IContextMenu>& contextMenu, UINT& commandCount) const {
        contextMenu.Reset();
        commandCount = 0;
        PIDLIST_ABSOLUTE itemPidl = nullptr;
        HRESULT hr = SHParseDisplayName(path.c_str(), nullptr, &itemPidl, 0, nullptr);
        if (FAILED(hr)) return hr;
        ComPtr<IShellFolder> parent;
        PCUITEMID_CHILD child = nullptr;
        hr = SHBindToParent(itemPidl, IID_PPV_ARGS(&parent), &child);
        if (SUCCEEDED(hr)) hr = parent->GetUIObjectOf(window_, 1, &child, IID_IContextMenu, nullptr,
            reinterpret_cast<void**>(contextMenu.GetAddressOf()));
        CoTaskMemFree(itemPidl);
        if (FAILED(hr)) return hr;
        HMENU menu = CreatePopupMenu();
        if (!menu) return HRESULT_FROM_WIN32(GetLastError());
        const HRESULT queried = contextMenu->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL | CMF_EXTENDEDVERBS);
        DestroyMenu(menu);
        if (FAILED(queried)) { contextMenu.Reset(); return queried; }
        commandCount = HRESULT_CODE(queried);
        return S_OK;
    }

    void RefreshHeifShellRotationCapability() {
        if (!IsHeifPath(currentPath_)) {
            heifShellRotationCapabilityPath_.clear();
            heifShellRotateLeftAvailable_ = false;
            heifShellRotateRightAvailable_ = false;
            return;
        }
        if (PathsEqual(fs::path(heifShellRotationCapabilityPath_), fs::path(currentPath_))) return;
        heifShellRotationCapabilityPath_ = currentPath_;
        heifShellRotateLeftAvailable_ = false;
        heifShellRotateRightAvailable_ = false;
        ComPtr<IContextMenu> contextMenu;
        UINT commandCount = 0;
        if (FAILED(GetShellContextMenu(currentPath_, contextMenu, commandCount))) return;
        for (UINT offset = 0; offset < commandCount; ++offset) {
            wchar_t verb[64]{};
            if (FAILED(contextMenu->GetCommandString(offset, GCS_VERBW, nullptr,
                    reinterpret_cast<LPSTR>(verb), ARRAYSIZE(verb)))) continue;
            if (_wcsicmp(verb, L"rotate90") == 0) heifShellRotateRightAvailable_ = true;
            else if (_wcsicmp(verb, L"rotate270") == 0) heifShellRotateLeftAvailable_ = true;
        }
    }

    bool ReadShellRotationFileState(const std::wstring& path, WIN32_FILE_ATTRIBUTE_DATA& attributes) const {
        return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) != FALSE;
    }

    HRESULT DetachDisplayedImageForShellWrite() {
        if (!source_) return E_FAIL;
        ComPtr<IWICBitmap> detached;
        const HRESULT hr = wicFactory_->CreateBitmapFromSource(source_.Get(), WICBitmapCacheOnLoad, &detached);
        if (FAILED(hr)) return hr;
        source_ = detached;
        displayedPixels_.reset();
        bitmap_.Reset();
        return S_OK;
    }

    void BeginShellRotationRefresh() {
        ++decodeRequestGeneration_;
        pendingFullDecode_.reset();
        imageDecodePending_ = false;
        KillTimer(window_, kNavigationDecodeDebounceTimer);
        shellRotationPath_ = currentPath_;
        shellRotationStarted_ = GetTickCount64();
        shellRotationInitialStateValid_ = ReadShellRotationFileState(shellRotationPath_, shellRotationInitialState_);
        shellRotationPending_ = true;
        shellRotationStableChecks_ = 0;
        shellRotationLastStateValid_ = false;
        SetTimer(window_, kShellRotationCheckTimer, kShellRotationCheckIntervalMs, nullptr);
    }

    bool IsHeifRotationGateActive() const {
        return shellRotationPending_ || GetTickCount64() < heifRotationCooldownUntil_;
    }

    void BeginHeifRotationCooldown() {
        heifRotationCooldownUntil_ = GetTickCount64() + kHeifRotationCooldownMs;
    }

    void RefreshHeifRotationContextMenu() {
        if (!contextMenuOpen_ || !heifRotationMenuLocked_ || !IsHeifPath(currentPath_)) {
            KillTimer(window_, kHeifRotationMenuRefreshTimer);
            return;
        }
        if (IsHeifRotationGateActive()) return;
        heifRotationMenuLocked_ = false;
        KillTimer(window_, kHeifRotationMenuRefreshTimer);
        InvalidateRect(window_, nullptr, FALSE);
        UpdateWindow(window_);
    }

    void FailShellRotationRefresh() {
        KillTimer(window_, kShellRotationCheckTimer);
        shellRotationPending_ = false;
        BeginHeifRotationCooldown();
        ShowActionError(L"HEIC rotation did not complete.");
    }

    void CompleteShellRotationRefresh() {
        KillTimer(window_, kShellRotationCheckTimer);
        shellRotationPending_ = false;
        BeginHeifRotationCooldown();
        if (!PathsEqual(fs::path(shellRotationPath_), fs::path(currentPath_))) return;
        currentFileIdentity_ = ReadFileIdentity(fs::path(currentPath_));
        fileSizeText_ = FormatFileSize(currentPath_);
        imageDecodePending_ = true;
        pendingFullDecode_ = DecodeRequest{ currentPath_, decodeRequestGeneration_, navigationFolderGeneration_ };
        QueueLatestFullDecode();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void UpdateShellRotation() {
        if (!shellRotationPending_) { KillTimer(window_, kShellRotationCheckTimer); return; }
        WIN32_FILE_ATTRIBUTE_DATA state{};
        ++shellRotationProbeCount_;
        const bool changed = ReadShellRotationFileState(shellRotationPath_, state) && (!shellRotationInitialStateValid_ ||
            CompareFileTime(&state.ftLastWriteTime, &shellRotationInitialState_.ftLastWriteTime) != 0 ||
            state.nFileSizeHigh != shellRotationInitialState_.nFileSizeHigh || state.nFileSizeLow != shellRotationInitialState_.nFileSizeLow);
        if (changed) {
            const bool stable = shellRotationLastStateValid_ &&
                CompareFileTime(&state.ftLastWriteTime, &shellRotationLastState_.ftLastWriteTime) == 0 &&
                state.nFileSizeHigh == shellRotationLastState_.nFileSizeHigh && state.nFileSizeLow == shellRotationLastState_.nFileSizeLow;
            shellRotationLastState_ = state;
            shellRotationLastStateValid_ = true;
            shellRotationStableChecks_ = stable ? shellRotationStableChecks_ + 1 : 0;
            if (shellRotationStableChecks_ >= 2) {
                ComPtr<IWICBitmapDecoder> decoder;
                const HRESULT reopen = wicFactory_->CreateDecoderFromFilename(shellRotationPath_.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
                if (SUCCEEDED(reopen)) { CompleteShellRotationRefresh(); return; }
            }
        } else {
        shellRotationStableChecks_ = 0;
        shellRotationLastStateValid_ = false;
        }
        if (GetTickCount64() - shellRotationStarted_ >= kShellRotationTimeoutMs) {
            FailShellRotationRefresh();
        }
    }

    HRESULT RotateHeifWithShell(bool clockwise) {
        if (IsHeifRotationGateActive()) return HRESULT_FROM_WIN32(ERROR_BUSY);
        shellRotationPending_ = true;
        RefreshHeifShellRotationCapability();
        const bool available = clockwise ? heifShellRotateRightAvailable_ : heifShellRotateLeftAvailable_;
        if (!available) { FailShellRotationRefresh(); return E_NOTIMPL; }
        ComPtr<IContextMenu> contextMenu;
        UINT commandCount = 0;
        HRESULT hr = GetShellContextMenu(currentPath_, contextMenu, commandCount);
        if (FAILED(hr)) { FailShellRotationRefresh(); return hr; }
        const wchar_t* verb = clockwise ? L"rotate90" : L"rotate270";
        bool found = false;
        for (UINT offset = 0; offset < commandCount; ++offset) {
            wchar_t candidate[64]{};
            if (SUCCEEDED(contextMenu->GetCommandString(offset, GCS_VERBW, nullptr,
                    reinterpret_cast<LPSTR>(candidate), ARRAYSIZE(candidate))) && _wcsicmp(candidate, verb) == 0) {
                found = true;
                break;
            }
        }
        if (!found) { FailShellRotationRefresh(); return HRESULT_FROM_WIN32(ERROR_NOT_FOUND); }
        hr = DetachDisplayedImageForShellWrite();
        if (FAILED(hr)) { FailShellRotationRefresh(); return hr; }
        BeginShellRotationRefresh();
        CMINVOKECOMMANDINFOEX invoke{ sizeof(invoke) };
        // Do not authorize asynchronous execution here. The Shell handler may otherwise
        // report a timestamp change before its own HEIC writer has released the file.
        // CMINVOKECOMMANDINFOEX is accepted through its CMINVOKECOMMANDINFO base.
        invoke.fMask = CMIC_MASK_UNICODE;
        invoke.hwnd = window_;
        invoke.lpVerb = clockwise ? "rotate90" : "rotate270";
        invoke.lpVerbW = verb;
        invoke.nShow = SW_SHOWNORMAL;
        hr = contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        if (FAILED(hr)) {
            FailShellRotationRefresh();
            return hr;
        }
        // InvokeCommand has returned; retaining the handler can keep handler-owned state
        // alive and needlessly contend with the next rotation.
        contextMenu.Reset();
        return S_OK;
    }

    HRESULT RotateJpeg(bool clockwise, const wchar_t*& failedStage, DWORD& failedWin32Error) {
        failedStage = nullptr;
        failedWin32Error = ERROR_SUCCESS;
        LogRotationStage(L"strategy: EXIF Orientation metadata change (no JPEG re-encode)", S_OK);
        const auto stage = [&](const wchar_t* name, HRESULT result) {
            const DWORD win32Error = FAILED(result) ? GetLastError() : ERROR_SUCCESS;
            LogRotationStage(name, result, win32Error);
            if (FAILED(result)) { failedStage = name; failedWin32Error = win32Error; }
            return SUCCEEDED(result);
        };
        ComPtr<IWICBitmapDecoder> decoder;
        HRESULT hr = wicFactory_->CreateDecoderFromFilename(currentPath_.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder);
        if (!stage(L"source file open: IWICImagingFactory::CreateDecoderFromFilename", hr)) return hr;
        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, &frame);
        if (!stage(L"source frame read: IWICBitmapDecoder::GetFrame", hr)) return hr;
        const UINT orientation = ReadPhotoOrientation(frame.Get());
        LogRotationStage(L"metadata/orientation read", S_OK);
        frame.Reset();
        decoder.Reset();
        LogRotationStage(L"release rotation-owned WIC decoder/frame before writable property store", S_OK);

        ComPtr<IPropertyStore> store;
        hr = SHGetPropertyStoreFromParsingName(currentPath_.c_str(), nullptr, GPS_READWRITE, IID_PPV_ARGS(&store));
        if (hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) DiagnoseRotationSharingViolation();
        if (!stage(L"metadata store open: SHGetPropertyStoreFromParsingName(GPS_READWRITE)", hr)) return hr;
        PROPVARIANT value{};
        PropVariantInit(&value);
        value.vt = VT_UI2;
        value.uiVal = static_cast<USHORT>(RotatedOrientation(orientation, clockwise));
        hr = store->SetValue(PKEY_Photo_Orientation, value);
        if (!stage(L"metadata/orientation write: IPropertyStore::SetValue", hr)) { PropVariantClear(&value); return hr; }
        hr = store->Commit();
        PropVariantClear(&value);
        if (!stage(L"metadata commit: IPropertyStore::Commit", hr)) return hr;
        LogRotationStage(L"temporary output creation: not applicable to metadata-only JPEG rotation", S_OK);
        LogRotationStage(L"transform operation: EXIF orientation value updated", S_OK);
        LogRotationStage(L"output write/close/flush/original-file replacement: not applicable to metadata-only JPEG rotation", S_OK);
        return hr;
    }

    static HRESULT GdiplusStatusToHresult(Gdiplus::Status status) {
        if (status == Gdiplus::Ok) return S_OK;
        if (status == Gdiplus::OutOfMemory) return E_OUTOFMEMORY;
        if (status == Gdiplus::InvalidParameter) return E_INVALIDARG;
        if (status == Gdiplus::AccessDenied) return E_ACCESSDENIED;
        return E_FAIL;
    }

    static bool FindPngEncoder(CLSID& encoderClsid) {
        UINT count = 0, bytes = 0;
        if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0) return false;
        std::vector<BYTE> storage(bytes);
        auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
        if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) return false;
        for (UINT index = 0; index < count; ++index) {
            if (encoders[index].MimeType && wcscmp(encoders[index].MimeType, L"image/png") == 0) {
                encoderClsid = encoders[index].Clsid;
                return true;
            }
        }
        return false;
    }

    HRESULT RotatePngWithGdiPlus(bool clockwise, const wchar_t*& failedStage, DWORD& failedWin32Error) {
        failedStage = nullptr;
        failedWin32Error = ERROR_SUCCESS;
        rotationDiagnosticDetail_.clear();
        const auto stage = [&](const wchar_t* name, HRESULT result) {
            const DWORD win32Error = FAILED(result) ? GetLastError() : ERROR_SUCCESS;
            LogRotationStage(name, result, win32Error);
            if (FAILED(result)) { failedStage = name; failedWin32Error = win32Error; }
            return SUCCEEDED(result);
        };
        const auto gdiplusStage = [&](const wchar_t* name, Gdiplus::Status status) {
            const HRESULT result = GdiplusStatusToHresult(status);
            if (FAILED(result)) rotationDiagnosticDetail_ = L"GDI+ Status: " + std::to_wstring(static_cast<unsigned int>(status));
            return stage(name, result);
        };
        struct TemporarySiblingFile {
            std::wstring path;
            ~TemporarySiblingFile() { if (!path.empty()) DeleteFileW(path.c_str()); }
            void Release() { path.clear(); }
        } temporary;

        WIN32_FILE_ATTRIBUTE_DATA originalAttributes{};
        if (!GetFileAttributesExW(currentPath_.c_str(), GetFileExInfoStandard, &originalAttributes)) {
            const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
            stage(L"GDI+ source PNG attributes: GetFileAttributesExW", hr);
            return hr;
        }

        wchar_t tempPath[MAX_PATH]{};
        const std::wstring directory = fs::path(currentPath_).parent_path().wstring();
        if (!GetTempFileNameW(directory.c_str(), L"VTR", 0, tempPath)) {
            const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
            stage(L"GDI+ temp sibling path creation: GetTempFileNameW", hr);
            return hr;
        }
        temporary.path = tempPath;
        if (!DeleteFileW(temporary.path.c_str())) {
            const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
            stage(L"GDI+ temp output preparation: DeleteFileW", hr);
            return hr;
        }

        struct GdiplusSession {
            ULONG_PTR token = 0;
            bool active = false;
            ~GdiplusSession() { if (active) Gdiplus::GdiplusShutdown(token); }
            void Close() { if (active) { Gdiplus::GdiplusShutdown(token); active = false; } }
        } gdiplus;
        Gdiplus::GdiplusStartupInput startupInput;
        Gdiplus::Status gdiplusStatus = Gdiplus::GdiplusStartup(&gdiplus.token, &startupInput, nullptr);
        HRESULT hr = GdiplusStatusToHresult(gdiplusStatus);
        if (!gdiplusStage(L"GDI+ initialization: GdiplusStartup", gdiplusStatus)) return hr;
        gdiplus.active = true;
        UINT sourceProperties = 0, outputProperties = 0;
        std::vector<PROPID> sourcePropertyIds, outputPropertyIds;
        bool sourceHasAlpha = false, outputHasAlpha = false;
        {
            Gdiplus::Image image(currentPath_.c_str(), FALSE);
            gdiplusStatus = image.GetLastStatus();
            hr = GdiplusStatusToHresult(gdiplusStatus);
            if (!gdiplusStage(L"GDI+ source PNG open/decode: Image::GetLastStatus", gdiplusStatus)) return hr;
            sourceProperties = image.GetPropertyCount();
            sourcePropertyIds.resize(sourceProperties);
            if (sourceProperties != 0 && image.GetPropertyIdList(sourceProperties, sourcePropertyIds.data()) != Gdiplus::Ok) {
                failedStage = L"GDI+ source PNG metadata enumeration";
                return E_FAIL;
            }
            sourceHasAlpha = (image.GetPixelFormat() & 0x00040000u) != 0;
            gdiplusStatus = image.RotateFlip(clockwise ? Gdiplus::Rotate90FlipNone : Gdiplus::Rotate270FlipNone);
            hr = GdiplusStatusToHresult(gdiplusStatus);
            if (!gdiplusStage(L"GDI+ transform/rotation: Image::RotateFlip", gdiplusStatus)) return hr;
            CLSID pngEncoder{};
            if (!FindPngEncoder(pngEncoder)) { failedStage = L"GDI+ PNG encoder discovery"; return E_FAIL; }
            gdiplusStatus = image.Save(temporary.path.c_str(), &pngEncoder, nullptr);
            hr = GdiplusStatusToHresult(gdiplusStatus);
            if (!gdiplusStage(L"GDI+ PNG save: Image::Save", gdiplusStatus)) return hr;
        }

        WIN32_FILE_ATTRIBUTE_DATA temporaryAttributes{};
        if (!GetFileAttributesExW(temporary.path.c_str(), GetFileExInfoStandard, &temporaryAttributes)) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            stage(L"GDI+ output file check: GetFileAttributesExW", hr);
            return hr;
        }
        HANDLE signatureFile = CreateFileW(temporary.path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (signatureFile == INVALID_HANDLE_VALUE) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            stage(L"GDI+ output PNG signature open: CreateFileW", hr);
            return hr;
        }
        BYTE signature[8]{};
        DWORD bytesRead = 0;
        const bool signatureRead = ReadFile(signatureFile, signature, sizeof(signature), &bytesRead, nullptr) && bytesRead == sizeof(signature);
        CloseHandle(signatureFile);
        static constexpr BYTE kPngSignature[] = { 137, 80, 78, 71, 13, 10, 26, 10 };
        if (!signatureRead || memcmp(signature, kPngSignature, sizeof(signature)) != 0) {
            hr = E_FAIL;
            stage(L"GDI+ output PNG signature validation", hr);
            return hr;
        }

        {
            Gdiplus::Image output(temporary.path.c_str(), FALSE);
            gdiplusStatus = output.GetLastStatus();
            hr = GdiplusStatusToHresult(gdiplusStatus);
            if (!gdiplusStage(L"GDI+ temporary PNG validation: Image::GetLastStatus", gdiplusStatus)) return hr;
            outputProperties = output.GetPropertyCount();
            outputPropertyIds.resize(outputProperties);
            if (outputProperties != 0 && output.GetPropertyIdList(outputProperties, outputPropertyIds.data()) != Gdiplus::Ok) {
                failedStage = L"GDI+ temporary PNG metadata enumeration";
                return E_FAIL;
            }
            outputHasAlpha = (output.GetPixelFormat() & 0x00040000u) != 0;
        }
        gdiplus.Close();
        LogRotationStage(L"GDI+ close: released source image, validation image, and encoder state", S_OK);
        if (sourceHasAlpha && !outputHasAlpha) {
            failedStage = L"PNG alpha preservation validation";
            rotationDiagnosticDetail_ = L"The source PNG has alpha, but the GDI+ output does not.";
            return E_FAIL;
        }
        const auto propertyName = [](PROPID id) -> const wchar_t* {
            switch (id) {
            case 0x010E: return L"ImageDescription";
            case 0x0112: return L"Orientation";
            case 0x011A: return L"XResolution";
            case 0x011B: return L"YResolution";
            case 0x0128: return L"ResolutionUnit";
            case 0x0131: return L"Software";
            case 0x0132: return L"DateTime";
            case 0x013B: return L"Artist";
            case 0x0301: return L"Gamma";
            default: return L"unknown or codec-specific metadata";
            }
        };
        const auto mustPreserve = [](PROPID id) {
            switch (id) {
            case 0x010E: case 0x0112: case 0x011A: case 0x011B: case 0x0128:
            case 0x0131: case 0x0132: case 0x013B: case 0x0301:
                return true;
            default:
                return false;
            }
        };
        const auto missing = std::find_if(sourcePropertyIds.begin(), sourcePropertyIds.end(), [&](PROPID id) {
            if (!mustPreserve(id)) return false;
            return std::find(outputPropertyIds.begin(), outputPropertyIds.end(), id) == outputPropertyIds.end();
        });
        if (missing != sourcePropertyIds.end()) {
            wchar_t detail[256]{};
            swprintf_s(detail, L"GDI+ dropped property ID 0x%04X (%s). Source property count: %u; output count: %u.",
                *missing, propertyName(*missing), sourceProperties, outputProperties);
            failedStage = L"PNG metadata preservation validation";
            rotationDiagnosticDetail_ = detail;
            return E_FAIL;
        }

        const auto probe = [&](const std::wstring& path, const wchar_t* name) {
            HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                const HRESULT probeHr = HRESULT_FROM_WIN32(GetLastError());
                stage(name, probeHr);
                return probeHr;
            }
            CloseHandle(handle);
            return S_OK;
        };
        hr = probe(currentPath_, L"GDI+ replacement boundary probe: original source path");
        if (FAILED(hr)) return hr;
        hr = probe(temporary.path, L"GDI+ replacement boundary probe: temporary replacement path");
        if (FAILED(hr)) return hr;
        if (!ReplaceFileW(currentPath_.c_str(), temporary.path.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            stage(L"GDI+ replacement of original: ReplaceFileW", hr);
            return hr;
        }
        temporary.Release();
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, currentPath_.c_str(), nullptr);
        return S_OK;
    }

    HRESULT RotatePng(bool clockwise, const wchar_t*& failedStage, DWORD& failedWin32Error) {
        // GDI+ provides the fast, lossless PNG pixel rotation path while the transaction below keeps replacement safe.
        return RotatePngWithGdiPlus(clockwise, failedStage, failedWin32Error);
    }

    void ExecuteRotationBackend(bool clockwise) {
        if (currentPath_.empty()) return;
        if (IsHeifPath(currentPath_)) {
            RotateHeifWithShell(clockwise);
            return;
        }
        rotationDiagnosticDetail_.clear();
        const wchar_t* failedStage = nullptr;
        DWORD failedWin32Error = ERROR_SUCCESS;
        const HRESULT hr = IsJpegPath(currentPath_) ? RotateJpeg(clockwise, failedStage, failedWin32Error) :
            IsPngPath(currentPath_) ? RotatePng(clockwise, failedStage, failedWin32Error) : E_NOTIMPL;
        if (FAILED(hr)) {
            if (IsJpegPath(currentPath_) || IsPngPath(currentPath_))
                ShowRotationFailure(failedStage ? failedStage : L"unknown rotation stage", hr, failedWin32Error);
            else ShowActionError(L"Viewtrious could not safely rotate this image. The original file was not replaced.");
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        const HRESULT reload = ReloadCurrentImage();
        if (IsJpegPath(currentPath_) || IsPngPath(currentPath_)) LogRotationStage(L"reload: DecodeImage", reload, FAILED(reload) ? GetLastError() : ERROR_SUCCESS);
        if (FAILED(reload) && (IsJpegPath(currentPath_) || IsPngPath(currentPath_))) ShowRotationFailure(L"reload: DecodeImage", reload, GetLastError());
        InvalidateRect(window_, nullptr, FALSE);
    }

    void RotateImage(bool clockwise) {
        if (currentPath_.empty()) return;
        if (IsHeifPath(currentPath_) && IsHeifRotationGateActive()) return;
        ExecuteRotationBackend(clockwise);
    }

    void ClearDeletedImage() {
        StopGifPlayback();
        StopDirectoryWatcher();
        KillTimer(window_, kShellRotationCheckTimer);
        shellRotationPending_ = false;
        ++decodeRequestGeneration_;
        ++navigationFolderGeneration_;
        pendingFullDecode_.reset(); imageDecodePending_ = false;
        KillTimer(window_, kNavigationDecodeDebounceTimer);
        source_.Reset(); bitmap_.Reset(); imageWidth_ = imageHeight_ = 0;
        displayedPixels_.reset();
        currentPath_.clear(); displayedPath_.clear(); currentFileIdentity_ = {}; resolutionText_.clear(); fileSizeText_.clear(); filenameText_.clear();
        navigationFiles_.clear(); navigationBuilt_ = false; navigationBuildQueued_ = false;
        fitToWindow_ = true; zoom_ = 1.0f; pan_ = D2D1::Point2F();
        error_ = L"Drop an image here, or launch Viewtrious with an image path.";
        InvalidateRect(window_, nullptr, FALSE);
    }

    void ShowImageAfterDelete() {
        StopGifPlayback();
        BuildNavigation();
        const fs::path deleted(currentPath_);
        auto current = std::find_if(navigationFiles_.begin(), navigationFiles_.end(),
            [&deleted](const fs::path& path) { return PathsEqual(path, deleted); });
        if (current == navigationFiles_.end()) { ClearDeletedImage(); return; }
        const size_t index = static_cast<size_t>(std::distance(navigationFiles_.begin(), current));
        navigationFiles_.erase(current);
        for (size_t offset = 0; offset < navigationFiles_.size(); ++offset) {
            const size_t candidate = (index + offset) % navigationFiles_.size();
            if (IsGifPath(navigationFiles_[candidate].wstring()) && SUCCEEDED(LoadAnimatedGif(navigationFiles_[candidate].wstring(), false))) {
                InvalidateRect(window_, nullptr, FALSE);
                return;
            }
            ComPtr<IWICBitmapSource> source;
            UINT width = 0, height = 0;
            if (SUCCEEDED(DecodeImage(navigationFiles_[candidate].wstring(), source, width, height))) {
                CommitImage(navigationFiles_[candidate].wstring(), source, width, height, false);
                InvalidateRect(window_, nullptr, FALSE);
                return;
            }
        }
        ClearDeletedImage();
    }

    void DeleteImage() {
        if (currentPath_.empty()) return;
        ComPtr<IShellItem> item;
        ComPtr<IFileOperation> operation;
        HRESULT hr = SHCreateItemFromParsingName(currentPath_.c_str(), nullptr, IID_PPV_ARGS(&item));
        if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));
        if (SUCCEEDED(hr)) hr = operation->SetOwnerWindow(window_);
        if (SUCCEEDED(hr)) hr = operation->SetOperationFlags(FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE);
        if (SUCCEEDED(hr)) hr = operation->DeleteItem(item.Get(), nullptr);
        if (SUCCEEDED(hr)) hr = operation->PerformOperations();
        BOOL aborted = FALSE;
        if (SUCCEEDED(hr)) hr = operation->GetAnyOperationsAborted(&aborted);
        if (FAILED(hr)) { ShowActionError(L"Windows could not move this image to the Recycle Bin."); return; }
        if (!aborted) ShowImageAfterDelete();
    }

    RECT GetDropdownBounds() const {
        RECT client{};
        GetClientRect(window_, &client);
        const UINT dpi = GetDpiForWindow(window_);
        const FrameMetrics frame = GetFrameMetrics(window_);
        const LONG margin = MulDiv(4, dpi, 96);
        const LONG width = std::min<LONG>(MulDiv(236, dpi, 96), std::max<LONG>(1, client.right - margin * 2));
        const LONG height = MulDiv(301, dpi, 96);
        const LONG left = std::clamp<LONG>(frame.hamburger.left + margin, margin,
            std::max<LONG>(margin, client.right - width - margin));
        const LONG top = frame.hamburger.bottom + margin;
        return { left, top, left + width, std::min<LONG>(client.bottom - margin, top + height) };
    }

    HRESULT ReloadCurrentImage() {
        if (IsGifPath(currentPath_)) return LoadImage(currentPath_, false);
        ComPtr<IWICBitmapSource> source;
        UINT width = 0, height = 0;
        const HRESULT hr = DecodeImage(currentPath_, source, width, height);
        if (SUCCEEDED(hr)) {
            CommitImage(currentPath_, source, width, height, false);
            InvalidateRect(window_, nullptr, FALSE);
        } else {
            ShowActionError(L"The image was changed, but Viewtrious could not reload it.");
        }
        return hr;
    }

    bool ReadGifLoopCount(const std::wstring& path, UINT& loopCount) const {
        std::ifstream input(fs::path(path), std::ios::binary);
        std::array<BYTE, 13> header{};
        if (!input.read(reinterpret_cast<char*>(header.data()), header.size()) ||
            std::memcmp(header.data(), "GIF", 3) != 0) return false;
        const auto skipSubBlocks = [&input]() {
            for (;;) { BYTE size = 0; if (!input.read(reinterpret_cast<char*>(&size), 1)) return false; if (!size) return true; input.seekg(size, std::ios::cur); if (!input) return false; }
        };
        if (header[10] & 0x80) input.seekg(3 * (1u << ((header[10] & 0x07) + 1)), std::ios::cur);
        for (;;) {
            BYTE marker = 0; if (!input.read(reinterpret_cast<char*>(&marker), 1)) return false;
            if (marker == 0x3B) return false;
            if (marker == 0x2C) { std::array<BYTE, 9> descriptor{}; if (!input.read(reinterpret_cast<char*>(descriptor.data()), descriptor.size())) return false; const BYTE packed = descriptor[8]; if (packed & 0x80) input.seekg(3 * (1u << ((packed & 7) + 1)), std::ios::cur); BYTE lzw = 0; if (!input.read(reinterpret_cast<char*>(&lzw), 1)) return false; if (!skipSubBlocks()) return false; continue; }
            if (marker != 0x21) return false;
            BYTE label = 0; if (!input.read(reinterpret_cast<char*>(&label), 1)) return false;
            if (label == 0xFF) {
                BYTE size = 0; if (!input.read(reinterpret_cast<char*>(&size), 1) || size != 11) return false;
                std::array<char, 11> application{}; if (!input.read(application.data(), application.size())) return false;
                BYTE dataSize = 0; if (!input.read(reinterpret_cast<char*>(&dataSize), 1)) return false;
                std::array<BYTE, 255> data{}; if (dataSize && !input.read(reinterpret_cast<char*>(data.data()), dataSize)) return false;
                if ((std::memcmp(application.data(), "NETSCAPE2.0", 11) == 0 || std::memcmp(application.data(), "ANIMEXTS1.0", 11) == 0) && dataSize >= 3 && data[0] == 1) {
                    loopCount = static_cast<UINT>(data[1]) | (static_cast<UINT>(data[2]) << 8);
                    return true;
                }
                if (!skipSubBlocks()) return false;
            } else if (label == 0xF9) input.seekg(6, std::ios::cur);
            else if (!skipSubBlocks()) return false;
        }
    }

    void StopGifPlayback() {
        KillTimer(window_, kGifPlaybackTimer);
        gifDecoder_.Reset();
        gifCanvas_.reset();
        gifPreviousCanvas_.reset();
        gifFrameIndex_ = gifFrameCount_ = 0;
        gifCompletedLoops_ = 0;
        gifLoopCount_ = 0; gifHasLoopExtension_ = false;
        gifPlaying_ = gifPaused_ = gifPlaybackTimerActive_ = false;
    }

    void FinishGifPlayback() {
        KillTimer(window_, kGifPlaybackTimer);
        gifDecoder_.Reset();
        gifPreviousCanvas_.reset();
        gifPlaying_ = gifPaused_ = gifPlaybackTimerActive_ = false;
        if (lanczosSelected_) QueueLanczosRefinement();
    }

    void ActivateGifPlayback() {
        if (!window_ || !gifPlaying_ || gifPaused_ || gifPlaybackTimerActive_) return;
        if (SetTimer(window_, kGifPlaybackTimer, gifFrameDelayMs_, nullptr)) gifPlaybackTimerActive_ = true;
    }

    void ApplyGifPreviousDisposal() {
        if (!gifCanvas_) return;
        if (gifPreviousDisposal_ == 3 && gifPreviousCanvas_) {
            *gifCanvas_ = *gifPreviousCanvas_;
        } else if (gifPreviousDisposal_ == 2) {
            const UINT right = std::min(gifCanvasWidth_, gifPreviousLeft_ + gifPreviousWidth_);
            const UINT bottom = std::min(gifCanvasHeight_, gifPreviousTop_ + gifPreviousHeight_);
            for (UINT y = std::min(gifPreviousTop_, gifCanvasHeight_); y < bottom; ++y) {
                BYTE* row = gifCanvas_->data() + (static_cast<size_t>(y) * gifCanvasWidth_ + gifPreviousLeft_) * 4;
                std::memset(row, 0, static_cast<size_t>(right - gifPreviousLeft_) * 4);
            }
        }
        gifPreviousCanvas_.reset();
    }

    HRESULT PresentGifFrame(bool initial, bool resetNavigation = false) {
        if (!gifDecoder_ || !gifCanvas_ || gifFrameIndex_ >= gifFrameCount_) return E_FAIL;
        ApplyGifPreviousDisposal();
        ComPtr<IWICBitmapFrameDecode> frame;
        HRESULT hr = gifDecoder_->GetFrame(gifFrameIndex_, &frame);
        ComPtr<IWICMetadataQueryReader> metadata;
        if (SUCCEEDED(hr)) frame->GetMetadataQueryReader(&metadata);
        const UINT left = GifMetadataUInt(metadata.Get(), L"/imgdesc/Left");
        const UINT top = GifMetadataUInt(metadata.Get(), L"/imgdesc/Top");
        UINT frameWidth = GifMetadataUInt(metadata.Get(), L"/imgdesc/Width");
        UINT frameHeight = GifMetadataUInt(metadata.Get(), L"/imgdesc/Height");
        const UINT disposal = GifMetadataUInt(metadata.Get(), L"/grctlext/Disposal");
        const UINT delayCentiseconds = GifMetadataUInt(metadata.Get(), L"/grctlext/Delay");
        if (SUCCEEDED(hr) && (!frameWidth || !frameHeight)) hr = frame->GetSize(&frameWidth, &frameHeight);
        ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(hr)) hr = wicFactory_->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        UINT decodedWidth = 0, decodedHeight = 0;
        if (SUCCEEDED(hr)) hr = converter->GetSize(&decodedWidth, &decodedHeight);
        if (FAILED(hr)) return hr;

        // Keep at most the logical canvas, one previous-disposal canvas, and this frame.
        const UINT copyWidth = std::min({ frameWidth, decodedWidth, gifCanvasWidth_ > left ? gifCanvasWidth_ - left : 0u });
        const UINT copyHeight = std::min({ frameHeight, decodedHeight, gifCanvasHeight_ > top ? gifCanvasHeight_ - top : 0u });
        if (!copyWidth || !copyHeight) return E_FAIL;
        if (decodedWidth > UINT_MAX / 4 || decodedHeight > UINT_MAX / (decodedWidth * 4)) return E_OUTOFMEMORY;
        const size_t frameBytes = static_cast<size_t>(decodedWidth) * decodedHeight * 4;
        std::vector<BYTE> pixels(frameBytes);
        hr = converter->CopyPixels(nullptr, decodedWidth * 4, static_cast<UINT>(frameBytes), pixels.data());
        if (FAILED(hr)) return hr;
        if (disposal == 3) gifPreviousCanvas_ = std::make_shared<std::vector<BYTE>>(*gifCanvas_);
        for (UINT y = 0; y < copyHeight; ++y) for (UINT x = 0; x < copyWidth; ++x) {
            const BYTE* src = pixels.data() + (static_cast<size_t>(y) * decodedWidth + x) * 4;
            BYTE* dst = gifCanvas_->data() + (static_cast<size_t>(top + y) * gifCanvasWidth_ + left + x) * 4;
            const unsigned alpha = src[3];
            const unsigned inverse = 255 - alpha;
            dst[0] = static_cast<BYTE>(src[0] + (dst[0] * inverse + 127) / 255);
            dst[1] = static_cast<BYTE>(src[1] + (dst[1] * inverse + 127) / 255);
            dst[2] = static_cast<BYTE>(src[2] + (dst[2] * inverse + 127) / 255);
            dst[3] = static_cast<BYTE>(alpha + (dst[3] * inverse + 127) / 255);
        }
        gifPreviousDisposal_ = disposal;
        gifPreviousLeft_ = left; gifPreviousTop_ = top; gifPreviousWidth_ = copyWidth; gifPreviousHeight_ = copyHeight;
        // Clamp zero/near-zero authored delays to 20 ms, matching common viewer behavior without a busy timer.
        gifFrameDelayMs_ = std::max(20u, delayCentiseconds * 10u);

        ComPtr<IWICBitmap> bitmap;
        hr = wicFactory_->CreateBitmapFromMemory(gifCanvasWidth_, gifCanvasHeight_, GUID_WICPixelFormat32bppPBGRA,
            gifCanvasWidth_ * 4, static_cast<UINT>(gifCanvas_->size()), gifCanvas_->data(), &bitmap);
        if (FAILED(hr)) return hr;
        if (initial) {
            committingGifFrame_ = true;
            CommitImage(currentPath_, bitmap, gifCanvasWidth_, gifCanvasHeight_, resetNavigation);
            committingGifFrame_ = false;
        } else {
            InvalidateLanczosVariant(false);
            source_ = bitmap; bitmap_.Reset(); imageWidth_ = gifCanvasWidth_; imageHeight_ = gifCanvasHeight_;
        }
        displayedPixels_ = gifCanvas_;
        InvalidateRect(window_, nullptr, FALSE);
        if (initial) ActivateGifPlayback();
        return S_OK;
    }

    HRESULT LoadAnimatedGif(const std::wstring& path, bool resetNavigation) {
        ComPtr<IWICBitmapDecoder> decoder;
        HRESULT hr = wicFactory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
        UINT frameCount = 0;
        if (SUCCEEDED(hr)) hr = decoder->GetFrameCount(&frameCount);
        if (FAILED(hr) || frameCount <= 1) return FAILED(hr) ? hr : S_FALSE;
        ComPtr<IWICMetadataQueryReader> metadata;
        if (SUCCEEDED(decoder->GetMetadataQueryReader(&metadata))) {
            gifCanvasWidth_ = GifMetadataUInt(metadata.Get(), L"/logscrdesc/Width");
            gifCanvasHeight_ = GifMetadataUInt(metadata.Get(), L"/logscrdesc/Height");
        }
        ComPtr<IWICBitmapFrameDecode> first;
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &first);
        if (SUCCEEDED(hr) && (!gifCanvasWidth_ || !gifCanvasHeight_)) hr = first->GetSize(&gifCanvasWidth_, &gifCanvasHeight_);
        const size_t bytes = static_cast<size_t>(gifCanvasWidth_) * gifCanvasHeight_ * 4;
        if (FAILED(hr) || !gifCanvasWidth_ || !gifCanvasHeight_ || bytes > UINT_MAX) return FAILED(hr) ? hr : E_OUTOFMEMORY;
        gifDecoder_ = decoder; gifFrameCount_ = frameCount; gifFrameIndex_ = 0; gifCompletedLoops_ = 0;
        gifHasLoopExtension_ = ReadGifLoopCount(path, gifLoopCount_);
        gifCanvas_ = std::make_shared<std::vector<BYTE>>(bytes, BYTE{ 0 }); gifPreviousCanvas_.reset();
        gifPreviousDisposal_ = gifPreviousLeft_ = gifPreviousTop_ = gifPreviousWidth_ = gifPreviousHeight_ = 0;
        gifPlaying_ = true; gifPaused_ = !gifVisible_;
        currentPath_ = path;
        hr = PresentGifFrame(true, resetNavigation);
        if (FAILED(hr)) { StopGifPlayback(); return hr; }
        return S_OK;
    }

    void GifPlaybackTimer() {
        if (!gifPlaying_ || gifPaused_) return;
        ++gifFrameIndex_;
        if (gifFrameIndex_ == gifFrameCount_) {
            ++gifCompletedLoops_;
            // NETSCAPE's count is the number of repetitions after the first pass; zero means forever.
            if (gifHasLoopExtension_ && gifLoopCount_ != 0 && gifCompletedLoops_ > gifLoopCount_) { FinishGifPlayback(); return; }
            gifFrameIndex_ = 0;
        }
        if (FAILED(PresentGifFrame(false))) { StopGifPlayback(); return; }
        gifPlaybackTimerActive_ = false;
        ActivateGifPlayback();
    }

    void SetGifPlaybackVisible(bool visible) {
        gifVisible_ = visible;
        if (!gifPlaying_) return;
        if (!visible) { gifPaused_ = true; gifPlaybackTimerActive_ = false; KillTimer(window_, kGifPlaybackTimer); }
        else if (gifPaused_) { gifPaused_ = false; ActivateGifPlayback(); }
    }

    HRESULT DecodeImage(const std::wstring& path, ComPtr<IWICBitmapSource>& source, UINT& width, UINT& height) {
        timer_.Log(L"decode start");
        ComPtr<IWICBitmapDecoder> decoder;
        HRESULT hr = wicFactory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder);
        if (SUCCEEDED(hr)) {
            ComPtr<IWICBitmapFrameDecode> frame;
            hr = decoder->GetFrame(0, &frame);
            if (SUCCEEDED(hr)) {
                hr = frame->GetSize(&width, &height);
                if (SUCCEEDED(hr) && width > 0 && height > 0) {
                    ComPtr<IWICFormatConverter> converter;
                    hr = wicFactory_->CreateFormatConverter(&converter);
                    if (SUCCEEDED(hr)) {
                        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
                    }
                    const UINT orientation = ReadPhotoOrientation(frame.Get());
                    ComPtr<IWICBitmapSource> transformed;
                    if (SUCCEEDED(hr) && orientation != 1) {
                        ComPtr<IWICBitmapFlipRotator> rotator;
                        hr = wicFactory_->CreateBitmapFlipRotator(&rotator);
                        if (SUCCEEDED(hr)) hr = rotator->Initialize(converter.Get(), TransformForOrientation(orientation));
                        if (SUCCEEDED(hr)) transformed = rotator;
                        if (orientation >= 5 && orientation <= 8) std::swap(width, height);
                    } else if (SUCCEEDED(hr)) {
                        transformed = converter;
                    }
                    ComPtr<IWICBitmap> cachedBitmap;
                    if (SUCCEEDED(hr)) hr = wicFactory_->CreateBitmapFromSource(transformed.Get(), WICBitmapCacheOnLoad, &cachedBitmap);
                    if (SUCCEEDED(hr)) source = cachedBitmap;
                } else if (SUCCEEDED(hr)) {
                    hr = E_FAIL;
                }
            }
        }
        timer_.Log(L"decode complete");
        return hr;
    }

    HRESULT DecodeImagePixels(const std::wstring& path, PixelBuffer& decoded) const {
        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        ComPtr<IWICBitmapDecoder> decoder;
        if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
        ComPtr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
        UINT width = 0, height = 0;
        if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
        if (FAILED(hr) || width == 0 || height == 0) return FAILED(hr) ? hr : E_FAIL;
        ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        ComPtr<IWICBitmapSource> transformed = converter;
        const UINT orientation = ReadPhotoOrientation(frame.Get());
        if (SUCCEEDED(hr) && orientation != 1) {
            ComPtr<IWICBitmapFlipRotator> rotator;
            hr = factory->CreateBitmapFlipRotator(&rotator);
            if (SUCCEEDED(hr)) hr = rotator->Initialize(converter.Get(), TransformForOrientation(orientation));
            if (SUCCEEDED(hr)) transformed = rotator;
            if (orientation >= 5 && orientation <= 8) std::swap(width, height);
        }
        if (FAILED(hr)) return hr;
        if (width > UINT_MAX / 4 || height > UINT_MAX / (width * 4)) return E_OUTOFMEMORY;
        const UINT stride = width * 4;
        const size_t bytes = static_cast<size_t>(stride) * height;
        auto pixels = std::make_shared<std::vector<BYTE>>(bytes);
        hr = transformed->CopyPixels(nullptr, stride, static_cast<UINT>(bytes), pixels->data());
        if (SUCCEEDED(hr)) { decoded.width = width; decoded.height = height; decoded.stride = stride; decoded.pixels = std::move(pixels); }
        return hr;
    }

    void QueueLanczosRefinement(UINT delayMs = 120) {
        if (lanczosSelected_ && source_ && !gifPlaying_ && !spaceMouseMotionActive_)
            SetTimer(window_, kLanczosSettleTimer, delayMs, nullptr);
    }

    bool BuildLanczosRequest(LanczosRequest& request) {
        if (!source_ || !lanczosSelected_ || gifPlaying_ || spaceMouseMotionActive_) return false;
        const float dpiScale = RenderTargetDpi() / 96.0f;
        const float physicalScale = PhysicalPixelScale();
        const D2D1_SIZE_F canvas = ImageCanvasSize();
        const D2D1_POINT_2F imageTopLeft = ImageTopLeft(CurrentScale(), canvas);
        const UINT fullWidth = std::max(1u, static_cast<UINT>(std::lround(imageWidth_ * physicalScale)));
        const UINT fullHeight = std::max(1u, static_cast<UINT>(std::lround(imageHeight_ * physicalScale)));
        const int visibleLeft = std::clamp(static_cast<int>(std::floor(-imageTopLeft.x * dpiScale)), 0, static_cast<int>(fullWidth));
        const int visibleTop = std::clamp(static_cast<int>(std::floor(-imageTopLeft.y * dpiScale)), 0, static_cast<int>(fullHeight));
        const int visibleRight = std::clamp(static_cast<int>(std::ceil(canvas.width * dpiScale - imageTopLeft.x * dpiScale)), 0, static_cast<int>(fullWidth));
        const int visibleBottom = std::clamp(static_cast<int>(std::ceil(canvas.height * dpiScale - imageTopLeft.y * dpiScale)), 0, static_cast<int>(fullHeight));
        if (visibleRight <= visibleLeft || visibleBottom <= visibleTop) return false;
        const uint64_t fullArea = static_cast<uint64_t>(fullWidth) * fullHeight;
        const uint64_t canvasArea = static_cast<uint64_t>(std::max(1.0f, canvas.width * dpiScale)) *
            static_cast<uint64_t>(std::max(1.0f, canvas.height * dpiScale));
        const bool wholeImage = fullArea <= canvasArea + canvasArea / 2;
        constexpr int kOverscanPixels = 96;
        const int outputLeft = wholeImage ? 0 : std::max(0, visibleLeft - kOverscanPixels);
        const int outputTop = wholeImage ? 0 : std::max(0, visibleTop - kOverscanPixels);
        const int outputRight = wholeImage ? static_cast<int>(fullWidth) : std::min(static_cast<int>(fullWidth), visibleRight + kOverscanPixels);
        const int outputBottom = wholeImage ? static_cast<int>(fullHeight) : std::min(static_cast<int>(fullHeight), visibleBottom + kOverscanPixels);
        const float sourcePerDestination = 1.0f / physicalScale;
        const float support = 3.0f * std::max(1.0f, sourcePerDestination) + 1.0f;
        const int sourceLeft = wholeImage ? 0 : std::max(0, static_cast<int>(std::floor((outputLeft + 0.5f) * sourcePerDestination - 0.5f - support)));
        const int sourceTop = wholeImage ? 0 : std::max(0, static_cast<int>(std::floor((outputTop + 0.5f) * sourcePerDestination - 0.5f - support)));
        const int sourceRight = wholeImage ? static_cast<int>(imageWidth_) : std::min(static_cast<int>(imageWidth_), static_cast<int>(std::ceil((outputRight - 0.5f) * sourcePerDestination - 0.5f + support + 1.0f)));
        const int sourceBottom = wholeImage ? static_cast<int>(imageHeight_) : std::min(static_cast<int>(imageHeight_), static_cast<int>(std::ceil((outputBottom - 0.5f) * sourcePerDestination - 0.5f + support + 1.0f)));
        if (sourceRight <= sourceLeft || sourceBottom <= sourceTop) return false;
        auto pixels = displayedPixels_;
        if (!pixels) {
            const size_t bytes = static_cast<size_t>(imageWidth_) * imageHeight_ * 4;
            pixels = std::make_shared<std::vector<BYTE>>(bytes);
            if (FAILED(source_->CopyPixels(nullptr, imageWidth_ * 4, static_cast<UINT>(bytes), pixels->data()))) return false;
        }
        auto cropped = std::make_shared<std::vector<BYTE>>(static_cast<size_t>(sourceRight - sourceLeft) * (sourceBottom - sourceTop) * 4);
        for (int row = sourceTop; row < sourceBottom; ++row) std::memcpy(cropped->data() + static_cast<size_t>(row - sourceTop) * (sourceRight - sourceLeft) * 4,
            pixels->data() + (static_cast<size_t>(row) * imageWidth_ + sourceLeft) * 4, static_cast<size_t>(sourceRight - sourceLeft) * 4);
        request.sourcePixels = std::move(cropped);
        request.sourceWidth = static_cast<UINT>(sourceRight - sourceLeft); request.sourceHeight = static_cast<UINT>(sourceBottom - sourceTop);
        request.targetWidth = static_cast<UINT>(outputRight - outputLeft); request.targetHeight = static_cast<UINT>(outputBottom - outputTop);
        request.generation = lanczosGeneration_;
        request.mapping = { static_cast<float>(sourceLeft), static_cast<float>(sourceTop), static_cast<float>(outputLeft), static_cast<float>(outputTop), sourcePerDestination, sourcePerDestination };
        request.destination = D2D1::RectF(imageTopLeft.x + outputLeft / dpiScale, imageTopLeft.y + outputTop / dpiScale,
            imageTopLeft.x + outputRight / dpiScale, imageTopLeft.y + outputBottom / dpiScale);
        return true;
    }

    void RequestLanczosVariant() {
        if (spaceMouseMotionActive_) return;
        LanczosRequest request{};
        if (!BuildLanczosRequest(request)) return;
        if (lanczosRendering_) {
            pendingLanczosRequest_ = std::move(request);
            return;
        }
        StartLanczosRequest(std::move(request));
    }

    void StartLanczosRequest(LanczosRequest request) {
        if (spaceMouseMotionActive_) return;
        if (lanczosThread_.joinable()) lanczosThread_.join();
        lanczosRendering_ = true;
        auto cancellation = std::make_shared<std::atomic_bool>(false);
        lanczosCancellation_ = cancellation;
        TraceSpaceMouseDiagnostic(L"Lanczos job start", request.generation, request.targetWidth, request.targetHeight);
        lanczosThread_ = std::thread([window = window_, request = std::move(request), cancellation = std::move(cancellation)]() mutable {
            auto* result = new LanczosResult{};
            result->generation = request.generation;
            result->width = request.targetWidth;
            result->height = request.targetHeight;
            result->destination = request.destination;
            viewtrious::Lanczos3Scaler scaler;
            if (!cancellation->load(std::memory_order_relaxed) &&
                scaler.Initialize(request.sourceWidth, request.sourceHeight, request.targetWidth, request.targetHeight, request.mapping)) {
                result->pixels = std::make_shared<std::vector<BYTE>>();
                result->succeeded = scaler.Scale(request.sourcePixels->data(), request.sourceWidth * 4, *result->pixels, cancellation.get());
            }
#if defined(_DEBUG)
            LARGE_INTEGER now{}, frequency{};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            wchar_t message[256]{};
            swprintf_s(message, L"Viewtrious SpaceMouse/Lanczos: Lanczos job complete qpc=%lld (%.3f ms) generation=%llu size=%ux%u succeeded=%d\\n",
                now.QuadPart, 1000.0 * static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart),
                static_cast<unsigned long long>(result->generation), result->width, result->height, result->succeeded ? 1 : 0);
            OutputDebugStringW(message);
#endif
            if (!PostMessageW(window, kLanczosCompleteMessage, 0, reinterpret_cast<LPARAM>(result))) delete result;
        });
    }

    void HandleLanczosResult(LanczosResult* result) {
        if (!result) return;
        TraceSpaceMouseDiagnostic(L"Lanczos result received", result->generation, result->width, result->height);
        if (lanczosThread_.joinable()) lanczosThread_.join();
        lanczosRendering_ = false;
        lanczosCancellation_.reset();
        const bool mayPublish = !spaceMouseMotionActive_ && result->generation == lanczosGeneration_ && result->succeeded && result->pixels;
        if (mayPublish) {
            TraceSpaceMouseDiagnostic(L"Lanczos cache publish", result->generation, result->width, result->height);
            lanczosPixels_ = std::move(result->pixels);
            lanczosWidth_ = result->width;
            lanczosHeight_ = result->height;
            lanczosDestination_ = result->destination;
            lanczosViewGeneration_ = result->generation;
            lanczosBitmap_.Reset();
        }
        delete result;
        if (pendingLanczosRequest_) {
            LanczosRequest request = std::move(*pendingLanczosRequest_);
            pendingLanczosRequest_.reset();
            if (!spaceMouseMotionActive_ && request.generation == lanczosGeneration_ && lanczosSelected_)
                StartLanczosRequest(std::move(request));
        }
        if (!spaceMouseMotionActive_ && mayPublish) {
            TraceSpaceMouseDiagnostic(L"Lanczos invalidation requested");
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    bool IsFastNavigationPath(const std::wstring& path) const {
        std::wstring extension = fs::path(path).extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
        return extension == L".jpg" || extension == L".jpeg" || extension == L".png" || extension == L".bmp";
    }

    bool NavigateFastRasterSynchronously(const std::wstring& path, int direction) {
        (void)direction;
        if (!IsFastNavigationPath(path)) return false;

        // This is the accepted pre-0.4.4 path: present each inexpensive raster image
        // before the next queued navigation repeat is allowed to advance the target.
        ++decodeRequestGeneration_;
        pendingFullDecode_.reset();
        imageDecodePending_ = false;
        KillTimer(window_, kNavigationDecodeDebounceTimer);

        ComPtr<IWICBitmapSource> source;
        UINT width = 0;
        UINT height = 0;
        if (FAILED(DecodeImage(path, source, width, height))) return false;

        CommitImage(path, source, width, height, false);
        InvalidateRect(window_, nullptr, FALSE);
        UpdateWindow(window_);
        return true;
    }

    void QueueLatestFullDecode() {
        KillTimer(window_, kNavigationDecodeDebounceTimer);
        if (pendingFullDecode_ && IsFastNavigationPath(pendingFullDecode_->path) && fullDecodeWorkersInFlight_ < 2) {
            StartPendingFullDecode();
        } else {
            SetTimer(window_, kNavigationDecodeDebounceTimer, 60, nullptr);
        }
    }

    void PresentNavigationUpdate(bool immediate) {
        const FrameMetrics frame = GetFrameMetrics(window_);
        RECT client{};
        GetClientRect(window_, &client);
        RECT title{ 0, 0, client.right, frame.titleBarHeight };
        RedrawWindow(window_, &title, nullptr, RDW_INVALIDATE);
        if (immediate) UpdateWindow(window_);
    }

    void SelectNavigationTarget(const std::wstring& path, int direction = 0, bool immediatePaint = true) {
        (void)direction;
        if (path.empty()) return;
        if (IsGifPath(path)) {
            LoadImage(path, false);
            PresentNavigationUpdate(immediatePaint);
            return;
        }
        currentPath_ = path;
        currentFileIdentity_ = ReadFileIdentity(fs::path(path));
        filenameText_ = fs::path(path).filename().wstring();
        fileSizeText_ = FormatFileSize(path);
        resolutionText_.clear();
        error_.clear();
        imageDecodePending_ = true;
        ++decodeRequestGeneration_;
        pendingFullDecode_ = DecodeRequest{ path, decodeRequestGeneration_, navigationFolderGeneration_ };
        QueueLatestFullDecode();
        PresentNavigationUpdate(immediatePaint);
    }

    void StartPendingFullDecode() {
        KillTimer(window_, kNavigationDecodeDebounceTimer);
        if (!pendingFullDecode_ || fullDecodeWorkersInFlight_ >= 2) return;
        DecodeRequest request = *pendingFullDecode_;
        pendingFullDecode_.reset();
        ++fullDecodeWorkersInFlight_;
        fullDecodeThreads_.emplace_back([this, request] {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            auto* result = new FullDecodeResult{};
            result->request = request;
            result->workerId = std::this_thread::get_id();
            result->result = DecodeImagePixels(request.path, *result);
            if (SUCCEEDED(apartment)) CoUninitialize();
            if (request.requestGeneration != decodeRequestGeneration_.load(std::memory_order_acquire)) {
                auto* finished = new DecodeWorkerFinished{ result->workerId };
                delete result;
                if (!PostMessageW(window_, kDecodeWorkerFinishedMessage, 0, reinterpret_cast<LPARAM>(finished))) delete finished;
                return;
            }
            if (IsFastNavigationPath(request.path) && !decodeShuttingDown_.load(std::memory_order_acquire)) {
                result->deliveredSynchronously = true;
                DWORD_PTR ignored = 0;
                if (SendMessageTimeoutW(window_, kFullDecodeCompleteMessage, 0, reinterpret_cast<LPARAM>(result),
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored)) {
                    delete result;
                    auto* finished = new DecodeWorkerFinished{ std::this_thread::get_id() };
                    if (!PostMessageW(window_, kDecodeWorkerFinishedMessage, 0, reinterpret_cast<LPARAM>(finished))) delete finished;
                    return;
                }
                result->deliveredSynchronously = false;
            }
            if (!PostMessageW(window_, kFullDecodeCompleteMessage, 0, reinterpret_cast<LPARAM>(result))) delete result;
        });
    }

    void CompleteFullDecodeWorker(const std::thread::id& workerId) {
        const auto worker = std::find_if(fullDecodeThreads_.begin(), fullDecodeThreads_.end(), [&](const std::thread& thread) { return thread.get_id() == workerId; });
        if (worker != fullDecodeThreads_.end()) { worker->join(); fullDecodeThreads_.erase(worker); }
        fullDecodeWorkersInFlight_ = std::max(0, fullDecodeWorkersInFlight_ - 1);
    }

    void HandleDecodeWorkerFinished(DecodeWorkerFinished* finished) {
        if (!finished) return;
        CompleteFullDecodeWorker(finished->workerId);
        delete finished;
        StartPendingFullDecode();
    }

    void HandleFullDecodeResult(FullDecodeResult* result) {
        if (!result) return;
        if (!result->deliveredSynchronously) CompleteFullDecodeWorker(result->workerId);
        const bool current = result->request.requestGeneration == decodeRequestGeneration_ &&
            result->request.folderGeneration == navigationFolderGeneration_ && PathsEqual(fs::path(result->request.path), fs::path(currentPath_));
        if (current) {
            imageDecodePending_ = false;
            if (SUCCEEDED(result->result) && result->pixels) {
                ComPtr<IWICBitmap> bitmap;
                const HRESULT hr = wicFactory_->CreateBitmapFromMemory(result->width, result->height, GUID_WICPixelFormat32bppPBGRA,
                    result->stride, static_cast<UINT>(result->pixels->size()), result->pixels->data(), &bitmap);
                if (SUCCEEDED(hr)) {
                    CommitImage(result->request.path, bitmap, result->width, result->height, false);
                    displayedPixels_ = result->pixels;
                } else error_ = L"Unable to open this image. It may be corrupt or use an unsupported codec.";
            } else {
                source_.Reset(); bitmap_.Reset(); displayedPixels_.reset(); displayedPath_.clear(); imageWidth_ = imageHeight_ = 0;
                error_ = L"Unable to open this image. It may be corrupt or use an unsupported codec.";
            }
            InvalidateRect(window_, nullptr, FALSE);
            if (result->deliveredSynchronously) UpdateWindow(window_);
        }
        if (!result->deliveredSynchronously) {
            delete result;
            StartPendingFullDecode();
        }
    }

    void CommitImage(const std::wstring& path, const ComPtr<IWICBitmapSource>& source, UINT width, UINT height,
        bool resetNavigation) {
        if (!committingGifFrame_) StopGifPlayback();
        InvalidateLanczosVariant(false);
        displayedPixels_.reset();
        source_ = source;
        bitmap_.Reset();
        imageWidth_ = width;
        imageHeight_ = height;
        currentPath_ = path;
        displayedPath_ = path;
        currentFileIdentity_ = ReadFileIdentity(fs::path(path));
        resolutionText_ = std::to_wstring(width) + L"\u00D7" + std::to_wstring(height);
        fileSizeText_ = FormatFileSize(path);
        filenameText_ = fs::path(path).filename().wstring();
        error_.clear();
        fitToWindow_ = true;
        zoom_ = 1.0f;
        pan_ = D2D1::Point2F();
        EndPan();
        StartDirectoryWatcher(fs::path(path).parent_path());
        if (lanczosSelected_ && !gifPlaying_) QueueLanczosRefinement();
        if (resetNavigation) {
            navigationFiles_.clear();
            navigationBuilt_ = false;
            navigationBuildQueued_ = false;
        }
    }

    void EnsureRenderTarget() {
        if (renderTarget_ || !window_) return;
        std::wstring error;
        if (!graphicsHost_.Create(window_, d2dFactory_.Get(), graphicsAdapterAuto_ ? nullptr : &graphicsAdapterLuid_, error)) { error_ = error; return; }
        renderTarget_ = graphicsHost_.D2DContext();
        if (contentKind_ == ContentKind::Model3D && modelDocument_ && !modelViewport_.Active() &&
            !modelViewport_.Create(graphicsHost_, modelDocument_, error, ModelUpVector())) {
            contentKind_ = ContentKind::None;
            error_ = error;
        }
        if (contentKind_ == ContentKind::Model3D && modelViewport_.Active()) { modelViewport_.SetProjectionMode(modelProjectionMode_); modelViewport_.SetBuildPlate(BuildPlateVisible(), ModelUpVector()); }
        timer_.Log(L"shared graphics/window initialization complete");
    }

    void EnsureBitmap() {
        if (!renderTarget_ || bitmap_) return;
        renderTarget_->CreateBitmapFromWicBitmap(source_.Get(), nullptr, &bitmap_);
    }

    D2D1_SIZE_F ClientSize() const {
        RECT client{};
        GetClientRect(window_, &client);
        return D2D1::SizeF(static_cast<float>(std::max(1L, client.right - client.left)),
            static_cast<float>(std::max(1L, client.bottom - client.top)));
    }

    D2D1_SIZE_F ImageCanvasSize() const {
        return ClientSize();
    }

    float BaseScale() const {
        if (!source_) return 1.0f;
        const D2D1_SIZE_F target = ImageCanvasSize();
        const float fitScale = std::min(target.width / static_cast<float>(imageWidth_),
            target.height / static_cast<float>(imageHeight_));
        return std::min(1.0f, fitScale);
    }

    float CurrentScale() const { return fitToWindow_ ? BaseScale() : std::max(zoom_, BaseScale()); }

    std::pair<UINT, UINT> LanczosTargetSize() const {
        const float physicalScale = PhysicalPixelScale();
        return { std::max(1u, static_cast<UINT>(std::lround(imageWidth_ * physicalScale))),
            std::max(1u, static_cast<UINT>(std::lround(imageHeight_ * physicalScale))) };
    }

    bool LanczosVariantMatchesCurrent() const {
        return lanczosPixels_ && source_ && lanczosViewGeneration_ == lanczosGeneration_;
    }

    void InvalidateLanczosVariant(bool keepSelection) {
        (void)keepSelection;
        if (lanczosCancellation_) lanczosCancellation_->store(true, std::memory_order_relaxed);
        ++lanczosGeneration_;
        KillTimer(window_, kLanczosSettleTimer);
        lanczosPixels_.reset();
        lanczosBitmap_.Reset();
        lanczosWidth_ = lanczosHeight_ = 0;
        pendingLanczosRequest_.reset();
    }

    bool EnsureLanczosBitmap() {
        if (lanczosBitmap_ || !lanczosPixels_ || !renderTarget_) return lanczosBitmap_ != nullptr;
        const float dpi = RenderTargetDpi();
        const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
        return SUCCEEDED(renderTarget_->CreateBitmap(D2D1::SizeU(lanczosWidth_, lanczosHeight_), lanczosPixels_->data(),
            lanczosWidth_ * 4, properties, &lanczosBitmap_));
    }

    D2D1_POINT_2F ImageTopLeft(float scale, const D2D1_SIZE_F& target) const {
        return D2D1::Point2F((target.width - imageWidth_ * scale) / 2.0f + pan_.x,
            (target.height - imageHeight_ * scale) / 2.0f + pan_.y);
    }

    void ClampPan() {
        if (!source_) return;
        const D2D1_SIZE_F canvas = ImageCanvasSize();
        const float scale = CurrentScale();
        const float width = imageWidth_ * scale, height = imageHeight_ * scale;
        const float dipScale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        const float visibleX = std::min(width, 100.0f * dipScale);
        const float visibleY = std::min(height, 100.0f * dipScale);
        const float centeredX = (canvas.width - width) / 2.0f;
        const float centeredY = (canvas.height - height) / 2.0f;
        pan_.x = std::clamp(pan_.x, visibleX - centeredX - width, canvas.width - visibleX - centeredX);
        pan_.y = std::clamp(pan_.y, visibleY - centeredY - height, canvas.height - visibleY - centeredY);
    }

    bool CanPan() const {
        return source_.Get() != nullptr;
    }

    bool DisplayedImageMatchesTarget() const {
        return source_ && !imageDecodePending_ && !displayedPath_.empty() && PathsEqual(fs::path(displayedPath_), fs::path(currentPath_));
    }


    bool EnsureCheckerboardBrush() {
        if (!renderTarget_) return false;
        const UINT dpi = GetDpiForWindow(window_);
        if (checkerboardBrush_ && checkerboardDpi_ == dpi) return true;

        checkerboardBrush_.Reset();
        checkerboardBitmap_.Reset();
        checkerboardDpi_ = dpi;
        const UINT tileSize = static_cast<UINT>(std::max(2, MulDiv(24, dpi, 96))) & ~1u;
        const UINT squareSize = tileSize / 2;
        std::vector<BYTE> pixels(static_cast<size_t>(tileSize) * tileSize * 4);
        for (UINT y = 0; y < tileSize; ++y) for (UINT x = 0; x < tileSize; ++x) {
            const bool light = ((x / squareSize) + (y / squareSize)) % 2 != 0;
            const BYTE color = light ? 29 : 22;
            const size_t offset = (static_cast<size_t>(y) * tileSize + x) * 4;
            pixels[offset] = color;
            pixels[offset + 1] = color;
            pixels[offset + 2] = color;
            pixels[offset + 3] = 255;
        }
        const D2D1_BITMAP_PROPERTIES bitmapProperties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(renderTarget_->CreateBitmap(D2D1::SizeU(tileSize, tileSize), pixels.data(), tileSize * 4,
                bitmapProperties, &checkerboardBitmap_))) return false;
        return SUCCEEDED(renderTarget_->CreateBitmapBrush(checkerboardBitmap_.Get(),
            D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR), &checkerboardBrush_));
    }

    void DrawCheckerboard(const D2D1_RECT_F& bounds) {
        if (!EnsureCheckerboardBrush()) return;
        const D2D1_SIZE_F target = ImageCanvasSize();
        const D2D1_RECT_F visible = D2D1::RectF(std::max(bounds.left, 0.0f), std::max(bounds.top, 0.0f),
            std::min(bounds.right, target.width), std::min(bounds.bottom, target.height));
        if (visible.right <= visible.left || visible.bottom <= visible.top) return;
        renderTarget_->PushAxisAlignedClip(visible, D2D1_ANTIALIAS_MODE_ALIASED);
        renderTarget_->FillRectangle(visible, checkerboardBrush_.Get());
        renderTarget_->PopAxisAlignedClip();
    }
    void DrawImage() {
        const D2D1_SIZE_F target = ImageCanvasSize();
        const float scale = CurrentScale();
        const D2D1_POINT_2F topLeft = ImageTopLeft(scale, target);
        const D2D1_RECT_F destination = D2D1::RectF(topLeft.x, topLeft.y,
            topLeft.x + imageWidth_ * scale, topLeft.y + imageHeight_ * scale);
        DrawCheckerboard(destination);
        if (!gifPlaying_ && !spaceMouseMotionActive_ && lanczosSelected_ && LanczosVariantMatchesCurrent() && EnsureLanczosBitmap())
            renderTarget_->DrawBitmap(lanczosBitmap_.Get(), lanczosDestination_, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        else renderTarget_->DrawBitmap(bitmap_.Get(), destination, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }


    bool EnsureZoomHudFormat() {
        const UINT dpi = GetDpiForWindow(window_);
        if (zoomHudFormat_ && zoomHudDpi_ == dpi) return true;
        zoomHudFormat_.Reset();
        zoomHudDpi_ = 0;
        const float scale = static_cast<float>(dpi) / 96.0f;
        if (FAILED(dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.0f * scale, L"", &zoomHudFormat_))) return false;
        zoomHudFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        zoomHudFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        zoomHudDpi_ = dpi;
        return true;
    }

    void DrawZoomHud() {
        if (!source_ || !showZoomPercentage_ || !EnsureZoomHudFormat()) return;
        wchar_t label[16]{};
        const float percent = PhysicalPixelScale() * 100.0f;
        if (percent < 10.0f) swprintf_s(label, L"%.1f%%", percent);
        else swprintf_s(label, L"%.0f%%", percent);
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        const D2D1_SIZE_F target = renderTarget_->GetSize();
        const float width = 72.0f * scale, height = 30.0f * scale, margin = 14.0f * scale;
        const bool left = zoomHudPosition_ == ZoomHudPosition::BottomLeft || zoomHudPosition_ == ZoomHudPosition::TopLeft;
        const bool top = zoomHudPosition_ == ZoomHudPosition::TopLeft || zoomHudPosition_ == ZoomHudPosition::TopRight;
        const float leftEdge = left ? margin : target.width - margin - width;
        const float canvasTop = fullscreen_ ? 0.0f : static_cast<float>(GetFrameMetrics(window_).titleBarHeight);
        const float topEdge = top ? canvasTop + margin : target.height - margin - height;
        const D2D1_RECT_F bounds = D2D1::RectF(leftEdge, topEdge, leftEdge + width, topEdge + height);
        ComPtr<ID2D1SolidColorBrush> backing, text;
        if (FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.50f), &backing)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.50f), &text))) return;
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(bounds, 6.0f * scale, 6.0f * scale), backing.Get());
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwriteFactory_->CreateTextLayout(label, static_cast<UINT32>(wcslen(label)), zoomHudFormat_.Get(), width, height, &layout)))
            renderTarget_->DrawTextLayout(D2D1::Point2F(bounds.left, bounds.top), layout.Get(), text.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void DrawRevisionLabel() {
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        const D2D1_SIZE_F target = renderTarget_->GetSize();
        const float margin = 14.0f * scale, width = 78.0f * scale, height = 24.0f * scale;
        const D2D1_RECT_F bounds = D2D1::RectF(margin, target.height - margin - height, margin + width,
            target.height - margin);
        ComPtr<ID2D1SolidColorBrush> backing, text;
        if (FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.50f), &backing)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.65f), &text))) return;
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(bounds, 5.0f * scale, 5.0f * scale), backing.Get());
        DrawOverlayText(L"v" VIEWTRIOUS_VERSION, bounds.left, bounds.top, width, height, 11.5f,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, text.Get(), true, false, true);
    }

    void DrawCanvasNavigationButtons() {
        if (!CanvasNavigationButtonsVisible()) return;
        const UINT dpi = GetDpiForWindow(window_);
        const float scale = static_cast<float>(dpi) / 96.0f;
        const auto draw = [&](bool next, float opacity) {
            const RECT bounds = GetCanvasNavigationZoneBounds(next);
            ComPtr<ID2D1SolidColorBrush> chevron;
            if (FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, opacity), &chevron))) return;
            const float centerX = (static_cast<float>(bounds.left) + static_cast<float>(bounds.right)) / 2.0f;
            const float centerY = (static_cast<float>(bounds.top) + static_cast<float>(bounds.bottom)) / 2.0f;
            const float offset = 8.0f * scale, height = 16.0f * scale;
            const float startX = centerX + (next ? -offset : offset);
            const float tipX = centerX + (next ? offset : -offset);
            renderTarget_->DrawLine(D2D1::Point2F(startX, centerY - height), D2D1::Point2F(tipX, centerY), chevron.Get(), 2.4f * scale);
            renderTarget_->DrawLine(D2D1::Point2F(tipX, centerY), D2D1::Point2F(startX, centerY + height), chevron.Get(), 2.4f * scale);
        };
        draw(false, canvasPreviousOpacity_);
        draw(true, canvasNextOpacity_);
    }

    void DrawVideoPlaybackControls() {
        if (!VideoActive() || videoControlsOpacity_ <= 0.001f) return;
        const VideoControlsLayout layout = GetVideoControlsLayout();
        if (layout.island.right <= layout.island.left) return;
        const float opacity = videoControlsOpacity_;
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        const bool dark = UseDarkAppMode();
        ComPtr<ID2D1SolidColorBrush> surface, border, text, accent, track, hover;
        if (FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(dark ? 35.0f / 255.0f : 246.0f / 255.0f, dark ? 38.0f / 255.0f : 246.0f / 255.0f, dark ? 45.0f / 255.0f : 246.0f / 255.0f, 0.94f * opacity), &surface)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(dark ? 78.0f / 255.0f : 180.0f / 255.0f, dark ? 82.0f / 255.0f : 180.0f / 255.0f, dark ? 92.0f / 255.0f : 180.0f / 255.0f, 0.55f * opacity), &border)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(dark ? 242.0f / 255.0f : 35.0f / 255.0f, dark ? 242.0f / 255.0f : 35.0f / 255.0f, dark ? 242.0f / 255.0f : 35.0f / 255.0f, opacity), &text)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 120.0f / 255.0f, 212.0f / 255.0f, opacity), &accent)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(dark ? 100.0f / 255.0f : 170.0f / 255.0f, dark ? 104.0f / 255.0f : 170.0f / 255.0f, dark ? 114.0f / 255.0f : 170.0f / 255.0f, 0.75f * opacity), &track)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(dark ? 66.0f / 255.0f : 224.0f / 255.0f, dark ? 70.0f / 255.0f : 224.0f / 255.0f, dark ? 80.0f / 255.0f : 224.0f / 255.0f, opacity), &hover))) return;

        const auto rect = [](const RECT& value) { return D2D1::RectF(static_cast<float>(value.left), static_cast<float>(value.top), static_cast<float>(value.right), static_cast<float>(value.bottom)); };
        const D2D1_RECT_F island = rect(layout.island);
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(island, 11.0f * scale, 11.0f * scale), surface.Get());
        renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(island, 11.0f * scale, 11.0f * scale), border.Get(), 1.0f * scale);
        if (videoControlsHovered_ == ButtonKind::VideoPlayPause) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect(layout.playPause), 5.0f * scale, 5.0f * scale), hover.Get());
        if (videoControlsHovered_ == ButtonKind::VideoMute) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect(layout.mute), 5.0f * scale, 5.0f * scale), hover.Get());

        const float playCenterX = (layout.playPause.left + layout.playPause.right) * 0.5f;
        const float playCenterY = (layout.playPause.top + layout.playPause.bottom) * 0.5f;
        if (videoPlayer_.Playing()) {
            const float barWidth = 3.0f * scale, barHeight = 13.0f * scale, gap = 3.0f * scale;
            renderTarget_->FillRectangle(D2D1::RectF(playCenterX - gap - barWidth, playCenterY - barHeight * 0.5f, playCenterX - gap, playCenterY + barHeight * 0.5f), text.Get());
            renderTarget_->FillRectangle(D2D1::RectF(playCenterX + gap, playCenterY - barHeight * 0.5f, playCenterX + gap + barWidth, playCenterY + barHeight * 0.5f), text.Get());
        } else {
            ComPtr<ID2D1PathGeometry> triangle;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&triangle)) && SUCCEEDED(triangle->Open(&sink))) {
                const float half = 7.0f * scale;
                sink->BeginFigure(D2D1::Point2F(playCenterX - half * 0.55f, playCenterY - half), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(playCenterX - half * 0.55f, playCenterY + half));
                sink->AddLine(D2D1::Point2F(playCenterX + half, playCenterY));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                renderTarget_->FillGeometry(triangle.Get(), text.Get());
            }
        }

        double current = 0.0, duration = 0.0;
        const bool hasTimes = videoPlayer_.GetPlaybackTimes(current, duration);
        if (videoScrubbing_ && hasTimes) current = videoScrubSeconds_;
        DrawOverlayText(hasTimes ? FormatVideoTime(current).c_str() : L"--:--", static_cast<float>(layout.currentTime.left), static_cast<float>(layout.currentTime.top), static_cast<float>(layout.currentTime.right - layout.currentTime.left), static_cast<float>(layout.currentTime.bottom - layout.currentTime.top), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, text.Get(), true, false, true);
        DrawOverlayText(hasTimes ? FormatVideoTime(duration).c_str() : L"--:--", static_cast<float>(layout.duration.left), static_cast<float>(layout.duration.top), static_cast<float>(layout.duration.right - layout.duration.left), static_cast<float>(layout.duration.bottom - layout.duration.top), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, text.Get(), true, false, true);

        const float trackCenter = (layout.scrubber.top + layout.scrubber.bottom) * 0.5f;
        const bool scrubberHot = VideoScrubberContains(lastMousePoint_) || videoScrubbing_;
        const float trackHeight = (scrubberHot ? 6.0f : 4.0f) * scale;
        const D2D1_RECT_F trackBounds = D2D1::RectF(static_cast<float>(layout.scrubber.left), trackCenter - trackHeight * 0.5f, static_cast<float>(layout.scrubber.right), trackCenter + trackHeight * 0.5f);
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(trackBounds, trackHeight * 0.5f, trackHeight * 0.5f), track.Get());
        const float progress = hasTimes && duration > 0.0 ? std::clamp(static_cast<float>(current / duration), 0.0f, 1.0f) : 0.0f;
        const float progressX = trackBounds.left + (trackBounds.right - trackBounds.left) * progress;
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(trackBounds.left, trackBounds.top, progressX, trackBounds.bottom), trackHeight * 0.5f, trackHeight * 0.5f), accent.Get());
        if (scrubberHot) renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(progressX, trackCenter), 4.0f * scale, 4.0f * scale), accent.Get());

        const float muteCenterX = (layout.mute.left + layout.mute.right) * 0.5f;
        const float muteCenterY = (layout.mute.top + layout.mute.bottom) * 0.5f;
        const float speaker = 5.0f * scale;
        renderTarget_->FillRectangle(D2D1::RectF(muteCenterX - speaker, muteCenterY - speaker * 0.45f, muteCenterX - speaker * 0.35f, muteCenterY + speaker * 0.45f), text.Get());
        renderTarget_->DrawLine(D2D1::Point2F(muteCenterX - speaker * 0.35f, muteCenterY - speaker * 0.45f), D2D1::Point2F(muteCenterX + speaker * 0.55f, muteCenterY - speaker), text.Get(), 1.6f * scale);
        renderTarget_->DrawLine(D2D1::Point2F(muteCenterX + speaker * 0.55f, muteCenterY - speaker), D2D1::Point2F(muteCenterX + speaker * 0.55f, muteCenterY + speaker), text.Get(), 1.6f * scale);
        renderTarget_->DrawLine(D2D1::Point2F(muteCenterX + speaker * 0.55f, muteCenterY + speaker), D2D1::Point2F(muteCenterX - speaker * 0.35f, muteCenterY + speaker * 0.45f), text.Get(), 1.6f * scale);
        if (videoPlayer_.Muted()) {
            renderTarget_->DrawLine(D2D1::Point2F(muteCenterX + speaker, muteCenterY - speaker), D2D1::Point2F(muteCenterX + speaker * 2.0f, muteCenterY + speaker), accent.Get(), 1.8f * scale);
            renderTarget_->DrawLine(D2D1::Point2F(muteCenterX + speaker * 2.0f, muteCenterY - speaker), D2D1::Point2F(muteCenterX + speaker, muteCenterY + speaker), accent.Get(), 1.8f * scale);
        } else {
            renderTarget_->DrawLine(D2D1::Point2F(muteCenterX + speaker, muteCenterY - speaker * 0.75f), D2D1::Point2F(muteCenterX + speaker * 1.55f, muteCenterY - speaker * 0.35f), text.Get(), 1.4f * scale);
            renderTarget_->DrawLine(D2D1::Point2F(muteCenterX + speaker * 1.55f, muteCenterY - speaker * 0.35f), D2D1::Point2F(muteCenterX + speaker * 1.55f, muteCenterY + speaker * 0.35f), text.Get(), 1.4f * scale);
            renderTarget_->DrawLine(D2D1::Point2F(muteCenterX + speaker * 1.55f, muteCenterY + speaker * 0.35f), D2D1::Point2F(muteCenterX + speaker, muteCenterY + speaker * 0.75f), text.Get(), 1.4f * scale);
        }
    }

    bool EnsureTitleTextFormat() {
        const UINT dpi = GetDpiForWindow(window_);
        if (titleTextFormat_ && titleTextDpi_ == dpi) return true;
        titleTextFormat_.Reset();
        titleTextDpi_ = dpi;
        if (FAILED(dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f * static_cast<float>(dpi) / 96.0f,
                L"", &titleTextFormat_))) return false;
        titleTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    }

    bool HasSystemFontFamily(const wchar_t* familyName) const {
        ComPtr<IDWriteFontCollection> fonts;
        if (FAILED(dwriteFactory_->GetSystemFontCollection(&fonts))) return false;
        UINT32 index = 0;
        BOOL exists = FALSE;
        return SUCCEEDED(fonts->FindFamilyName(familyName, &index, &exists)) && exists;
    }

    bool EnsureCaptionIconFormat() {
        const UINT dpi = GetDpiForWindow(window_);
        if (captionIconFormat_ && captionIconDpi_ == dpi) return true;
        captionIconFormat_.Reset();
        captionIconDpi_ = dpi;
        const wchar_t* const familyName = HasSystemFontFamily(L"Segoe Fluent Icons")
            ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
        if (FAILED(dwriteFactory_->CreateTextFormat(familyName, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * static_cast<float>(dpi) / 96.0f,
                L"", &captionIconFormat_))) return false;
        captionIconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        captionIconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        captionIconFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    }

    void DrawCaptionGlyph(wchar_t glyph, const RECT& bounds, ID2D1Brush* brush) {
        if (!EnsureCaptionIconFormat()) return;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(&glyph, 1, captionIconFormat_.Get(),
                static_cast<float>(bounds.right - bounds.left), static_cast<float>(bounds.bottom - bounds.top), &layout))) return;
        renderTarget_->DrawTextLayout(D2D1::Point2F(static_cast<float>(bounds.left), static_cast<float>(bounds.top)),
            layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void DrawMenuGlyph(wchar_t glyph, float left, float top, float width, float height, ID2D1Brush* brush, bool mirror = false) {
        const wchar_t* const familyName = HasSystemFontFamily(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
        ComPtr<IDWriteTextFormat> format;
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        if (FAILED(dwriteFactory_->CreateTextFormat(familyName, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 16.0f * scale, L"", &format))) return;
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwriteFactory_->CreateTextLayout(&glyph, 1, format.Get(), width, height, &layout))) {
            D2D1_MATRIX_3X2_F transform{};
            renderTarget_->GetTransform(&transform);
            if (mirror) renderTarget_->SetTransform(D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, D2D1::Point2F(left + width / 2.0f, top + height / 2.0f)) * transform);
            renderTarget_->DrawTextLayout(D2D1::Point2F(left, top), layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (mirror) renderTarget_->SetTransform(transform);
        }
    }

    void DrawTitleText(const std::wstring& text, float left, float width, ID2D1Brush* brush, bool trim, bool center) {
        if (text.empty() || width <= 0.0f || !EnsureTitleTextFormat()) return;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), titleTextFormat_.Get(),
                width, static_cast<float>(GetFrameMetrics(window_).titleBarHeight), &layout))) return;
        if (trim) {
            DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(titleTextFormat_.Get(), &ellipsis))) {
                layout->SetTrimming(&trimming, ellipsis.Get());
            }
        }
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        const float top = std::max(0.0f, (static_cast<float>(GetFrameMetrics(window_).titleBarHeight) - metrics.height) / 2.0f);
        const float textLeft = center ? left + std::max(0.0f, (width - metrics.width) / 2.0f) : left;
        renderTarget_->DrawTextLayout(D2D1::Point2F(textLeft, top), layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    static std::wstring FormatExactTriangleCount(uint64_t count) {
        std::wstring text = std::to_wstring(count);
        for (size_t position = text.size(); position > 3; position -= 3) text.insert(position - 3, 1, L',');
        return text;
    }

    static std::wstring FormatCompactTriangleCount(uint64_t count) {
        if (count < 1000) return std::to_wstring(count);
        uint64_t unit = count >= 1000000 ? 1000000 : 1000;
        wchar_t suffix = unit == 1000000 ? L'M' : L'k';
        double value = static_cast<double>(count) / static_cast<double>(unit);
        int decimals = value >= 100.0 ? 0 : value >= 10.0 ? 1 : 2;
        double scale = decimals == 2 ? 100.0 : decimals == 1 ? 10.0 : 1.0;
        uint64_t scaled = static_cast<uint64_t>(std::llround(value * scale));
        if (unit == 1000 && scaled >= 1000 * static_cast<uint64_t>(scale)) {
            unit = 1000000;
            suffix = L'M';
            value = static_cast<double>(count) / static_cast<double>(unit);
            decimals = value >= 100.0 ? 0 : value >= 10.0 ? 1 : 2;
            scale = decimals == 2 ? 100.0 : decimals == 1 ? 10.0 : 1.0;
            scaled = static_cast<uint64_t>(std::llround(value * scale));
        }
        std::wstring text = std::to_wstring(scaled);
        if (decimals != 0) {
            while (text.size() <= static_cast<size_t>(decimals)) text.insert(text.begin(), L'0');
            text.insert(text.size() - static_cast<size_t>(decimals), 1, L'.');
            while (!text.empty() && text.back() == L'0') text.pop_back();
            if (!text.empty() && text.back() == L'.') text.pop_back();
        }
        return text + suffix;
    }

    bool TriangleCountTooltipAvailable() const {
        return !fullscreen_ && ModelActive() && modelTriangleCount_ != 0 && !tutorialPresentation_ &&
            !HasOverlay() && !dropdownOpen_ && !contextMenuOpen_;
    }

    RECT TriangleCountTitleTextBounds() {
        if (!ModelActive() || modelTriangleCount_ == 0 || !EnsureTitleTextFormat()) return {};
        const FrameMetrics frame = GetFrameMetrics(window_);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(resolutionText_.c_str(), static_cast<UINT32>(resolutionText_.size()),
                titleTextFormat_.Get(), static_cast<float>(frame.resolutionWidth), static_cast<float>(frame.titleBarHeight), &layout))) return {};
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        const float left = static_cast<float>(frame.resolutionLeft) + std::max(0.0f, (static_cast<float>(frame.resolutionWidth) - metrics.width) / 2.0f);
        const float top = std::max(0.0f, (static_cast<float>(frame.titleBarHeight) - metrics.height) / 2.0f);
        return { static_cast<LONG>(std::floor(left)), static_cast<LONG>(std::floor(top)),
            static_cast<LONG>(std::ceil(left + metrics.width)), static_cast<LONG>(std::ceil(top + metrics.height)) };
    }

    void DismissTriangleCountTooltip(bool invalidate = true) {
        KillTimer(window_, kTriangleCountTooltipTimer);
        const bool visible = triangleCountTooltipVisible_;
        triangleCountTooltipHovering_ = false;
        triangleCountTooltipVisible_ = false;
        if (visible && invalidate) InvalidateRect(window_, nullptr, FALSE);
    }

    void DrawTriangleCountTooltip() {
        if (!triangleCountTooltipVisible_ || !TriangleCountTooltipAvailable()) return;
        const RECT titleBounds = TriangleCountTitleTextBounds();
        if (titleBounds.right <= titleBounds.left || titleBounds.bottom <= titleBounds.top) return;
        const std::wstring text = FormatExactTriangleCount(modelTriangleCount_) + L" triangles";
        const UINT dpi = GetDpiForWindow(window_);
        const float scale = static_cast<float>(dpi) / 96.0f;
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f * scale, L"", &format))) return;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), 4096.0f, 4096.0f, &layout))) return;
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        const float horizontalPadding = 12.0f * scale;
        const float tooltipWidth = metrics.width + horizontalPadding * 2.0f;
        const float tooltipHeight = 30.0f * scale;
        layout->SetMaxWidth(tooltipWidth);
        layout->SetMaxHeight(tooltipHeight);
        RECT client{};
        GetClientRect(window_, &client);
        const float margin = 6.0f * scale;
        float left = (static_cast<float>(titleBounds.left + titleBounds.right) - tooltipWidth) / 2.0f;
        left = std::clamp(left, margin, std::max(margin, static_cast<float>(client.right) - tooltipWidth - margin));
        float top = static_cast<float>(GetFrameMetrics(window_).titleBarHeight) + margin;
        if (top + tooltipHeight > static_cast<float>(client.bottom) - margin) top = std::max(margin, static_cast<float>(titleBounds.top) - tooltipHeight - margin);
        const D2D1_RECT_F bounds = D2D1::RectF(left, top, left + tooltipWidth, top + tooltipHeight);
        const bool dark = UseDarkAppMode();
        ComPtr<ID2D1SolidColorBrush> shadow, surface, border, foreground;
        if (FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, dark ? 0.32f : 0.18f), &shadow)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(40.0f / 255.0f, 43.0f / 255.0f, 50.0f / 255.0f) : D2D1::ColorF(250.0f / 255.0f, 250.0f / 255.0f, 250.0f / 255.0f), &surface)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(82.0f / 255.0f, 86.0f / 255.0f, 96.0f / 255.0f) : D2D1::ColorF(190.0f / 255.0f, 190.0f / 255.0f, 190.0f / 255.0f), &border)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(D2D1::ColorF::White) : D2D1::ColorF(28.0f / 255.0f, 28.0f / 255.0f, 28.0f / 255.0f), &foreground))) return;
        const float radius = 7.0f * scale;
        const D2D1_RECT_F shadowBounds = D2D1::RectF(bounds.left, bounds.top + 2.0f * scale, bounds.right, bounds.bottom + 2.0f * scale);
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(shadowBounds, radius, radius), shadow.Get());
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(bounds, radius, radius), surface.Get());
        renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(bounds, radius, radius), border.Get(), 1.0f);
        renderTarget_->DrawTextLayout(D2D1::Point2F(bounds.left, bounds.top), layout.Get(), foreground.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    int GetShortcutRowHeight() const {
        RECT client{};
        GetClientRect(window_, &client);
        const UINT dpi = GetDpiForWindow(window_);
        const int top = fullscreen_ ? 0 : GetFrameMetrics(window_).titleBarHeight;
        const int availableHeight = std::max(1L, client.bottom - top - MulDiv(24, dpi, 96));
        const int normalRow = MulDiv(25, dpi, 96);
        const int compactRow = MulDiv(20, dpi, 96);
        const int fixedHeight = MulDiv(86, dpi, 96);
        return fixedHeight + static_cast<int>(kShortcutEntryCount) * normalRow <= availableHeight ? normalRow : compactRow;
    }

    RECT GetOverlayBounds() const {
        RECT client{};
        GetClientRect(window_, &client);
        if (!HasOverlay()) return {};
        const UINT dpi = GetDpiForWindow(window_);
        const int panelPadding = MulDiv(24, dpi, 96);
        const int titleHeight = MulDiv(24, dpi, 96);
        const int titleGap = MulDiv(14, dpi, 96);
        const int rowHeight = GetShortcutRowHeight();
        const int desiredWidth = MulDiv(overlay_ == OverlayKind::KeyboardShortcuts ? 460 :
            overlay_ == OverlayKind::Settings ? 760 : overlay_ == OverlayKind::ResetConfirm ? 500 : overlay_ == OverlayKind::DeleteConfirm ? 540 :
            overlay_ == OverlayKind::Welcome ? 640 : overlay_ == OverlayKind::DefaultAppsHelper ? 560 : overlay_ == OverlayKind::Feedback ? 440 : 608, dpi, 96);
        int desiredHeight = overlay_ == OverlayKind::KeyboardShortcuts
            ? panelPadding + titleHeight + titleGap + static_cast<int>(kShortcutEntryCount) * rowHeight + panelPadding
            : overlay_ == OverlayKind::Settings ? MulDiv(680, dpi, 96) : overlay_ == OverlayKind::ResetConfirm ? MulDiv(236, dpi, 96) : overlay_ == OverlayKind::DeleteConfirm ? MulDiv(268, dpi, 96) :
            overlay_ == OverlayKind::Welcome ? MulDiv(300, dpi, 96) : overlay_ == OverlayKind::DefaultAppsHelper ? MulDiv(344, dpi, 96) : overlay_ == OverlayKind::Feedback ? MulDiv(330, dpi, 96) : MulDiv(319, dpi, 96);
        const int top = fullscreen_ ? 0 : GetFrameMetrics(window_).titleBarHeight;
        const int availableWidth = std::max(1L, client.right - client.left - MulDiv(24, dpi, 96));
        const int availableHeight = std::max(1L, client.bottom - top - MulDiv(24, dpi, 96));
        const int width = std::min(desiredWidth, availableWidth);
        const int height = std::min(desiredHeight, availableHeight);
        const int left = (client.right - width) / 2;
        const int overlayTop = top + std::max(0L, (client.bottom - top - height) / 2);
        return { left, overlayTop, left + width, overlayTop + height };
    }

    void DrawOverlayText(const wchar_t* text, float x, float y, float width, float height, float size,
        DWRITE_FONT_WEIGHT weight, ID2D1Brush* brush, bool verticallyCenter = false, bool rightAlign = false,
        bool centerAlign = false, bool wrap = false, DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL) {
        ComPtr<IDWriteTextFormat> format;
        const float dpiScale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        if (FAILED(dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, weight, style,
                DWRITE_FONT_STRETCH_NORMAL, size * dpiScale, L"", &format))) return;
        format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (verticallyCenter) format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (rightAlign) format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        else if (centerAlign) format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)), format.Get(), width, height, &layout))) return;
        renderTarget_->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void DrawDropdownChevron(const D2D1_RECT_F& bounds, ID2D1Brush* brush, float dpiScale) {
        const float centerX = bounds.right - (static_cast<float>(kDropdownChevronReserveDips) * 0.5f) * dpiScale;
        const float centerY = (bounds.top + bounds.bottom) * 0.5f;
        const float halfWidth = static_cast<float>(kDropdownChevronHalfWidthDips) * dpiScale;
        const float halfHeight = static_cast<float>(kDropdownChevronHalfHeightDips) * dpiScale;
        renderTarget_->DrawLine(D2D1::Point2F(centerX - halfWidth, centerY - halfHeight), D2D1::Point2F(centerX, centerY + halfHeight), brush, 1.5f * dpiScale);
        renderTarget_->DrawLine(D2D1::Point2F(centerX, centerY + halfHeight), D2D1::Point2F(centerX + halfWidth, centerY - halfHeight), brush, 1.5f * dpiScale);
    }

    bool CreateBitmapFromResource(int resourceId, UINT targetWidth, UINT targetHeight, ComPtr<ID2D1Bitmap>& target) {
        const HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
        if (!resource) return false;
        const DWORD size = SizeofResource(nullptr, resource);
        const HGLOBAL loadedResource = LoadResource(nullptr, resource);
        const void* bytes = loadedResource ? LockResource(loadedResource) : nullptr;
        if (!bytes || size == 0) return false;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IWICBitmapSource> source;
        HRESULT hr = wicFactory_->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<void*>(bytes)), size);
        if (SUCCEEDED(hr)) hr = wicFactory_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
        if (SUCCEEDED(hr)) hr = wicFactory_->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr)) source = converter;
        if (SUCCEEDED(hr) && targetWidth && targetHeight) {
            hr = wicFactory_->CreateBitmapScaler(&scaler);
            if (SUCCEEDED(hr)) hr = scaler->Initialize(source.Get(), targetWidth, targetHeight, WICBitmapInterpolationModeFant);
            if (SUCCEEDED(hr)) source = scaler;
        }
        if (FAILED(hr)) return false;
        return SUCCEEDED(renderTarget_->CreateBitmapFromWicBitmap(source.Get(), nullptr, &target));
    }

    bool EnsureAboutLogo(UINT width, UINT height) {
        if (aboutLogo_ && aboutLogoWidth_ == width && aboutLogoHeight_ == height) return true;
        aboutLogo_.Reset();
        aboutLogoWidth_ = 0;
        aboutLogoHeight_ = 0;
        if (!CreateBitmapFromResource(kAboutLogoResourceId, width, height, aboutLogo_)) return false;
        aboutLogoWidth_ = width;
        aboutLogoHeight_ = height;
        return true;
    }

    bool EnsureTopBarLogo(UINT width, UINT height) {
        if (topBarLogo_ && topBarLogoWidth_ == width && topBarLogoHeight_ == height) return true;
        topBarLogo_.Reset();
        topBarLogoWidth_ = 0;
        topBarLogoHeight_ = 0;
        if (!CreateBitmapFromResource(kTopBarLogoResourceId, width, height, topBarLogo_)) return false;
        topBarLogoWidth_ = width;
        topBarLogoHeight_ = height;
        return true;
    }

    RECT GetEmptyOpenFileButtonBounds() const {
        RECT client{};
        GetClientRect(window_, &client);
        const UINT dpi = GetDpiForWindow(window_);
        const int width = MulDiv(132, dpi, 96);
        const int height = MulDiv(38, dpi, 96);
        const int groupTop = (fullscreen_ ? 0 : GetFrameMetrics(window_).titleBarHeight) +
            std::max(0L, (client.bottom - (fullscreen_ ? 0 : GetFrameMetrics(window_).titleBarHeight) - MulDiv(86, dpi, 96)) / 2);
        const int top = groupTop + MulDiv(48, dpi, 96);
        const int left = (client.right - width) / 2;
        return { left, top, left + width, top + height };
    }

    void DrawEmptyState() {
        if (!EmptyStateActive() || HasOverlay()) return;
        const bool dark = UseDarkAppMode();
        ComPtr<ID2D1SolidColorBrush> primary, secondary, button, buttonHover, buttonPressed, buttonText;
        if (FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(D2D1::ColorF::White) : D2D1::ColorF(30.f/255,30.f/255,30.f/255), &primary)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(205.f/255,208.f/255,214.f/255) : D2D1::ColorF(78.f/255,78.f/255,78.f/255), &secondary)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f/255,120.f/255,212.f/255), &button)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f/255,139.f/255,244.f/255), &buttonHover)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f/255,94.f/255,168.f/255), &buttonPressed)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &buttonText))) return;
        const UINT dpi = GetDpiForWindow(window_);
        const float scale = static_cast<float>(dpi) / 96.0f;
        const D2D1_SIZE_F target = ClientSize();
        const float top = fullscreen_ ? 0.0f : static_cast<float>(GetFrameMetrics(window_).titleBarHeight);
        const float groupTop = top + std::max(0.0f, (target.height - top - 86.0f * scale) / 2.0f);
        DrawOverlayText(error_.empty() ? L"Drag and drop an image here or open a file" : error_.c_str(), 24.0f * scale,
            groupTop, target.width - 48.0f * scale, 24.0f * scale, 16.0f,
            DWRITE_FONT_WEIGHT_NORMAL, secondary.Get(), true, false, true);
        const RECT buttonBounds = GetEmptyOpenFileButtonBounds();
        const D2D1_RECT_F buttonRect = D2D1::RectF(static_cast<float>(buttonBounds.left), static_cast<float>(buttonBounds.top),
            static_cast<float>(buttonBounds.right), static_cast<float>(buttonBounds.bottom));
        ID2D1Brush* buttonBrush = pressedButton_ == ButtonKind::EmptyOpenFile ? buttonPressed.Get() :
            hoveredButton_ == ButtonKind::EmptyOpenFile ? buttonHover.Get() : button.Get();
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(buttonRect, 5.0f * scale, 5.0f * scale), buttonBrush);
        DrawOverlayText(L"Open File", buttonRect.left, buttonRect.top, buttonRect.right - buttonRect.left,
            buttonRect.bottom - buttonRect.top, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, buttonText.Get(), true, false, true);
    }

    void DrawOverlay() {
        if (!HasOverlay()) return;
        const RECT bounds = GetOverlayBounds();
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
        const bool dark = UseDarkAppMode();
        const D2D1_COLOR_F veil = D2D1::ColorF(0.0f, 0.0f, 0.0f, dark ? 0.38f : 0.18f);
        const D2D1_COLOR_F panel = dark ? D2D1::ColorF(40.0f / 255.0f, 43.0f / 255.0f, 50.0f / 255.0f)
            : D2D1::ColorF(250.0f / 255.0f, 250.0f / 255.0f, 250.0f / 255.0f);
        const D2D1_COLOR_F border = dark ? D2D1::ColorF(82.0f / 255.0f, 86.0f / 255.0f, 96.0f / 255.0f)
            : D2D1::ColorF(190.0f / 255.0f, 190.0f / 255.0f, 190.0f / 255.0f);
        const D2D1_COLOR_F primary = dark ? D2D1::ColorF(D2D1::ColorF::White) : D2D1::ColorF(28.0f / 255.0f, 28.0f / 255.0f, 28.0f / 255.0f);
        const D2D1_COLOR_F secondary = dark ? D2D1::ColorF(205.0f / 255.0f, 208.0f / 255.0f, 214.0f / 255.0f)
            : D2D1::ColorF(78.0f / 255.0f, 78.0f / 255.0f, 78.0f / 255.0f);
        ComPtr<ID2D1SolidColorBrush> veilBrush, panelBrush, borderBrush, primaryBrush, secondaryBrush;
        if (FAILED(renderTarget_->CreateSolidColorBrush(veil, &veilBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(panel, &panelBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(border, &borderBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(primary, &primaryBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(secondary, &secondaryBrush))) return;
        const D2D1_SIZE_F size = renderTarget_->GetSize();
        const float top = fullscreen_ ? 0.0f : static_cast<float>(GetFrameMetrics(window_).titleBarHeight);
        renderTarget_->FillRectangle(D2D1::RectF(0, top, size.width, size.height), veilBrush.Get());
        const D2D1_RECT_F panelRect = D2D1::RectF(static_cast<float>(bounds.left), static_cast<float>(bounds.top),
            static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(panelRect, 8.0f, 8.0f), panelBrush.Get());
        renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(panelRect, 8.0f, 8.0f), borderBrush.Get(), 1.0f);
        const float dpiScale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        const float panelPadding = 24.0f * dpiScale;
        const float left = static_cast<float>(bounds.left) + panelPadding;
        const float contentWidth = static_cast<float>(bounds.right - bounds.left) - panelPadding * 2.0f;
        if (overlay_ == OverlayKind::Welcome) {
            const UINT logoWidth = static_cast<UINT>(std::max(1.0f, std::round(std::min(216.0f * dpiScale, contentWidth))));
            const UINT logoHeight = static_cast<UINT>(std::max(1.0f, std::round(static_cast<float>(logoWidth) * 577.0f / 2375.0f)));
            if (EnsureAboutLogo(logoWidth, logoHeight)) {
                const float logoLeft = std::round(static_cast<float>(bounds.left) + (static_cast<float>(bounds.right - bounds.left) - logoWidth) * 0.5f);
                const float logoTop = std::round(static_cast<float>(bounds.top) + 24.0f * dpiScale);
                renderTarget_->DrawBitmap(aboutLogo_.Get(), D2D1::RectF(logoLeft, logoTop, logoLeft + logoWidth, logoTop + logoHeight));
            }
            DrawOverlayText(L"Make Viewtrious the default for common image formats?", left, static_cast<float>(bounds.top) + 136.0f * dpiScale,
                contentWidth, 26.0f * dpiScale, 19.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), false, false, true);
            DrawOverlayText(L"Windows will open Default Apps so you can choose which image formats Viewtrious should open.", left,
                static_cast<float>(bounds.top) + 170.0f * dpiScale, contentWidth, 44.0f * dpiScale,
                16.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), false, false, true, true);
            const RECT secondaryBounds = GetWelcomeButtonBounds(false), primaryBounds = GetWelcomeButtonBounds(true);
            const D2D1_RECT_F secondaryButton = D2D1::RectF(static_cast<float>(secondaryBounds.left), static_cast<float>(secondaryBounds.top),
                static_cast<float>(secondaryBounds.right), static_cast<float>(secondaryBounds.bottom));
            const D2D1_RECT_F primaryButton = D2D1::RectF(static_cast<float>(primaryBounds.left), static_cast<float>(primaryBounds.top),
                static_cast<float>(primaryBounds.right), static_cast<float>(primaryBounds.bottom));
            ComPtr<ID2D1SolidColorBrush> accent, accentHover, accentPressed, neutralHover, neutralPressed, buttonText;
            if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f / 255, 120.f / 255, 212.f / 255), &accent)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f / 255, 139.f / 255, 244.f / 255), &accentHover)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f / 255, 94.f / 255, 168.f / 255), &accentPressed)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(60.f / 255, 64.f / 255, 74.f / 255), &neutralHover)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(75.f / 255, 80.f / 255, 92.f / 255), &neutralPressed)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &buttonText))) {
                ID2D1Brush* primaryButtonBrush = pressedButton_ == ButtonKind::WelcomePrimary ? accentPressed.Get() :
                    hoveredButton_ == ButtonKind::WelcomePrimary ? accentHover.Get() : accent.Get();
                renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(primaryButton, 5.0f * dpiScale, 5.0f * dpiScale), primaryButtonBrush);
                if (pressedButton_ == ButtonKind::WelcomeSecondary) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(secondaryButton, 5.0f * dpiScale, 5.0f * dpiScale), neutralPressed.Get());
                else if (hoveredButton_ == ButtonKind::WelcomeSecondary) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(secondaryButton, 5.0f * dpiScale, 5.0f * dpiScale), neutralHover.Get());
                DrawOverlayText(L"Choose defaults", primaryButton.left, primaryButton.top,
                    primaryButton.right - primaryButton.left, primaryButton.bottom - primaryButton.top, 16.0f,
                    DWRITE_FONT_WEIGHT_SEMI_BOLD, buttonText.Get(), true, false, true);
            }
            renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(secondaryButton, 5.0f * dpiScale, 5.0f * dpiScale), borderBrush.Get(), 1.0f);
            DrawOverlayText(L"Not now", secondaryButton.left, secondaryButton.top,
                secondaryButton.right - secondaryButton.left, secondaryButton.bottom - secondaryButton.top, 16.0f,
                DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), true, false, true);
        } else if (overlay_ == OverlayKind::DefaultAppsHelper) {
            DrawOverlayText(L"Set up Viewtrious", left, static_cast<float>(bounds.top) + 28.0f * dpiScale,
                contentWidth, 32.0f * dpiScale, 24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
            DrawOverlayText(L"Choose Viewtrious for the image formats you want to open.", left,
                static_cast<float>(bounds.top) + 88.0f * dpiScale, contentWidth, 28.0f * dpiScale,
                16.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get());
            DrawOverlayText(L"Close Windows Settings when you are finished.", left,
                static_cast<float>(bounds.top) + 134.0f * dpiScale, contentWidth, 28.0f * dpiScale,
                16.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get());
            const RECT cancelBounds = GetDefaultAppsHelperButtonBounds(false), openBounds = GetDefaultAppsHelperButtonBounds(true);
            const D2D1_RECT_F cancel = D2D1::RectF(static_cast<float>(cancelBounds.left), static_cast<float>(cancelBounds.top), static_cast<float>(cancelBounds.right), static_cast<float>(cancelBounds.bottom));
            const D2D1_RECT_F open = D2D1::RectF(static_cast<float>(openBounds.left), static_cast<float>(openBounds.top), static_cast<float>(openBounds.right), static_cast<float>(openBounds.bottom));
            ComPtr<ID2D1SolidColorBrush> accent, accentHover, accentPressed, neutralHover, neutralPressed, buttonText;
            if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f / 255, 120.f / 255, 212.f / 255), &accent)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f / 255, 139.f / 255, 244.f / 255), &accentHover)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f / 255, 94.f / 255, 168.f / 255), &accentPressed)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(60.f / 255, 64.f / 255, 74.f / 255), &neutralHover)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(75.f / 255, 80.f / 255, 92.f / 255), &neutralPressed)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &buttonText))) {
                ID2D1Brush* openBrush = pressedButton_ == ButtonKind::DefaultAppsHelperOpen ? accentPressed.Get() :
                    hoveredButton_ == ButtonKind::DefaultAppsHelperOpen ? accentHover.Get() : accent.Get();
                renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(open, 5.0f * dpiScale, 5.0f * dpiScale), openBrush);
                if (pressedButton_ == ButtonKind::DefaultAppsHelperCancel) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(cancel, 5.0f * dpiScale, 5.0f * dpiScale), neutralPressed.Get());
                else if (hoveredButton_ == ButtonKind::DefaultAppsHelperCancel) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(cancel, 5.0f * dpiScale, 5.0f * dpiScale), neutralHover.Get());
                DrawOverlayText(L"Open Windows Settings", open.left, open.top, open.right - open.left, open.bottom - open.top, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, buttonText.Get(), true, false, true);
            }
            renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(cancel, 5.0f * dpiScale, 5.0f * dpiScale), borderBrush.Get(), 1.0f);
            DrawOverlayText(L"Cancel", cancel.left, cancel.top, cancel.right - cancel.left, cancel.bottom - cancel.top, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), true, false, true);
        } else if (overlay_ == OverlayKind::KeyboardShortcuts) {
            DrawOverlayText(L"Keyboard Shortcuts", left, static_cast<float>(bounds.top) + panelPadding,
                contentWidth, 24.0f * dpiScale, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
            const float shortcutWidth = 154.0f * dpiScale;
            float y = static_cast<float>(bounds.top) + panelPadding + 38.0f * dpiScale;
            const float shortcutRowHeight = static_cast<float>(GetShortcutRowHeight());
            for (const ShortcutEntry& line : kShortcutEntries) {
                DrawOverlayText(line.shortcut, left, y, shortcutWidth, 18.0f * dpiScale, shortcutRowHeight < 22.0f * dpiScale ? 11.0f : 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
                DrawOverlayText(line.description, left + shortcutWidth, y, contentWidth - shortcutWidth,
                    18.0f * dpiScale, shortcutRowHeight < 22.0f * dpiScale ? 11.0f : 12.5f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get());
                y += shortcutRowHeight;
            }
        } else if (overlay_ == OverlayKind::Settings) {
            const float settingsLeft = static_cast<float>(SettingsContentLeft());
            const float settingsWidth = static_cast<float>(bounds.right) - settingsLeft - 18.0f * dpiScale;
            DrawOverlayText(L"settings", settingsLeft, static_cast<float>(bounds.top) + 18.0f * dpiScale,
                settingsWidth, 32.0f * dpiScale, 24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
            const auto group = [&](const wchar_t* label, float top) {
                DrawOverlayText(label, settingsLeft, static_cast<float>(bounds.top) + top * dpiScale, settingsWidth,
                    20.0f * dpiScale, 15.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
            };
            ComPtr<ID2D1SolidColorBrush> accent, checkmark, rowHover, segmentIdle, segmentHover;
            if (FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f / 255, 120.f / 255, 212.f / 255), &accent)) ||
                FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &checkmark)) ||
                FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(60.f / 255, 64.f / 255, 74.f / 255) : D2D1::ColorF(228.f / 255, 228.f / 255, 228.f / 255), &rowHover)) ||
                FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(50.f / 255, 54.f / 255, 63.f / 255) : D2D1::ColorF(238.f / 255, 238.f / 255, 238.f / 255), &segmentIdle)) ||
                FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(65.f / 255, 69.f / 255, 80.f / 255) : D2D1::ColorF(220.f / 255, 220.f / 255, 220.f / 255), &segmentHover))) return;
            const auto drawNavigation = [&](SettingsPage page, ButtonKind button, const wchar_t* label) {
                const RECT navigationBounds = GetSettingsNavigationBounds(page);
                const D2D1_RECT_F navigation = D2D1::RectF(static_cast<float>(navigationBounds.left), static_cast<float>(navigationBounds.top), static_cast<float>(navigationBounds.right), static_cast<float>(navigationBounds.bottom));
                const bool selected = settingsPage_ == page;
                if (selected) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(navigation, 4.0f * dpiScale, 4.0f * dpiScale), accent.Get());
                else if (hoveredButton_ == button || pressedButton_ == button) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(navigation, 4.0f * dpiScale, 4.0f * dpiScale), rowHover.Get());
                DrawOverlayText(label, navigation.left + 10.0f * dpiScale, navigation.top, navigation.right - navigation.left - 20.0f * dpiScale,
                    navigation.bottom - navigation.top, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, selected ? checkmark.Get() : primaryBrush.Get(), true);
            };
            drawNavigation(SettingsPage::General, ButtonKind::SettingsGeneralPage, L"GENERAL");
            drawNavigation(SettingsPage::Image2D, ButtonKind::SettingsImage2DPage, L"2D SETTINGS");
            drawNavigation(SettingsPage::Model3D, ButtonKind::SettingsModel3DPage, L"3D SETTINGS");
            const float dividerX = static_cast<float>(bounds.left) + 194.0f * dpiScale;
            renderTarget_->DrawLine(D2D1::Point2F(dividerX, static_cast<float>(bounds.top) + 58.0f * dpiScale),
                D2D1::Point2F(dividerX, static_cast<float>(bounds.bottom) - 18.0f * dpiScale), borderBrush.Get(), 1.0f);
            const auto drawToggle = [&](int index, ButtonKind button, const wchar_t* label, bool checked, bool enabled = true) {
                const RECT rowBounds = GetSettingsOptionBounds(index);
                const D2D1_RECT_F row = D2D1::RectF(static_cast<float>(rowBounds.left), static_cast<float>(rowBounds.top), static_cast<float>(rowBounds.right), static_cast<float>(rowBounds.bottom));
                if (enabled && (hoveredButton_ == button || pressedButton_ == button)) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(row, 4.0f * dpiScale, 4.0f * dpiScale), rowHover.Get());
                const float boxSize = 18.0f * dpiScale;
                const D2D1_RECT_F checkbox = D2D1::RectF(row.left, row.top + (row.bottom - row.top - boxSize) / 2.0f, row.left + boxSize, row.top + (row.bottom - row.top - boxSize) / 2.0f + boxSize);
                if (checked) {
                    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(checkbox, 3.0f * dpiScale, 3.0f * dpiScale), accent.Get());
                    const float stroke = std::max(1.5f, 2.0f * dpiScale);
                    const D2D1_POINT_2F start = D2D1::Point2F(checkbox.left + boxSize * 0.22f, checkbox.top + boxSize * 0.53f);
                    const D2D1_POINT_2F middle = D2D1::Point2F(checkbox.left + boxSize * 0.43f, checkbox.top + boxSize * 0.74f);
                    const D2D1_POINT_2F end = D2D1::Point2F(checkbox.left + boxSize * 0.78f, checkbox.top + boxSize * 0.30f);
                    renderTarget_->DrawLine(start, middle, checkmark.Get(), stroke);
                    renderTarget_->DrawLine(middle, end, checkmark.Get(), stroke);
                } else renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(checkbox, 3.0f * dpiScale, 3.0f * dpiScale), borderBrush.Get(), 1.0f);
                DrawOverlayText(label, checkbox.right + 12.0f * dpiScale, row.top, row.right - checkbox.right - 12.0f * dpiScale,
                    row.bottom - row.top, 16.0f, DWRITE_FONT_WEIGHT_NORMAL, enabled ? secondaryBrush.Get() : borderBrush.Get(), true, false, false, true);
            };
            const D2D1_RECT_F settingsViewport = D2D1::RectF(static_cast<float>(bounds.left),
                static_cast<float>(bounds.top) + 60.0f * dpiScale, static_cast<float>(bounds.right),
                static_cast<float>(bounds.bottom) - 18.0f * dpiScale);
            renderTarget_->PushAxisAlignedClip(settingsViewport, D2D1_ANTIALIAS_MODE_ALIASED);
            renderTarget_->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -settingsScroll_));
            const auto drawForegroundMenu = [&](RECT menu, const std::vector<const wchar_t*>& items, int selected, const std::vector<ButtonKind>& buttons, int hoveredItem = -1) {
                const D2D1_RECT_F r=D2D1::RectF((float)menu.left,(float)menu.top,(float)menu.right,(float)menu.bottom);
                renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(r,4*dpiScale,4*dpiScale),panelBrush.Get());
                renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(r,4*dpiScale,4*dpiScale),borderBrush.Get(),1);
                const int row=MulDiv(30,GetDpiForWindow(window_),96);
                for(int i=0;i<(int)items.size();++i) { const D2D1_RECT_F item=D2D1::RectF((float)menu.left,(float)(menu.top+i*row),(float)menu.right,(float)(menu.top+(i+1)*row)); const bool active=i==selected, hover=(i<(int)buttons.size()&&hoveredButton_==buttons[i])||i==hoveredItem; if(active)renderTarget_->FillRectangle(item,accent.Get()); else if(hover)renderTarget_->FillRectangle(item,rowHover.Get()); DrawOverlayText(items[i],item.left+kDropdownLeftPaddingDips*dpiScale,item.top,item.right-item.left-kDropdownLeftPaddingDips*dpiScale,item.bottom-item.top,13,DWRITE_FONT_WEIGHT_NORMAL,active?checkmark.Get():primaryBrush.Get(),true); }
            };
            if (settingsPage_ == SettingsPage::General) {
            const RECT confirmBounds = GetSettingsOptionBounds(2);
            const float appearanceTop = static_cast<float>(confirmBounds.bottom - bounds.top + SettingsSectionGap()) / dpiScale;
            const RECT themeBounds = GetSettingsThemeBounds(ThemePreference::System);
            const float defaultTypesTop = static_cast<float>(themeBounds.bottom - bounds.top + SettingsSectionGap()) / dpiScale;
            const RECT defaultAppsLayoutBounds = GetSettingsDefaultAppsButtonBounds();
            const float resetTop = static_cast<float>(defaultAppsLayoutBounds.bottom - bounds.top + SettingsSectionGap()) / dpiScale;
            group(L"GENERAL", 76.0f);
            drawToggle(0, ButtonKind::SettingsRememberPlacement, L"remember application position and size", rememberWindowPlacement_);
            drawToggle(2, ButtonKind::SettingsConfirmDelete, L"confirm before deleting images", confirmBeforeDeleting_);
            group(L"THEME", appearanceTop);
            DrawOverlayText(L"", settingsLeft, static_cast<float>(bounds.top) + (appearanceTop + 28.0f) * dpiScale, settingsWidth,
                22.0f * dpiScale, 16.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get());
            const auto drawTheme = [&](ThemePreference preference, ButtonKind button, const wchar_t* label) {
                const RECT segmentBounds = GetSettingsThemeBounds(preference);
                const D2D1_RECT_F segment = D2D1::RectF(static_cast<float>(segmentBounds.left), static_cast<float>(segmentBounds.top), static_cast<float>(segmentBounds.right), static_cast<float>(segmentBounds.bottom));
                ID2D1Brush* fill = themePreference_ == preference ? accent.Get() : (hoveredButton_ == button || pressedButton_ == button ? segmentHover.Get() : segmentIdle.Get());
                renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(segment, 4.0f * dpiScale, 4.0f * dpiScale), fill);
                renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(segment, 4.0f * dpiScale, 4.0f * dpiScale), themePreference_ == preference ? accent.Get() : borderBrush.Get(), 1.0f);
                DrawOverlayText(label, segment.left, segment.top, segment.right - segment.left, segment.bottom - segment.top,
                    14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, themePreference_ == preference ? checkmark.Get() : primaryBrush.Get(), true, false, true);
            };
            drawTheme(ThemePreference::System, ButtonKind::SettingsThemeSystem, L"system");
            drawTheme(ThemePreference::Light, ButtonKind::SettingsThemeLight, L"light");
            drawTheme(ThemePreference::Dark, ButtonKind::SettingsThemeDark, L"dark");
            group(L"DEFAULT FILE TYPES", defaultTypesTop);
            const int defaultTypesDescriptionHeight = MeasureSettingsTextHeight(L"choose which image types open with viewtrious", static_cast<int>(settingsWidth), 16.0f, DWRITE_FONT_WEIGHT_NORMAL);
            DrawOverlayText(L"choose which image types open with viewtrious", settingsLeft,
                static_cast<float>(bounds.top) + (defaultTypesTop + 22.0f) * dpiScale, settingsWidth, static_cast<float>(defaultTypesDescriptionHeight),
                16.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), false, false, false, true);
            const RECT defaultAppsBounds = GetSettingsDefaultAppsButtonBounds();
            const D2D1_RECT_F defaultAppsButton = D2D1::RectF(static_cast<float>(defaultAppsBounds.left), static_cast<float>(defaultAppsBounds.top),
                static_cast<float>(defaultAppsBounds.right), static_cast<float>(defaultAppsBounds.bottom));
            if (pressedButton_ == ButtonKind::SettingsDefaultApps) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(defaultAppsButton, 5.0f * dpiScale, 5.0f * dpiScale), segmentHover.Get());
            else if (hoveredButton_ == ButtonKind::SettingsDefaultApps) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(defaultAppsButton, 5.0f * dpiScale, 5.0f * dpiScale), rowHover.Get());
            renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(defaultAppsButton, 5.0f * dpiScale, 5.0f * dpiScale), borderBrush.Get(), 1.0f);
            DrawOverlayText(L"change file type defaults", defaultAppsButton.left, defaultAppsButton.top, defaultAppsButton.right - defaultAppsButton.left, defaultAppsButton.bottom - defaultAppsButton.top, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), true, false, true);
            group(L"RESET VIEWTRIOUS", resetTop);
            const int resetDescriptionHeight = MeasureSettingsTextHeight(L"removes preferences and app-owned data", static_cast<int>(settingsWidth), 16.0f, DWRITE_FONT_WEIGHT_NORMAL);
            DrawOverlayText(L"removes preferences and app-owned data", settingsLeft,
                static_cast<float>(bounds.top) + (resetTop + 22.0f) * dpiScale, settingsWidth, static_cast<float>(resetDescriptionHeight), 16.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), false, false, false, true);
            const RECT resetBounds = GetSettingsResetButtonBounds();
            const D2D1_RECT_F resetButton = D2D1::RectF(static_cast<float>(resetBounds.left), static_cast<float>(resetBounds.top), static_cast<float>(resetBounds.right), static_cast<float>(resetBounds.bottom));
            if (pressedButton_ == ButtonKind::SettingsReset) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(resetButton, 5.0f * dpiScale, 5.0f * dpiScale), segmentHover.Get());
            else if (hoveredButton_ == ButtonKind::SettingsReset) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(resetButton, 5.0f * dpiScale, 5.0f * dpiScale), rowHover.Get());
            renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(resetButton, 5.0f * dpiScale, 5.0f * dpiScale), borderBrush.Get(), 1.0f);
            DrawOverlayText(L"reset", resetButton.left, resetButton.top, resetButton.right - resetButton.left, resetButton.bottom - resetButton.top, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), true, false, true);
            } else if (settingsPage_ == SettingsPage::Image2D) {
            group(L"2D VIEWER", 76.0f);
            drawToggle(1, ButtonKind::SettingsIncludeHidden, L"include hidden images in folder", includeHiddenImages_);
            drawToggle(4, ButtonKind::SettingsAnimations, L"animations and face effects", animationsEnabled_);
            drawToggle(5, ButtonKind::SettingsReverseWheelZoom, L"reverse mouse wheel zoom direction", reverseMouseWheelZoom_);
            const auto drawImageDropdown = [&](RECT control, ButtonKind button, const wchar_t* value, bool open) { const D2D1_RECT_F r=D2D1::RectF((float)control.left,(float)control.top,(float)control.right,(float)control.bottom); renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(r,4*dpiScale,4*dpiScale),((button != ButtonKind::None && hoveredButton_==button)||open)?segmentHover.Get():segmentIdle.Get()); renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(r,4*dpiScale,4*dpiScale),borderBrush.Get(),1); DrawOverlayText(value,r.left+kDropdownLeftPaddingDips*dpiScale,r.top,r.right-r.left-(kDropdownLeftPaddingDips+kDropdownChevronReserveDips)*dpiScale,r.bottom-r.top,14,DWRITE_FONT_WEIGHT_SEMI_BOLD,primaryBrush.Get(),true); DrawDropdownChevron(r,primaryBrush.Get(),dpiScale); };
            const RECT scalingBounds=GetSettingsScalingBounds(ImageScaling::Quality); const int scalingLabelHeight=MeasureSettingsTextHeight(L"image scaling",static_cast<int>(settingsWidth),16,DWRITE_FONT_WEIGHT_NORMAL); DrawOverlayText(L"image scaling",settingsLeft,(float)scalingBounds.top-scalingLabelHeight-kSettingsLabelToControlGapDips*dpiScale,settingsWidth,(float)scalingLabelHeight,16,DWRITE_FONT_WEIGHT_NORMAL,secondaryBrush.Get(),false,false,false,true);
            const auto drawScalingButton = [&](ImageScaling value, ButtonKind button, const wchar_t* text) { const RECT control=GetSettingsScalingBounds(value); const D2D1_RECT_F r=D2D1::RectF((float)control.left,(float)control.top,(float)control.right,(float)control.bottom); const bool selected=imageScaling_==value; renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(r,4*dpiScale,4*dpiScale),selected?accent.Get():(hoveredButton_==button?segmentHover.Get():segmentIdle.Get())); renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(r,4*dpiScale,4*dpiScale),selected?accent.Get():borderBrush.Get(),1); DrawOverlayText(text,r.left,r.top,r.right-r.left,r.bottom-r.top,14,DWRITE_FONT_WEIGHT_SEMI_BOLD,selected?checkmark.Get():primaryBrush.Get(),true,false,true); };
            drawScalingButton(ImageScaling::Quality,ButtonKind::SettingsScalingQuality,L"quality"); drawScalingButton(ImageScaling::Performance,ButtonKind::SettingsScalingPerformance,L"performance");
            const RECT zoomHudBounds=GetSettingsZoomHudBounds(); const int zoomLabelHeight=MeasureSettingsTextHeight(L"show zoom percentage",static_cast<int>(settingsWidth),16,DWRITE_FONT_WEIGHT_NORMAL); const wchar_t* zoomHudPositionLabel=zoomHudPosition_==ZoomHudPosition::BottomLeft?L"bottom left":zoomHudPosition_==ZoomHudPosition::BottomRight?L"bottom right":zoomHudPosition_==ZoomHudPosition::TopLeft?L"top left":L"top right"; DrawOverlayText(L"show zoom percentage",settingsLeft,(float)zoomHudBounds.top-zoomLabelHeight-kSettingsLabelToControlGapDips*dpiScale,settingsWidth,(float)zoomLabelHeight,16,DWRITE_FONT_WEIGHT_NORMAL,secondaryBrush.Get(),false,false,false,true); drawImageDropdown(zoomHudBounds,ButtonKind::SettingsZoomHudPositionToggle,zoomHudPositionLabel,zoomHudPositionMenuOpen_);
            } else {
            const auto label = [&](const wchar_t* text, int column, float top) { const RECT cell=GetSettingsGridCell(column, top); const int height=MeasureSettingsTextHeight(text,cell.right-cell.left,16,DWRITE_FONT_WEIGHT_NORMAL); DrawOverlayText(text,static_cast<float>(cell.left),static_cast<float>(cell.top-height-MulDiv(static_cast<int>(kSettingsLabelToControlGapDips),GetDpiForWindow(window_),96)),static_cast<float>(cell.right-cell.left),static_cast<float>(height),16,DWRITE_FONT_WEIGHT_NORMAL,secondaryBrush.Get(),false,false,false,true); };
            const auto drawDropdown = [&](RECT control, ButtonKind button, const wchar_t* text, bool open) { const D2D1_RECT_F r=D2D1::RectF((float)control.left,(float)control.top,(float)control.right,(float)control.bottom); renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(r,4*dpiScale,4*dpiScale),((button != ButtonKind::None && hoveredButton_==button)||open)?segmentHover.Get():segmentIdle.Get()); renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(r,4*dpiScale,4*dpiScale),borderBrush.Get(),1); DrawOverlayText(text,r.left+kDropdownLeftPaddingDips*dpiScale,r.top,r.right-r.left-(kDropdownLeftPaddingDips+kDropdownChevronReserveDips)*dpiScale,r.bottom-r.top,14,DWRITE_FONT_WEIGHT_SEMI_BOLD,primaryBrush.Get(),true); DrawDropdownChevron(r,primaryBrush.Get(),dpiScale); };
            const auto drawMenu = [&](RECT menu, const std::vector<const wchar_t*>& items, int selected) { const D2D1_RECT_F r=D2D1::RectF((float)menu.left,(float)menu.top,(float)menu.right,(float)menu.bottom); renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(r,4*dpiScale,4*dpiScale),panelBrush.Get()); renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(r,4*dpiScale,4*dpiScale),borderBrush.Get(),1); const int row=MulDiv(30,GetDpiForWindow(window_),96); for(int i=0;i<(int)items.size();++i){const D2D1_RECT_F item=D2D1::RectF((float)menu.left,(float)(menu.top+i*row),(float)menu.right,(float)(menu.top+(i+1)*row));if(i==selected)renderTarget_->FillRectangle(item,accent.Get());DrawOverlayText(items[i],item.left+kDropdownLeftPaddingDips*dpiScale,item.top,item.right-item.left-kDropdownLeftPaddingDips*dpiScale,item.bottom-item.top,13,DWRITE_FONT_WEIGHT_NORMAL,i==selected?checkmark.Get():primaryBrush.Get(),true); } };
            group(L"VIEW", static_cast<float>(SettingsViewHeadingTop() - bounds.top) / dpiScale);
            label(L"up axis",0,static_cast<float>(SettingsViewFirstControlTop() - bounds.top) / dpiScale); drawDropdown(GetSettingsUpAxisBounds(),ButtonKind::SettingsUpAxisToggle,modelUpAxis_==ModelUpAxis::ZUp?L"z axis up":modelUpAxis_==ModelUpAxis::YUp?L"y axis up":L"x axis up",upAxisMenuOpen_);
            label(L"build plate",1,static_cast<float>(SettingsViewFirstControlTop() - bounds.top) / dpiScale); drawDropdown(GetSettingsBuildPlateBounds(),ButtonKind::SettingsBuildPlateToggle,modelBuildPlate_==ModelBuildPlate::Auto?L"auto":modelBuildPlate_==ModelBuildPlate::On?L"on":L"off",buildPlateMenuOpen_);
            label(L"projection",0,static_cast<float>(GetSettingsProjectionBounds().top - bounds.top) / dpiScale); drawDropdown(GetSettingsProjectionBounds(),ButtonKind::SettingsProjectionToggle,modelProjectionMode_==ModelProjectionMode::Perspective?L"perspective":L"orthographic",projectionMenuOpen_);
            label(L"axis indicator position",1,static_cast<float>(GetSettingsAxisIndicatorPositionBounds().top - bounds.top) / dpiScale); const wchar_t* axisPositionLabel=axisIndicatorPosition_==AxisIndicatorPosition::BottomLeft?L"bottom left":axisIndicatorPosition_==AxisIndicatorPosition::BottomRight?L"bottom right":axisIndicatorPosition_==AxisIndicatorPosition::TopLeft?L"top left":L"top right"; drawDropdown(GetSettingsAxisIndicatorPositionBounds(),ButtonKind::SettingsAxisIndicatorPositionToggle,axisPositionLabel,axisIndicatorPositionMenuOpen_);
            if(upAxisMenuOpen_)drawMenu(GetSettingsUpAxisMenuBounds(),{L"z axis up",L"y axis up",L"x axis up"},modelUpAxis_==ModelUpAxis::ZUp?0:modelUpAxis_==ModelUpAxis::YUp?1:2);
            if(buildPlateMenuOpen_)drawMenu(GetSettingsBuildPlateMenuBounds(),{L"auto",L"on",L"off"},static_cast<int>(modelBuildPlate_));
            if(axisIndicatorPositionMenuOpen_)drawMenu(GetSettingsAxisIndicatorPositionMenuBounds(),{L"bottom left",L"bottom right",L"top left",L"top right"},static_cast<int>(axisIndicatorPosition_));
            if(projectionMenuOpen_)drawMenu(GetSettingsProjectionMenuBounds(),{L"perspective",L"orthographic"},static_cast<int>(modelProjectionMode_));
            group(L"RENDER", static_cast<float>(GetSettingsRenderHeadingTop() - bounds.top) / dpiScale);
            label(L"graphics adapter",0,static_cast<float>(GetSettingsRenderControlTop() - bounds.top) / dpiScale); drawDropdown(GetSettingsGraphicsAdapterBounds(),ButtonKind::SettingsGraphicsAdapterToggle,GraphicsAdapterLabel().c_str(),graphicsAdapterMenuOpen_);
            label(L"anti-aliasing",1,static_cast<float>(GetSettingsRenderControlTop() - bounds.top) / dpiScale); const bool fallback=modelVisualStyle_==ModelVisualStyle::Wireframe&&IsModelAntiAliasingSsaa(modelAntiAliasing_); const ModelAntiAliasing displayed=EffectiveModelAntiAliasing(modelAntiAliasing_,modelVisualStyle_); const wchar_t* aaLabel=fallback?L"8x MSAA (Wireframe fallback)":displayed==ModelAntiAliasing::Off?L"Off":displayed==ModelAntiAliasing::Msaa2x?L"2x MSAA":displayed==ModelAntiAliasing::Msaa4x?L"4x MSAA":displayed==ModelAntiAliasing::Msaa8x?L"8x MSAA":displayed==ModelAntiAliasing::Ssaa1_5x?L"1.5x SSAA":L"2x SSAA"; drawDropdown(GetSettingsAntiAliasingBounds(),ButtonKind::SettingsAntiAliasingToggle,aaLabel,antiAliasingMenuOpen_);
            const RECT adapter=GetSettingsGraphicsAdapterBounds(); const int activeLabelWidth=MulDiv(46,GetDpiForWindow(window_),96); int adapterInfoTop=adapter.bottom+MulDiv(6,GetDpiForWindow(window_),96); const int activeHeight=std::max(MulDiv(18,GetDpiForWindow(window_),96),MeasureSettingsTextHeight(graphicsHost_.ActiveAdapterName().empty()?L"Unavailable":graphicsHost_.ActiveAdapterName().c_str(),adapter.right-adapter.left-activeLabelWidth,12,DWRITE_FONT_WEIGHT_SEMI_BOLD)); DrawOverlayText(L"active",(float)adapter.left,(float)adapterInfoTop,(float)activeLabelWidth,(float)activeHeight,12,DWRITE_FONT_WEIGHT_NORMAL,secondaryBrush.Get()); DrawOverlayText(graphicsHost_.ActiveAdapterName().empty()?L"Unavailable":graphicsHost_.ActiveAdapterName().c_str(),(float)adapter.left+activeLabelWidth,(float)adapterInfoTop,(float)(adapter.right-adapter.left-activeLabelWidth),(float)activeHeight,12,DWRITE_FONT_WEIGHT_SEMI_BOLD,primaryBrush.Get(),false,false,false,true); adapterInfoTop+=activeHeight; const int apiHeight=MeasureSettingsTextHeight(L"rendering API    Direct3D 11",adapter.right-adapter.left,12,DWRITE_FONT_WEIGHT_NORMAL); const int apiLabelWidth=activeLabelWidth+MeasureSettingsTextWidth(L"rendering API",12,DWRITE_FONT_WEIGHT_NORMAL)-MeasureSettingsTextWidth(L"active",12,DWRITE_FONT_WEIGHT_NORMAL); DrawOverlayText(L"rendering API",(float)adapter.left,(float)adapterInfoTop,(float)apiLabelWidth,(float)apiHeight,12,DWRITE_FONT_WEIGHT_NORMAL,secondaryBrush.Get()); DrawOverlayText(L"Direct3D 11",(float)adapter.left+apiLabelWidth,(float)adapterInfoTop,(float)(adapter.right-adapter.left-apiLabelWidth),(float)apiHeight,12,DWRITE_FONT_WEIGHT_SEMI_BOLD,primaryBrush.Get()); adapterInfoTop+=apiHeight; const int restartHeight=MeasureSettingsTextHeight(L"restart to apply GPU selection changes",adapter.right-adapter.left,12,DWRITE_FONT_WEIGHT_NORMAL); DrawOverlayText(L"restart to apply GPU selection changes",(float)adapter.left,(float)adapterInfoTop,(float)(adapter.right-adapter.left),(float)restartHeight,12,DWRITE_FONT_WEIGHT_NORMAL,secondaryBrush.Get(),false,false,false,true,DWRITE_FONT_STYLE_ITALIC);
            if(graphicsAdapterMenuOpen_){ const RECT menu=GetSettingsGraphicsAdapterMenuBounds(); std::vector<const wchar_t*> items{L"Auto (High Performance)"}; int selectedAdapter=graphicsAdapterAuto_?0:-1; for(int i=0;i<(int)graphicsAdapters_.size();++i){items.push_back(graphicsAdapters_[i].name.c_str()); if(!graphicsAdapterAuto_&&SameGraphicsAdapterLuid(graphicsAdapters_[i].luid,graphicsAdapterLuid_))selectedAdapter=i+1;} drawMenu(menu,items,selectedAdapter); }
            if(antiAliasingMenuOpen_){ const RECT menu=GetSettingsAntiAliasingMenuBounds(); std::vector<const wchar_t*> items(kAntiAliasingOptions.begin(),kAntiAliasingOptions.end()); drawMenu(menu,items,static_cast<int>(displayed)); }
            group(L"INPUT", static_cast<float>(GetSettingsInputHeadingTop() - bounds.top) / dpiScale);
            drawToggle(6, ButtonKind::SettingsSpaceMouse, L"Enable 3Dconnexion SpaceMouse", spaceMouseRuntimeAvailable_ && spaceMouseEnabled_, spaceMouseRuntimeAvailable_);
            }
            renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
            const auto drawSettingsScrollIndicator = [&](bool up) {
                const RECT indicatorBounds = GetSettingsScrollIndicatorBounds(up);
                const D2D1_RECT_F indicator = D2D1::RectF(static_cast<float>(indicatorBounds.left), static_cast<float>(indicatorBounds.top), static_cast<float>(indicatorBounds.right), static_cast<float>(indicatorBounds.bottom));
                const ButtonKind button = up ? ButtonKind::SettingsScrollUp : ButtonKind::SettingsScrollDown;
                if (hoveredButton_ == button || pressedButton_ == button) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(indicator, 4.0f * dpiScale, 4.0f * dpiScale), rowHover.Get());
                const float centerX = (indicator.left + indicator.right) * 0.5f, centerY = (indicator.top + indicator.bottom) * 0.5f, arm = 4.0f * dpiScale;
                const float tipY = up ? centerY - arm * 0.5f : centerY + arm * 0.5f, baseY = up ? centerY + arm * 0.5f : centerY - arm * 0.5f;
                renderTarget_->DrawLine(D2D1::Point2F(centerX - arm, baseY), D2D1::Point2F(centerX, tipY), secondaryBrush.Get(), 1.5f * dpiScale);
                renderTarget_->DrawLine(D2D1::Point2F(centerX, tipY), D2D1::Point2F(centerX + arm, baseY), secondaryBrush.Get(), 1.5f * dpiScale);
            };
            if (SettingsScrollUpVisible()) drawSettingsScrollIndicator(true);
            if (SettingsScrollDownVisible()) drawSettingsScrollIndicator(false);
            renderTarget_->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -settingsScroll_));
            if (settingsPage_ == SettingsPage::Image2D) {
                if (zoomHudPositionMenuOpen_) drawForegroundMenu(GetSettingsZoomHudMenuBounds(), { L"bottom left", L"bottom right", L"top left", L"top right" }, static_cast<int>(zoomHudPosition_), { ButtonKind::SettingsZoomHudBottomLeft, ButtonKind::SettingsZoomHudBottomRight, ButtonKind::SettingsZoomHudTopLeft, ButtonKind::SettingsZoomHudTopRight });
            } else if (settingsPage_ == SettingsPage::Model3D) {
                if (upAxisMenuOpen_) drawForegroundMenu(GetSettingsUpAxisMenuBounds(), { L"z axis up", L"y axis up", L"x axis up" }, modelUpAxis_ == ModelUpAxis::ZUp ? 0 : modelUpAxis_ == ModelUpAxis::YUp ? 1 : 2, { ButtonKind::SettingsUpAxisZ, ButtonKind::SettingsUpAxisY, ButtonKind::SettingsUpAxisX });
                if (buildPlateMenuOpen_) drawForegroundMenu(GetSettingsBuildPlateMenuBounds(), { L"auto", L"on", L"off" }, static_cast<int>(modelBuildPlate_), { ButtonKind::SettingsBuildPlateAuto, ButtonKind::SettingsBuildPlateOn, ButtonKind::SettingsBuildPlateOff });
                if (axisIndicatorPositionMenuOpen_) drawForegroundMenu(GetSettingsAxisIndicatorPositionMenuBounds(), { L"bottom left", L"bottom right", L"top left", L"top right" }, static_cast<int>(axisIndicatorPosition_), { ButtonKind::SettingsAxisIndicatorBottomLeft, ButtonKind::SettingsAxisIndicatorBottomRight, ButtonKind::SettingsAxisIndicatorTopLeft, ButtonKind::SettingsAxisIndicatorTopRight });
                if (projectionMenuOpen_) drawForegroundMenu(GetSettingsProjectionMenuBounds(), { L"perspective", L"orthographic" }, static_cast<int>(modelProjectionMode_), { ButtonKind::SettingsProjectionPerspective, ButtonKind::SettingsProjectionOrthographic });
                if (graphicsAdapterMenuOpen_) { std::vector<const wchar_t*> items{L"Auto (High Performance)"}; int selected=graphicsAdapterAuto_?0:-1; for(int i=0;i<(int)graphicsAdapters_.size();++i){items.push_back(graphicsAdapters_[i].name.c_str());if(!graphicsAdapterAuto_&&SameGraphicsAdapterLuid(graphicsAdapters_[i].luid,graphicsAdapterLuid_))selected=i+1;} drawForegroundMenu(GetSettingsGraphicsAdapterMenuBounds(),items,selected,{},hoveredButton_==ButtonKind::SettingsGraphicsAdapterOption?graphicsAdapterMenuOption_:-1); }
                if (antiAliasingMenuOpen_) drawForegroundMenu(GetSettingsAntiAliasingMenuBounds(),std::vector<const wchar_t*>(kAntiAliasingOptions.begin(),kAntiAliasingOptions.end()),static_cast<int>(EffectiveModelAntiAliasing(modelAntiAliasing_,modelVisualStyle_)),{ ButtonKind::SettingsAntiAliasingOff, ButtonKind::SettingsAntiAliasing2x, ButtonKind::SettingsAntiAliasing4x, ButtonKind::SettingsAntiAliasing8x, ButtonKind::SettingsAntiAliasingSsaa1_5x, ButtonKind::SettingsAntiAliasingSsaa2x });
            }
            renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
            renderTarget_->PopAxisAlignedClip();
        } else if (overlay_ == OverlayKind::ResetConfirm) {
            DrawOverlayText(L"Reset Viewtrious to defaults?", left, static_cast<float>(bounds.top) + panelPadding,
                contentWidth, 36.0f * dpiScale, 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
            DrawOverlayText(L"This removes Viewtrious preferences, saved window placement, and Viewtrious-owned app data.", left,
                static_cast<float>(bounds.top) + panelPadding + 45.0f * dpiScale, contentWidth, 48.0f * dpiScale,
                16.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), false, false, false, true);
            DrawOverlayText(L"Your images will not be touched.", left, static_cast<float>(bounds.top) + panelPadding + 96.0f * dpiScale,
                contentWidth, 24.0f * dpiScale, 16.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get());
            const RECT cancelBounds = GetResetConfirmationButtonBounds(false), resetBounds = GetResetConfirmationButtonBounds(true);
            const D2D1_RECT_F cancel = D2D1::RectF(static_cast<float>(cancelBounds.left), static_cast<float>(cancelBounds.top), static_cast<float>(cancelBounds.right), static_cast<float>(cancelBounds.bottom));
            const D2D1_RECT_F reset = D2D1::RectF(static_cast<float>(resetBounds.left), static_cast<float>(resetBounds.top), static_cast<float>(resetBounds.right), static_cast<float>(resetBounds.bottom));
            ComPtr<ID2D1SolidColorBrush> destructive, destructiveHover, destructivePressed, neutralHover, neutralPressed, buttonText;
            if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(196.f/255,43.f/255,28.f/255), &destructive)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(220.f/255,58.f/255,40.f/255), &destructiveHover)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(153.f/255,27.f/255,20.f/255), &destructivePressed)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(60.f/255,64.f/255,74.f/255), &neutralHover)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(75.f/255,80.f/255,92.f/255), &neutralPressed)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &buttonText))) {
                ID2D1Brush* resetBrush = pressedButton_ == ButtonKind::ResetConfirm ? destructivePressed.Get() : hoveredButton_ == ButtonKind::ResetConfirm ? destructiveHover.Get() : destructive.Get();
                renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(reset, 5.0f * dpiScale, 5.0f * dpiScale), resetBrush);
                if (pressedButton_ == ButtonKind::ResetCancel) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(cancel, 5.0f * dpiScale, 5.0f * dpiScale), neutralPressed.Get());
                else if (hoveredButton_ == ButtonKind::ResetCancel) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(cancel, 5.0f * dpiScale, 5.0f * dpiScale), neutralHover.Get());
            }
            renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(cancel, 5.0f * dpiScale, 5.0f * dpiScale), borderBrush.Get(), 1.0f);
            DrawOverlayText(L"Cancel", cancel.left, cancel.top, cancel.right - cancel.left, cancel.bottom - cancel.top, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), true, false, true);
            DrawOverlayText(L"Reset", reset.left, reset.top, reset.right - reset.left, reset.bottom - reset.top, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), true, false, true);
        } else if (overlay_ == OverlayKind::DeleteConfirm) {
            DrawOverlayText(L"Move this image to the Recycle Bin?", left, static_cast<float>(bounds.top) + panelPadding,
                contentWidth, 36.0f * dpiScale, 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
            const std::wstring filename = fs::path(currentPath_).filename().wstring();
            if (!filename.empty()) DrawOverlayText(filename.c_str(), left, static_cast<float>(bounds.top) + panelPadding + 52.0f * dpiScale,
                contentWidth, 28.0f * dpiScale, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), false, false, false, true);
            DrawOverlayText(L"The file will be moved to the Windows Recycle Bin.", left, static_cast<float>(bounds.top) + panelPadding + 94.0f * dpiScale,
                contentWidth, 22.0f * dpiScale, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get());
            const RECT warningBounds = GetDeleteWarningCheckboxBounds();
            const D2D1_RECT_F warningRow = D2D1::RectF(static_cast<float>(warningBounds.left), static_cast<float>(warningBounds.top), static_cast<float>(warningBounds.right), static_cast<float>(warningBounds.bottom));
            const float warningBoxSize = 18.0f * dpiScale;
            const D2D1_RECT_F warningBox = D2D1::RectF(warningRow.left, warningRow.top + (warningRow.bottom - warningRow.top - warningBoxSize) / 2.0f,
                warningRow.left + warningBoxSize, warningRow.top + (warningRow.bottom - warningRow.top - warningBoxSize) / 2.0f + warningBoxSize);
            const RECT cancelBounds = GetDeleteConfirmationButtonBounds(false), deleteBounds = GetDeleteConfirmationButtonBounds(true);
            const D2D1_RECT_F cancel = D2D1::RectF(static_cast<float>(cancelBounds.left), static_cast<float>(cancelBounds.top), static_cast<float>(cancelBounds.right), static_cast<float>(cancelBounds.bottom));
            const D2D1_RECT_F remove = D2D1::RectF(static_cast<float>(deleteBounds.left), static_cast<float>(deleteBounds.top), static_cast<float>(deleteBounds.right), static_cast<float>(deleteBounds.bottom));
            ComPtr<ID2D1SolidColorBrush> destructive, destructiveHover, destructivePressed, neutralHover, neutralPressed, buttonText;
            if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(196.f/255,43.f/255,28.f/255), &destructive)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(220.f/255,58.f/255,40.f/255), &destructiveHover)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(153.f/255,27.f/255,20.f/255), &destructivePressed)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(60.f/255,64.f/255,74.f/255), &neutralHover)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(75.f/255,80.f/255,92.f/255), &neutralPressed)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &buttonText))) {
                ID2D1Brush* deleteBrush = pressedButton_ == ButtonKind::DeleteConfirm ? destructivePressed.Get() : hoveredButton_ == ButtonKind::DeleteConfirm ? destructiveHover.Get() : destructive.Get();
                renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(remove, 5.0f * dpiScale, 5.0f * dpiScale), deleteBrush);
                if (pressedButton_ == ButtonKind::DeleteCancel) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(cancel, 5.0f * dpiScale, 5.0f * dpiScale), neutralPressed.Get());
                else if (hoveredButton_ == ButtonKind::DeleteCancel) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(cancel, 5.0f * dpiScale, 5.0f * dpiScale), neutralHover.Get());
            }
            renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(cancel, 5.0f * dpiScale, 5.0f * dpiScale), borderBrush.Get(), 1.0f);
            ComPtr<ID2D1SolidColorBrush> checkboxAccent, checkboxMark, checkboxHover;
            if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f / 255, 120.f / 255, 212.f / 255), &checkboxAccent)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &checkboxMark)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(60.f / 255, 64.f / 255, 74.f / 255) : D2D1::ColorF(228.f / 255, 228.f / 255, 228.f / 255), &checkboxHover))) {
                if (hoveredButton_ == ButtonKind::DeleteWarningSuppress || pressedButton_ == ButtonKind::DeleteWarningSuppress)
                    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(warningRow, 4.0f * dpiScale, 4.0f * dpiScale), checkboxHover.Get());
                if (deleteWarningSuppressOnConfirm_) {
                    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(warningBox, 3.0f * dpiScale, 3.0f * dpiScale), checkboxAccent.Get());
                    const float stroke = std::max(1.5f, 2.0f * dpiScale);
                    const D2D1_POINT_2F start = D2D1::Point2F(warningBox.left + warningBoxSize * 0.22f, warningBox.top + warningBoxSize * 0.53f);
                    const D2D1_POINT_2F middle = D2D1::Point2F(warningBox.left + warningBoxSize * 0.43f, warningBox.top + warningBoxSize * 0.74f);
                    const D2D1_POINT_2F end = D2D1::Point2F(warningBox.left + warningBoxSize * 0.78f, warningBox.top + warningBoxSize * 0.30f);
                    renderTarget_->DrawLine(start, middle, checkboxMark.Get(), stroke);
                    renderTarget_->DrawLine(middle, end, checkboxMark.Get(), stroke);
                } else renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(warningBox, 3.0f * dpiScale, 3.0f * dpiScale), borderBrush.Get(), 1.0f);
            }
            DrawOverlayText(L"Don't show this warning again", warningBox.right + 12.0f * dpiScale, warningRow.top,
                warningRow.right - warningBox.right - 12.0f * dpiScale, warningRow.bottom - warningRow.top,
                15.0f, DWRITE_FONT_WEIGHT_NORMAL, primaryBrush.Get(), true);
            DrawOverlayText(L"Cancel", cancel.left, cancel.top, cancel.right - cancel.left, cancel.bottom - cancel.top, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), true, false, true);
            DrawOverlayText(L"Delete", remove.left, remove.top, remove.right - remove.left, remove.bottom - remove.top, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, buttonText.Get(), true, false, true);
        } else if (overlay_ == OverlayKind::Feedback) {
            DrawOverlayText(L"Feedback", left, static_cast<float>(bounds.top) + panelPadding, contentWidth, 34.0f * dpiScale, 24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
            DrawOverlayText(L"Help make Viewtrious better.", left, static_cast<float>(bounds.top) + panelPadding + 42.0f * dpiScale, contentWidth, 26.0f * dpiScale, 16.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get());
            ComPtr<ID2D1SolidColorBrush> actionHover, actionPressed;
            if (FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(60.f / 255, 64.f / 255, 74.f / 255), &actionHover)) || FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(75.f / 255, 80.f / 255, 92.f / 255), &actionPressed))) return;
            const auto drawAction = [&](bool feature, const wchar_t* title, const wchar_t* detail) {
                const ButtonKind button = feature ? ButtonKind::FeedbackFeature : ButtonKind::FeedbackBug;
                const RECT buttonBounds = GetFeedbackActionBounds(feature);
                const D2D1_RECT_F action = D2D1::RectF(static_cast<float>(buttonBounds.left), static_cast<float>(buttonBounds.top), static_cast<float>(buttonBounds.right), static_cast<float>(buttonBounds.bottom));
                if (pressedButton_ == button) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(action, 5.0f * dpiScale, 5.0f * dpiScale), actionPressed.Get());
                else if (hoveredButton_ == button) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(action, 5.0f * dpiScale, 5.0f * dpiScale), actionHover.Get());
                renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(action, 5.0f * dpiScale, 5.0f * dpiScale), borderBrush.Get(), 1.0f);
                DrawOverlayText(title, action.left + 16.0f * dpiScale, action.top + 6.0f * dpiScale, action.right - action.left - 32.0f * dpiScale, 27.0f * dpiScale, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), true);
                DrawOverlayText(detail, action.left + 16.0f * dpiScale, action.top + 35.0f * dpiScale, action.right - action.left - 32.0f * dpiScale, 23.0f * dpiScale, 15.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), true);
            };
            drawAction(false, L"Report a bug", L"Something isn't working correctly.");
            drawAction(true, L"Suggest a feature", L"Have an idea for Viewtrious?");
            DrawOverlayText(L"Opens GitHub in your web browser.", left, static_cast<float>(bounds.bottom) - panelPadding - 20.0f * dpiScale, contentWidth, 20.0f * dpiScale, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), false, true);
        } else {
            float logoBottom = static_cast<float>(bounds.top) + panelPadding;
            const UINT logoWidth = static_cast<UINT>(std::max(1.0f, std::round(std::min(520.0f * dpiScale, contentWidth))));
            const UINT logoHeight = static_cast<UINT>(std::max(1.0f, std::round(static_cast<float>(logoWidth) * 577.0f / 2375.0f)));
            if (EnsureAboutLogo(logoWidth, logoHeight)) {
                const float logoLeft = std::round(static_cast<float>(bounds.left) + 40.0f * dpiScale);
                const float logoTop = std::round(logoBottom);
                renderTarget_->DrawBitmap(aboutLogo_.Get(), D2D1::RectF(logoLeft, logoTop, logoLeft + logoWidth, logoTop + logoHeight));
                logoBottom = logoTop + logoHeight;
            }
            const float logoLeft = static_cast<float>(bounds.left) + 40.0f * dpiScale;
            const float textTop = logoBottom + 16.0f * dpiScale;
            DrawOverlayText(L"Version " VIEWTRIOUS_VERSION, logoLeft, textTop, static_cast<float>(logoWidth), 20.0f * dpiScale,
                14.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), false, true);
            DrawOverlayText(L"Extremely lightweight image viewer", logoLeft, textTop + 25.0f * dpiScale, static_cast<float>(logoWidth),
                20.0f * dpiScale, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), false, true);
            DrawOverlayText(L"3D input device development tools and related technology are provided under license from 3Dconnexion. © 3Dconnexion 1992 - 2025. All rights reserved.",
                logoLeft, textTop + 49.0f * dpiScale, static_cast<float>(logoWidth), 38.0f * dpiScale, 10.5f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), false, true);
        }
    }

    void DrawDropdown() {
        if (!dropdownOpen_) return;
        const RECT bounds = GetDropdownBounds();
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
        const bool dark = UseDarkAppMode();
        const D2D1_COLOR_F surface = dark ? D2D1::ColorF(40.0f / 255.0f, 43.0f / 255.0f, 50.0f / 255.0f)
            : D2D1::ColorF(250.0f / 255.0f, 250.0f / 255.0f, 250.0f / 255.0f);
        const D2D1_COLOR_F border = dark ? D2D1::ColorF(82.0f / 255.0f, 86.0f / 255.0f, 96.0f / 255.0f)
            : D2D1::ColorF(190.0f / 255.0f, 190.0f / 255.0f, 190.0f / 255.0f);
        const D2D1_COLOR_F text = dark ? D2D1::ColorF(D2D1::ColorF::White) : D2D1::ColorF(28.0f / 255.0f, 28.0f / 255.0f, 28.0f / 255.0f);
        const D2D1_COLOR_F hover = dark ? D2D1::ColorF(60.0f / 255.0f, 64.0f / 255.0f, 74.0f / 255.0f)
            : D2D1::ColorF(228.0f / 255.0f, 228.0f / 255.0f, 228.0f / 255.0f);
        const D2D1_COLOR_F pressed = dark ? D2D1::ColorF(75.0f / 255.0f, 80.0f / 255.0f, 92.0f / 255.0f)
            : D2D1::ColorF(210.0f / 255.0f, 210.0f / 255.0f, 210.0f / 255.0f);
        ComPtr<ID2D1SolidColorBrush> surfaceBrush, borderBrush, textBrush, hoverBrush, pressedBrush;
        if (FAILED(renderTarget_->CreateSolidColorBrush(surface, &surfaceBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(border, &borderBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(text, &textBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(hover, &hoverBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(pressed, &pressedBrush))) return;
        const D2D1_RECT_F menu = D2D1::RectF(static_cast<float>(bounds.left), static_cast<float>(bounds.top),
            static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(menu, 7.0f, 7.0f), surfaceBrush.Get());
        const UINT dpi = GetDpiForWindow(window_);
        const int rowHeight = MulDiv(38, dpi, 96);
        const auto row = [&](int top) { return D2D1::RectF(static_cast<float>(bounds.left + 1), static_cast<float>(top),
            static_cast<float>(bounds.right - 1), static_cast<float>(top + rowHeight)); };
        const int firstTop = bounds.top + MulDiv(4, dpi, 96);
        const int iconLeft = bounds.left + MulDiv(14, dpi, 96), iconWidth = MulDiv(18, dpi, 96), labelLeft = iconLeft + MulDiv(28, dpi, 96);
        const auto drawItem = [&](DropdownItem item, int top, const wchar_t* label, wchar_t glyph) {
            if (dropdownPressed_ == item) renderTarget_->FillRectangle(row(top), pressedBrush.Get());
            else if (dropdownHovered_ == item) renderTarget_->FillRectangle(row(top), hoverBrush.Get());
            DrawMenuGlyph(glyph, static_cast<float>(iconLeft), static_cast<float>(top), static_cast<float>(iconWidth), static_cast<float>(rowHeight), textBrush.Get());
            DrawOverlayText(label, static_cast<float>(labelLeft), static_cast<float>(top),
                static_cast<float>(bounds.right - labelLeft - MulDiv(14, dpi, 96)), static_cast<float>(rowHeight),
                13.0f, DWRITE_FONT_WEIGHT_NORMAL, textBrush.Get(), true);
        };
        const int separatorGap = MulDiv(9, dpi, 96);
        int top = firstTop;
        const auto separator = [&] {
            const float y = static_cast<float>(top + separatorGap / 2);
            renderTarget_->DrawLine(D2D1::Point2F(static_cast<float>(bounds.left + MulDiv(12, dpi, 96)), y), D2D1::Point2F(static_cast<float>(bounds.right - MulDiv(12, dpi, 96)), y), borderBrush.Get());
            top += separatorGap;
        };
        drawItem(DropdownItem::OpenFile, top, L"Open File...", L'\uE8B7'); top += rowHeight;
        drawItem(DropdownItem::Settings, top, L"Settings", L'\uE713'); top += rowHeight;
        separator();
        drawItem(DropdownItem::QuickTour, top, L"Quick Tutorial", L'\uE897'); top += rowHeight;
        drawItem(DropdownItem::KeyboardShortcuts, top, L"Keyboard Shortcuts", L'\uE765'); top += rowHeight;
        separator();
        drawItem(DropdownItem::About, top, L"About", L'\uE946'); top += rowHeight;
        drawItem(DropdownItem::Feedback, top, L"Feedback", L'\uE939'); top += rowHeight;
        separator();
        drawItem(DropdownItem::Close, top, L"Close Viewtrious", L'\uE8BB');
        renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(menu, 7.0f, 7.0f), borderBrush.Get(), 1.0f);
    }

    void DrawContextMenu() {
        if (!contextMenuOpen_) return;
        const RECT bounds = GetContextMenuBounds();
        const bool dark = UseDarkAppMode();
        const D2D1_COLOR_F surface = dark ? D2D1::ColorF(40.0f / 255.0f, 43.0f / 255.0f, 50.0f / 255.0f) : D2D1::ColorF(250.0f / 255.0f, 250.0f / 255.0f, 250.0f / 255.0f);
        const D2D1_COLOR_F border = dark ? D2D1::ColorF(82.0f / 255.0f, 86.0f / 255.0f, 96.0f / 255.0f) : D2D1::ColorF(190.0f / 255.0f, 190.0f / 255.0f, 190.0f / 255.0f);
        const D2D1_COLOR_F text = dark ? D2D1::ColorF(D2D1::ColorF::White) : D2D1::ColorF(28.0f / 255.0f, 28.0f / 255.0f, 28.0f / 255.0f);
        const D2D1_COLOR_F disabled = dark ? D2D1::ColorF(125.0f / 255.0f, 128.0f / 255.0f, 134.0f / 255.0f) : D2D1::ColorF(145.0f / 255.0f, 145.0f / 255.0f, 145.0f / 255.0f);
        const D2D1_COLOR_F hover = dark ? D2D1::ColorF(60.0f / 255.0f, 64.0f / 255.0f, 74.0f / 255.0f) : D2D1::ColorF(228.0f / 255.0f, 228.0f / 255.0f, 228.0f / 255.0f);
        const D2D1_COLOR_F pressed = dark ? D2D1::ColorF(75.0f / 255.0f, 80.0f / 255.0f, 92.0f / 255.0f) : D2D1::ColorF(210.0f / 255.0f, 210.0f / 255.0f, 210.0f / 255.0f);
        ComPtr<ID2D1SolidColorBrush> surfaceBrush, borderBrush, textBrush, disabledBrush, hoverBrush, pressedBrush;
        if (FAILED(renderTarget_->CreateSolidColorBrush(surface, &surfaceBrush)) || FAILED(renderTarget_->CreateSolidColorBrush(border, &borderBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(text, &textBrush)) || FAILED(renderTarget_->CreateSolidColorBrush(disabled, &disabledBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(hover, &hoverBrush)) || FAILED(renderTarget_->CreateSolidColorBrush(pressed, &pressedBrush))) return;
        const D2D1_RECT_F menu = D2D1::RectF(static_cast<float>(bounds.left), static_cast<float>(bounds.top), static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(menu, 7.0f, 7.0f), surfaceBrush.Get());
        if (ModelActive()) {
            const UINT dpi = GetDpiForWindow(window_); const int rowHeight = MulDiv(38, dpi, 96), labelLeft = bounds.left + MulDiv(18, dpi, 96);
            int top = bounds.top + MulDiv(kContextMenuPaddingDip, dpi, 96);
            const auto drawModelItem = [&](ContextAction action, const wchar_t* label) {
                const bool enabled = ContextActionEnabled(action);
                const D2D1_RECT_F row = D2D1::RectF(float(bounds.left + 1), float(top), float(bounds.right - 1), float(top + rowHeight));
                if (enabled && contextPressed_ == action) renderTarget_->FillRectangle(row, pressedBrush.Get());
                else if (enabled && contextHovered_ == action) renderTarget_->FillRectangle(row, hoverBrush.Get());
                DrawOverlayText(label, float(labelLeft), float(top), float(bounds.right - labelLeft - MulDiv(12, dpi, 96)), float(rowHeight), 13,
                    DWRITE_FONT_WEIGHT_NORMAL, enabled ? textBrush.Get() : disabledBrush.Get(), true);
                top += rowHeight;
            };
            drawModelItem(ContextAction::SnapViewToFace, L"Face to View");
            renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(menu,7,7),borderBrush.Get(),1); return;
        }
        const UINT dpi = GetDpiForWindow(window_); const int rowHeight = MulDiv(38, dpi, 96); const int gap = MulDiv(9, dpi, 96);
        int top = bounds.top + MulDiv(kContextMenuPaddingDip, dpi, 96);
        const int iconLeft = bounds.left + MulDiv(14, dpi, 96), iconWidth = MulDiv(18, dpi, 96), labelLeft = iconLeft + MulDiv(28, dpi, 96);
        const auto drawItem = [&](ContextAction action, const wchar_t* label, wchar_t glyph) {
            const bool enabled = ContextActionEnabled(action);
            const D2D1_RECT_F row = D2D1::RectF(static_cast<float>(bounds.left + 1), static_cast<float>(top), static_cast<float>(bounds.right - 1), static_cast<float>(top + rowHeight));
            if (enabled && contextPressed_ == action) renderTarget_->FillRectangle(row, pressedBrush.Get());
            else if (enabled && contextHovered_ == action) renderTarget_->FillRectangle(row, hoverBrush.Get());
            const int rightPadding = action == ContextAction::OpenWith ? MulDiv(48, dpi, 96) : MulDiv(14, dpi, 96);
            ID2D1Brush* itemBrush = enabled ? textBrush.Get() : disabledBrush.Get();
            DrawMenuGlyph(glyph, static_cast<float>(iconLeft), static_cast<float>(top), static_cast<float>(iconWidth), static_cast<float>(rowHeight), itemBrush, action == ContextAction::RotateLeft);
            DrawOverlayText(label, static_cast<float>(labelLeft), static_cast<float>(top), static_cast<float>(bounds.right - labelLeft - rightPadding),
                static_cast<float>(rowHeight), 13.0f, DWRITE_FONT_WEIGHT_NORMAL, itemBrush, true);
            top += rowHeight;
        };
        const auto separator = [&] {
            const float y = static_cast<float>(top + gap / 2);
            renderTarget_->DrawLine(D2D1::Point2F(static_cast<float>(bounds.left + MulDiv(12, dpi, 96)), y), D2D1::Point2F(static_cast<float>(bounds.right - MulDiv(12, dpi, 96)), y), borderBrush.Get());
            top += gap;
        };
        drawItem(ContextAction::Fullscreen, fullscreen_ ? L"Exit Fullscreen" : L"Fullscreen", fullscreen_ ? L'\uE73F' : L'\uE740'); separator();
        const bool heifRotationWorking = IsHeifPath(currentPath_) && heifRotationMenuLocked_;
        drawItem(ContextAction::RotateLeft, heifRotationWorking ? L"Rotate Left (working...)" : L"Rotate Left", L'\uE7AD');
        drawItem(ContextAction::RotateRight, heifRotationWorking ? L"Rotate Right (working...)" : L"Rotate Right", L'\uE7AD'); separator();
        const int openWithTop = top;
        drawItem(ContextAction::OpenWith, L"Open With", L'\uE8A7');
        DrawOverlayText(L">", static_cast<float>(bounds.right - MulDiv(28, dpi, 96)), static_cast<float>(openWithTop), static_cast<float>(MulDiv(16, dpi, 96)),
            static_cast<float>(rowHeight), 14.0f, DWRITE_FONT_WEIGHT_NORMAL, ContextActionEnabled(ContextAction::OpenWith) ? textBrush.Get() : disabledBrush.Get(), true, true);
        drawItem(ContextAction::Copy, L"Copy", L'\uE8C8'); drawItem(ContextAction::Print, L"Print", L'\uE749'); separator();
        drawItem(ContextAction::SetBackground, L"Set as Desktop Background", L'\uE7F4'); separator(); drawItem(ContextAction::Delete, L"Delete", L'\uE74D');
        renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(menu, 7.0f, 7.0f), borderBrush.Get(), 1.0f);
    }

    void DrawOpenWithSubmenu() {
        if (!openWithSubmenuOpen_) return;
        const RECT bounds = GetOpenWithSubmenuBounds(); const bool dark = UseDarkAppMode();
        ComPtr<ID2D1SolidColorBrush> surface, border, text, hover;
        if (FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(40.f/255,43.f/255,50.f/255) : D2D1::ColorF(250.f/255,250.f/255,250.f/255), &surface)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(82.f/255,86.f/255,96.f/255) : D2D1::ColorF(190.f/255,190.f/255,190.f/255), &border)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(D2D1::ColorF::White) : D2D1::ColorF(28.f/255,28.f/255,28.f/255), &text)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(60.f/255,64.f/255,74.f/255) : D2D1::ColorF(228.f/255,228.f/255,228.f/255), &hover))) return;
        const D2D1_RECT_F menu = D2D1::RectF((float)bounds.left,(float)bounds.top,(float)bounds.right,(float)bounds.bottom);
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(menu,7,7),surface.Get());
        const UINT dpi=GetDpiForWindow(window_); const int row=MulDiv(38,dpi,96), gap=MulDiv(9,dpi,96); int top=bounds.top+MulDiv(4,dpi,96);
        const auto draw=[&](int index,const wchar_t* label){ if(openWithHovered_==index) renderTarget_->FillRectangle(D2D1::RectF((float)bounds.left+1,(float)top,(float)bounds.right-1,(float)(top+row)),hover.Get()); DrawOverlayText(label,(float)bounds.left+MulDiv(14,dpi,96),(float)top,(float)(bounds.right-bounds.left-MulDiv(28,dpi,96)),(float)row,13,DWRITE_FONT_WEIGHT_NORMAL,text.Get(),true); top+=row; };
        for(int i=0;i<(int)openWithHandlers_.size();++i) draw(i,openWithHandlers_[i].name.c_str());
        const float y=(float)(top+gap/2); renderTarget_->DrawLine(D2D1::Point2F((float)bounds.left+MulDiv(12,dpi,96),y),D2D1::Point2F((float)bounds.right-MulDiv(12,dpi,96),y),border.Get()); top+=gap;
        draw((int)openWithHandlers_.size(),L"Choose another app");
        renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(menu,7,7),border.Get());
    }

    void DrawHandwrittenText(const wchar_t* text, float x, float y, float width, float height, float size,
        ID2D1Brush* brush, bool center = false, bool right = false) {
        ComPtr<IDWriteTextFormat> format;
        const float dpiScale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        if (FAILED(dwriteFactory_->CreateTextFormat(L"Segoe Print", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * dpiScale, L"", &format))) return;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (center) format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        else if (right) format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)), format.Get(), width, height, &layout))) return;
        renderTarget_->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void DrawTutorial() {
        if (!TutorialActive()) return;
        const UINT dpi = GetDpiForWindow(window_);
        const float scale = static_cast<float>(dpi) / 96.0f;
        const bool dark = UseDarkAppMode();
        ComPtr<ID2D1SolidColorBrush> pencil, veil, accent, button, buttonHover, buttonPressed, buttonText;
        if (FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(250.f / 255, 194.f / 255, 72.f / 255) : D2D1::ColorF(93.f / 255, 64.f / 255, 12.f / 255), &pencil)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, dark ? 0.08f : 0.04f), &veil)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f / 255, 120.f / 255, 212.f / 255), &accent)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f / 255, 120.f / 255, 212.f / 255), &button)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f / 255, 139.f / 255, 244.f / 255), &buttonHover)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f / 255, 94.f / 255, 168.f / 255), &buttonPressed)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &buttonText))) return;
        RECT client{};
        GetClientRect(window_, &client);
        const float canvasTop = fullscreen_ ? 0.0f : static_cast<float>(GetFrameMetrics(window_).titleBarHeight);
        renderTarget_->FillRectangle(D2D1::RectF(0, canvasTop, static_cast<float>(client.right), static_cast<float>(client.bottom)), veil.Get());
        const auto scribble = [&](const RECT& target) {
            const float left = static_cast<float>(target.left) - 8.0f * scale, right = static_cast<float>(target.right) + 8.0f * scale;
            const float top = static_cast<float>(target.top) - 7.0f * scale, bottom = static_cast<float>(target.bottom) + 7.0f * scale;
            const D2D1_POINT_2F points[] = { { left, top + 6.0f * scale }, { left + (right-left)*0.25f, top }, { right - 3.0f*scale, top + 5.0f*scale },
                { right, top + (bottom-top)*0.48f }, { right - 5.0f*scale, bottom }, { left + (right-left)*0.46f, bottom - 2.0f*scale },
                { left, bottom - 5.0f*scale }, { left - 2.0f*scale, top + (bottom-top)*0.45f }, { left, top + 6.0f*scale } };
            for (size_t i = 1; i < ARRAYSIZE(points); ++i) renderTarget_->DrawLine(points[i - 1], points[i], pencil.Get(), 2.0f * scale);
        };
        const auto arrow = [&](D2D1_POINT_2F from, D2D1_POINT_2F to, float thickness = 2.2f) {
            const D2D1_POINT_2F bend = D2D1::Point2F((from.x + to.x) / 2.0f, from.y + (to.y - from.y) * 0.25f - 18.0f * scale);
            renderTarget_->DrawLine(from, bend, pencil.Get(), thickness * scale);
            renderTarget_->DrawLine(bend, to, pencil.Get(), thickness * scale);
            const float dx = to.x - bend.x, dy = to.y - bend.y, length = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
            const float ux = dx / length, uy = dy / length, wing = 10.0f * scale;
            renderTarget_->DrawLine(to, D2D1::Point2F(to.x - ux * wing - uy * wing * 0.55f, to.y - uy * wing + ux * wing * 0.55f), pencil.Get(), thickness * scale);
            renderTarget_->DrawLine(to, D2D1::Point2F(to.x - ux * wing + uy * wing * 0.55f, to.y - uy * wing - ux * wing * 0.55f), pencil.Get(), thickness * scale);
        };
        const auto straightArrow = [&](D2D1_POINT_2F from, D2D1_POINT_2F to, float thickness = 2.0f) {
            renderTarget_->DrawLine(from, to, pencil.Get(), thickness * scale);
            const float dx = to.x - from.x, dy = to.y - from.y, length = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
            const float ux = dx / length, uy = dy / length, wing = 9.0f * scale;
            renderTarget_->DrawLine(to, D2D1::Point2F(to.x - ux * wing - uy * wing * 0.55f, to.y - uy * wing + ux * wing * 0.55f), pencil.Get(), thickness * scale);
            renderTarget_->DrawLine(to, D2D1::Point2F(to.x - ux * wing + uy * wing * 0.55f, to.y - uy * wing - ux * wing * 0.55f), pencil.Get(), thickness * scale);
        };
        const FrameMetrics frame = GetFrameMetrics(window_);
        if (tutorialStep_ == TutorialStep::OpenImages) {
            const RECT target = GetEmptyOpenFileButtonBounds();
            const float annotationWidth = std::min(340.0f * scale, static_cast<float>(client.right) - 32.0f * scale);
            const float annotationLeft = (static_cast<float>(target.left + target.right) - annotationWidth) / 2.0f;
            const float headingTop = static_cast<float>(target.bottom) + 24.0f * scale;
            DrawHandwrittenText(L"Open images", annotationLeft, headingTop, annotationWidth, 32.0f * scale, 24.0f, pencil.Get(), true);
            DrawOverlayText(L"Use Open File or drag and drop", annotationLeft, headingTop + 31.0f * scale,
                annotationWidth, 24.0f * scale, 16.0f, DWRITE_FONT_WEIGHT_NORMAL, pencil.Get(), true, false, true);
            scribble(target);
            const float arrowX = (target.left + target.right) / 2.0f;
            const float arrowTipY = static_cast<float>(target.bottom) + 4.0f * scale;
            renderTarget_->DrawLine(D2D1::Point2F(arrowX, headingTop - 3.0f * scale), D2D1::Point2F(arrowX, arrowTipY), pencil.Get(), 2.2f * scale);
            renderTarget_->DrawLine(D2D1::Point2F(arrowX, arrowTipY), D2D1::Point2F(arrowX - 6.0f * scale, arrowTipY + 9.0f * scale), pencil.Get(), 2.2f * scale);
            renderTarget_->DrawLine(D2D1::Point2F(arrowX, arrowTipY), D2D1::Point2F(arrowX + 6.0f * scale, arrowTipY + 9.0f * scale), pencil.Get(), 2.2f * scale);
            const RECT skipBounds = GetTutorialButtonBounds(false), nextBounds = GetTutorialButtonBounds(true);
            const RECT footer{ skipBounds.left - MulDiv(8, dpi, 96), skipBounds.top - MulDiv(8, dpi, 96),
                nextBounds.right + MulDiv(8, dpi, 96), nextBounds.bottom + MulDiv(8, dpi, 96) };
            const float circleX = (footer.left + footer.right) / 2.0f, circleY = (footer.top + footer.bottom) / 2.0f;
            const float circleRadiusX = (footer.right - footer.left) / 2.0f + 8.0f * scale;
            const float circleRadiusY = (footer.bottom - footer.top) / 2.0f + 10.0f * scale;
            renderTarget_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(circleX, circleY), circleRadiusX, circleRadiusY), pencil.Get(), 2.0f * scale);
            const float arrowOffsets[] = { -0.66f, -0.22f, 0.22f, 0.66f };
            const float arrowStarts[] = { -120.0f, -48.0f, 48.0f, 120.0f };
            for (size_t index = 0; index < ARRAYSIZE(arrowOffsets); ++index) {
                const float offset = arrowOffsets[index] * circleRadiusX;
                const float tipX = circleX + offset;
                const float tipY = circleY - circleRadiusY * std::sqrt(1.0f - arrowOffsets[index] * arrowOffsets[index]);
                straightArrow(D2D1::Point2F(circleX + arrowStarts[index] * scale, tipY - 52.0f * scale),
                    D2D1::Point2F(tipX, tipY), 1.8f);
            }
        } else if (tutorialStep_ == TutorialStep::ResizeWindow) {
            const float noteWidth = std::min(310.0f * scale, static_cast<float>(client.right) - 36.0f * scale);
            const float noteLeft = 20.0f * scale;
            const float noteTop = static_cast<float>(client.bottom) - 136.0f * scale;
            DrawHandwrittenText(L"Resize the window", noteLeft, noteTop, noteWidth, 34.0f * scale, 23.0f, pencil.Get());
            DrawOverlayText(L"Drag the app corners to resize the app", noteLeft, noteTop + 33.0f * scale,
                noteWidth, 26.0f * scale, 16.0f, DWRITE_FONT_WEIGHT_NORMAL, pencil.Get(), true);
            arrow(D2D1::Point2F(noteLeft + noteWidth * 0.38f, noteTop + 66.0f * scale),
                D2D1::Point2F(5.0f * scale, static_cast<float>(client.bottom) - 5.0f * scale), 2.5f);
        } else if (tutorialStep_ == TutorialStep::MenuSettings) {
            const float lineLeft = static_cast<float>(frame.hamburger.right) + 4.0f * scale;
            const float headingLeft = lineLeft + 72.0f * scale;
            const float headingTop = static_cast<float>(frame.hamburger.top) + 1.0f * scale;
            const float lineY = static_cast<float>(frame.hamburger.top + frame.hamburger.bottom) / 2.0f;
            renderTarget_->DrawLine(D2D1::Point2F(lineLeft, lineY), D2D1::Point2F(headingLeft - 8.0f * scale, lineY), pencil.Get(), 2.2f * scale);
            renderTarget_->DrawLine(D2D1::Point2F(lineLeft, lineY), D2D1::Point2F(lineLeft + 11.0f * scale, lineY - 7.0f * scale), pencil.Get(), 2.2f * scale);
            renderTarget_->DrawLine(D2D1::Point2F(lineLeft, lineY), D2D1::Point2F(lineLeft + 11.0f * scale, lineY + 7.0f * scale), pencil.Get(), 2.2f * scale);
            DrawHandwrittenText(L"Menu & settings", headingLeft, headingTop,
                std::max(120.0f * scale, static_cast<float>(client.right) - headingLeft - 20.0f * scale), 38.0f * scale, 22.0f, pencil.Get());
            scribble(frame.hamburger);
        } else if (tutorialStep_ == TutorialStep::ImageDetails) {
            const RECT target{ frame.resolutionLeft, 0, frame.titleBarContent.right, frame.titleBarHeight };
            const float headingTop = canvasTop + 52.0f * scale;
            const float headingWidth = std::min(360.0f * scale, static_cast<float>(client.right) - 40.0f * scale);
            const float arrowStartX = std::clamp((target.left + target.right) / 2.0f + 60.0f * scale, headingWidth / 2.0f + 20.0f * scale,
                static_cast<float>(client.right) - headingWidth / 2.0f - 20.0f * scale);
            const float headingLeft = arrowStartX - headingWidth / 2.0f;
            arrow(D2D1::Point2F(arrowStartX, headingTop + 2.0f * scale),
                D2D1::Point2F((target.left + target.right) / 2.0f, static_cast<float>(target.bottom) + 4.0f * scale));
            DrawHandwrittenText(L"Image details appear here", headingLeft, headingTop,
                headingWidth, 38.0f * scale, 22.0f, pencil.Get(), true);
            scribble(target);
        } else if (tutorialStep_ == TutorialStep::ContextMenu) {
            const RECT target = GetContextMenuBounds();
            const float annotationWidth = std::min(230.0f * scale, std::max(150.0f * scale, static_cast<float>(target.left) - 34.0f * scale));
            const float headingLeft = std::max(16.0f * scale, static_cast<float>(target.left) - annotationWidth - 72.0f * scale);
            const float headingTop = std::clamp(static_cast<float>(target.top) + 16.0f * scale, canvasTop + 12.0f * scale,
                static_cast<float>(client.bottom) - 100.0f * scale);
            DrawHandwrittenText(L"Right click menu", headingLeft, headingTop, annotationWidth, 38.0f * scale, 22.0f, pencil.Get(), false, true);
            const float noteGap = 18.0f * scale;
            const float noteWidth = std::min(340.0f * scale, static_cast<float>(target.left) - noteGap - 16.0f * scale);
            const float noteLeft = std::max(16.0f * scale, static_cast<float>(target.left) - noteGap - noteWidth);
            DrawOverlayText(L"Right-click an image to find these options.", noteLeft, headingTop + 37.0f * scale,
                noteWidth, 28.0f * scale, 16.0f, DWRITE_FONT_WEIGHT_NORMAL, pencil.Get(), true, true);
            scribble(target);
            arrow(D2D1::Point2F(headingLeft + annotationWidth * 0.62f, headingTop - 8.0f * scale),
                D2D1::Point2F(static_cast<float>(target.left) - 4.0f * scale, static_cast<float>(target.top) + 10.0f * scale), 3.1f);
        } else if (tutorialStep_ == TutorialStep::Shortcuts) {
            const RECT target = GetOverlayBounds();
            DrawHandwrittenText(L"Keyboard shortcuts", static_cast<float>(target.left), static_cast<float>(target.top) - 38.0f * scale,
                static_cast<float>(target.right - target.left), 32.0f * scale, 21.0f, pencil.Get(), true);
            scribble(target);
            DrawHandwrittenText(L"Thanks for downloading, enjoy!", 0, static_cast<float>(target.bottom) + 10.0f * scale,
                static_cast<float>(client.right), 34.0f * scale, 18.0f, pencil.Get(), true);
        }
        const RECT skipBounds = GetTutorialButtonBounds(false), nextBounds = GetTutorialButtonBounds(true);
        const auto asRect = [](const RECT& value) { return D2D1::RectF(static_cast<float>(value.left), static_cast<float>(value.top), static_cast<float>(value.right), static_cast<float>(value.bottom)); };
        const D2D1_RECT_F skip = asRect(skipBounds), next = asRect(nextBounds);
        if (hoveredButton_ == ButtonKind::TutorialSkip || pressedButton_ == ButtonKind::TutorialSkip) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(skip, 5.0f * scale, 5.0f * scale), pressedButton_ == ButtonKind::TutorialSkip ? buttonPressed.Get() : buttonHover.Get());
        renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(skip, 5.0f * scale, 5.0f * scale), pencil.Get(), 1.0f * scale);
        ID2D1Brush* nextBrush = pressedButton_ == ButtonKind::TutorialNext ? buttonPressed.Get() : hoveredButton_ == ButtonKind::TutorialNext ? buttonHover.Get() : button.Get();
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(next, 5.0f * scale, 5.0f * scale), nextBrush);
        DrawOverlayText(L"Skip", skip.left, skip.top, skip.right - skip.left, skip.bottom - skip.top, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, pencil.Get(), true, false, true);
        DrawOverlayText(tutorialStep_ == TutorialStep::Shortcuts ? L"Finish" : L"Next", next.left, next.top, next.right - next.left, next.bottom - next.top, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, buttonText.Get(), true, false, true);
    }

    void DrawCopyFeedback() {
        if (!copyFeedbackActive_) return;
        const ULONGLONG elapsed = GetTickCount64() - copyFeedbackStart_;
        if (elapsed >= 1000) return;
        const float opacity = animationsEnabled_ ? 0.75f * (1.0f - static_cast<float>(elapsed) / 1000.0f) : 0.75f;
        ComPtr<ID2D1SolidColorBrush> brush, outline, textHalo, textOutline;
        const D2D1_COLOR_F color = D2D1::ColorF(D2D1::ColorF::White, opacity);
        if (FAILED(renderTarget_->CreateSolidColorBrush(color, &brush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(26.0f / 255.0f, 26.0f / 255.0f, 26.0f / 255.0f, opacity), &outline)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, opacity * 0.45f), &textHalo)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(26.0f / 255.0f, 26.0f / 255.0f, 26.0f / 255.0f, opacity), &textOutline))) return;
        const D2D1_SIZE_F size = renderTarget_->GetSize(); const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        const float top = fullscreen_ ? 0.0f : static_cast<float>(GetFrameMetrics(window_).titleBarHeight);
        const float glyph = 200.0f * scale;
        const float stroke = 12.0f * scale;
        const float offset = 36.0f * scale;
        const float textHeight = 40.0f * scale;
        const float gap = 24.0f * scale;
        const float totalHeight = glyph + gap + textHeight;
        const float x = (size.width - glyph) / 2.0f;
        const float y = top + (size.height - top - totalHeight) / 2.0f;
        if (feedbackIsWallpaper_) {
            D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties();
            properties.startCap = D2D1_CAP_STYLE_ROUND;
            properties.endCap = D2D1_CAP_STYLE_ROUND;
            properties.dashCap = D2D1_CAP_STYLE_ROUND;
            properties.lineJoin = D2D1_LINE_JOIN_ROUND;
            ComPtr<ID2D1StrokeStyle> roundedStroke;
            if (FAILED(d2dFactory_->CreateStrokeStyle(properties, nullptr, 0, &roundedStroke))) return;
            const float screenLeft = x + 12.0f * scale, screenTop = y + 24.0f * scale;
            const float screenRight = x + glyph - 12.0f * scale, screenBottom = y + 140.0f * scale;
            const D2D1_ROUNDED_RECT screen = D2D1::RoundedRect(D2D1::RectF(screenLeft, screenTop, screenRight, screenBottom), 14.0f * scale, 14.0f * scale);
            const D2D1_ELLIPSE sun = D2D1::Ellipse(D2D1::Point2F(screenLeft + 43.0f * scale, screenTop + 38.0f * scale), 9.0f * scale, 9.0f * scale);
            const D2D1_POINT_2F ridgeLeft = D2D1::Point2F(screenLeft + 31.0f * scale, screenBottom - 30.0f * scale);
            const D2D1_POINT_2F ridgePeak = D2D1::Point2F(x + glyph / 2.0f - 6.0f * scale, screenBottom - 53.0f * scale);
            const D2D1_POINT_2F ridgeRight = D2D1::Point2F(screenRight - 29.0f * scale, screenBottom - 32.0f * scale);
            ComPtr<ID2D1PathGeometry> ridge;
            ComPtr<ID2D1GeometrySink> ridgeSink;
            if (FAILED(d2dFactory_->CreatePathGeometry(&ridge)) || FAILED(ridge->Open(&ridgeSink))) return;
            ridgeSink->BeginFigure(ridgeLeft, D2D1_FIGURE_BEGIN_HOLLOW);
            ridgeSink->AddLine(ridgePeak);
            ridgeSink->AddLine(ridgeRight);
            ridgeSink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (FAILED(ridgeSink->Close())) return;
            ComPtr<ID2D1SolidColorBrush> iconBrush, iconOutline;
            ComPtr<ID2D1Layer> iconLayer;
            if (FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &iconBrush)) ||
                FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(26.0f / 255.0f, 26.0f / 255.0f, 26.0f / 255.0f), &iconOutline)) ||
                FAILED(renderTarget_->CreateLayer(nullptr, &iconLayer))) return;
            D2D1_LAYER_PARAMETERS layer = D2D1::LayerParameters();
            layer.opacity = opacity;
            renderTarget_->PushLayer(layer, iconLayer.Get());
            renderTarget_->DrawRoundedRectangle(screen, iconOutline.Get(), stroke + 3.0f * scale, roundedStroke.Get());
            renderTarget_->DrawRoundedRectangle(screen, iconBrush.Get(), stroke, roundedStroke.Get());
            renderTarget_->DrawEllipse(sun, iconOutline.Get(), stroke + 3.0f * scale, roundedStroke.Get());
            renderTarget_->DrawEllipse(sun, iconBrush.Get(), stroke, roundedStroke.Get());
            renderTarget_->DrawGeometry(ridge.Get(), iconOutline.Get(), stroke + 3.0f * scale, roundedStroke.Get());
            renderTarget_->DrawGeometry(ridge.Get(), iconBrush.Get(), stroke, roundedStroke.Get());
            const float center = x + glyph / 2.0f;
            renderTarget_->DrawLine(D2D1::Point2F(center, screenBottom), D2D1::Point2F(center, y + 170.0f * scale), iconBrush.Get(), stroke, roundedStroke.Get());
            renderTarget_->DrawLine(D2D1::Point2F(x + 58.0f * scale, y + 176.0f * scale), D2D1::Point2F(x + glyph - 58.0f * scale, y + 176.0f * scale), iconBrush.Get(), stroke, roundedStroke.Get());
            renderTarget_->PopLayer();
        } else {
            const D2D1_ROUNDED_RECT rear = D2D1::RoundedRect(D2D1::RectF(x, y, x + glyph - offset, y + glyph - offset), 18.0f * scale, 18.0f * scale);
            const D2D1_ROUNDED_RECT front = D2D1::RoundedRect(D2D1::RectF(x + offset, y + offset, x + glyph, y + glyph), 18.0f * scale, 18.0f * scale);
            renderTarget_->DrawRoundedRectangle(rear, outline.Get(), stroke + 3.0f * scale); renderTarget_->DrawRoundedRectangle(front, outline.Get(), stroke + 3.0f * scale);
            renderTarget_->DrawRoundedRectangle(rear, brush.Get(), stroke); renderTarget_->DrawRoundedRectangle(front, brush.Get(), stroke);
        }
        const float textY=y+glyph+gap;
        for (const POINT offsetPoint : { POINT{ -1, 0 }, POINT{ 1, 0 }, POINT{ 0, -1 }, POINT{ 0, 1 } }) DrawOverlayText(feedbackText_.c_str(),offsetPoint.x*scale,textY+offsetPoint.y*scale,size.width,textHeight,28.f,DWRITE_FONT_WEIGHT_SEMI_BOLD,textHalo.Get(),true,false,true);
        for (const POINT offsetPoint : { POINT{ -1, 0 }, POINT{ 1, 0 }, POINT{ 0, -1 }, POINT{ 0, 1 }, POINT{ -1, -1 }, POINT{ 1, -1 }, POINT{ -1, 1 }, POINT{ 1, 1 } }) DrawOverlayText(feedbackText_.c_str(),offsetPoint.x*scale,textY+offsetPoint.y*scale,size.width,textHeight,28.f,DWRITE_FONT_WEIGHT_SEMI_BOLD,textOutline.Get(),true,false,true);
        DrawOverlayText(feedbackText_.c_str(),0,textY,size.width,textHeight,28.f,DWRITE_FONT_WEIGHT_SEMI_BOLD,brush.Get(),true,false,true);
    }

    void DrawModelViewBar() {
        const RECT bounds = GetModelViewBarBounds(); const bool dark = UseDarkAppMode(); const float dpi = GetDpiForWindow(window_) / 96.0f;
        const D2D1_RECT_F bar = D2D1::RectF((float)bounds.left,(float)bounds.top,(float)bounds.right,(float)bounds.bottom);
        ComPtr<ID2D1SolidColorBrush> surface,border,text,hover,selected;
        if(FAILED(renderTarget_->CreateSolidColorBrush(dark?D2D1::ColorF(36.f/255,39.f/255,46.f/255,.94f):D2D1::ColorF(250.f/255,250.f/255,250.f/255,.94f),&surface))||FAILED(renderTarget_->CreateSolidColorBrush(dark?D2D1::ColorF(76.f/255,80.f/255,91.f/255):D2D1::ColorF(185.f/255,185.f/255,185.f/255),&border))||FAILED(renderTarget_->CreateSolidColorBrush(dark?D2D1::ColorF(D2D1::ColorF::White):D2D1::ColorF(28.f/255,28.f/255,28.f/255),&text))||FAILED(renderTarget_->CreateSolidColorBrush(dark?D2D1::ColorF(55.f/255,59.f/255,70.f/255):D2D1::ColorF(226.f/255,226.f/255,226.f/255),&hover))||FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f/255,120.f/255,212.f/255),&selected)))return;
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(bar,6*dpi,6*dpi),surface.Get());renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(bar,6*dpi,6*dpi),border.Get(),1);
        const auto control=[&](RECT rect,ButtonKind button,const wchar_t* label,bool open){const D2D1_RECT_F r=D2D1::RectF((float)rect.left,(float)rect.top,(float)rect.right,(float)rect.bottom);if(hoveredButton_==button||pressedButton_==button||open)renderTarget_->FillRectangle(r,hover.Get());DrawOverlayText(label,r.left+static_cast<float>(kDropdownLeftPaddingDips)*dpi,r.top,r.right-r.left-static_cast<float>(kDropdownLeftPaddingDips+kDropdownChevronReserveDips)*dpi,r.bottom-r.top,14,DWRITE_FONT_WEIGHT_SEMI_BOLD,text.Get(),true);DrawDropdownChevron(r,text.Get(),dpi);};
        const RECT projection=GetModelViewBarProjectionBounds(),style=GetModelViewBarStyleBounds();control(projection,ButtonKind::ViewBarProjectionToggle,modelProjectionMode_==ModelProjectionMode::Perspective?L"Perspective":L"Orthographic",viewBarProjectionMenuOpen_);renderTarget_->DrawLine(D2D1::Point2F((float)style.left,(float)style.top+7*dpi),D2D1::Point2F((float)style.left,(float)style.bottom-7*dpi),border.Get(),1);const wchar_t* styleLabel=modelVisualStyle_==ModelVisualStyle::Shaded?L"Shaded":modelVisualStyle_==ModelVisualStyle::ShadedWithVisibleEdges?L"Shaded with Visible Edges":L"Wireframe";control(style,ButtonKind::ViewBarVisualStyleToggle,styleLabel,viewBarVisualStyleMenuOpen_);
        const auto menuItem=[&](RECT bounds,ButtonKind button,const wchar_t* value,bool active,int top){const int row=MulDiv(32,GetDpiForWindow(window_),96);const D2D1_RECT_F r=D2D1::RectF((float)bounds.left,(float)top,(float)bounds.right,(float)(top+row));if(active)renderTarget_->FillRectangle(r,selected.Get());else if(hoveredButton_==button||pressedButton_==button)renderTarget_->FillRectangle(r,hover.Get());DrawOverlayText(value,r.left+12*dpi,r.top,r.right-r.left-24*dpi,r.bottom-r.top,14,DWRITE_FONT_WEIGHT_NORMAL,text.Get(),true);};
        if(viewBarProjectionMenuOpen_){const RECT menu=GetModelViewBarProjectionMenuBounds();const D2D1_RECT_F r=D2D1::RectF((float)menu.left,(float)menu.top,(float)menu.right,(float)menu.bottom);renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(r,6*dpi,6*dpi),surface.Get());renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(r,6*dpi,6*dpi),border.Get(),1);const int row=MulDiv(32,GetDpiForWindow(window_),96);menuItem(menu,ButtonKind::ViewBarProjectionPerspective,L"Perspective",modelProjectionMode_==ModelProjectionMode::Perspective,menu.top);menuItem(menu,ButtonKind::ViewBarProjectionOrthographic,L"Orthographic",modelProjectionMode_==ModelProjectionMode::Orthographic,menu.top+row);}
        if(viewBarVisualStyleMenuOpen_){const RECT menu=GetModelViewBarStyleMenuBounds();const D2D1_RECT_F r=D2D1::RectF((float)menu.left,(float)menu.top,(float)menu.right,(float)menu.bottom);renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(r,6*dpi,6*dpi),surface.Get());renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(r,6*dpi,6*dpi),border.Get(),1);const int row=MulDiv(32,GetDpiForWindow(window_),96);menuItem(menu,ButtonKind::ViewBarVisualStyleShaded,L"Shaded",modelVisualStyle_==ModelVisualStyle::Shaded,menu.top);menuItem(menu,ButtonKind::ViewBarVisualStyleVisibleEdges,L"Shaded with Visible Edges",modelVisualStyle_==ModelVisualStyle::ShadedWithVisibleEdges,menu.top+row);menuItem(menu,ButtonKind::ViewBarVisualStyleWireframe,L"Wireframe",modelVisualStyle_==ModelVisualStyle::Wireframe,menu.top+row*2);}
    }
    void DrawTitleBar() {
        if (fullscreen_) return;

        const bool showVideoMetadata = VideoActive();
        const FrameMetrics frame = GetFrameMetrics(window_, showVideoMetadata);
        const bool dark = UseDarkAppMode();
        const D2D1_COLOR_F stripColor = dark ? D2D1::ColorF(29.0f / 255.0f, 32.0f / 255.0f, 38.0f / 255.0f)
            : D2D1::ColorF(242.0f / 255.0f, 242.0f / 255.0f, 242.0f / 255.0f);
        const D2D1_COLOR_F hoverColor = dark ? D2D1::ColorF(48.0f / 255.0f, 52.0f / 255.0f, 62.0f / 255.0f)
            : D2D1::ColorF(224.0f / 255.0f, 224.0f / 255.0f, 224.0f / 255.0f);
        const D2D1_COLOR_F pressedColor = dark ? D2D1::ColorF(64.0f / 255.0f, 68.0f / 255.0f, 80.0f / 255.0f)
            : D2D1::ColorF(204.0f / 255.0f, 204.0f / 255.0f, 204.0f / 255.0f);
        const D2D1_COLOR_F closeHoverColor = D2D1::ColorF(196.0f / 255.0f, 43.0f / 255.0f, 28.0f / 255.0f);
        const D2D1_COLOR_F closePressedColor = D2D1::ColorF(153.0f / 255.0f, 27.0f / 255.0f, 20.0f / 255.0f);
        const D2D1_COLOR_F glyphColor = dark ? D2D1::ColorF(D2D1::ColorF::White)
            : D2D1::ColorF(30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f);
        const D2D1_COLOR_F metadataColor = dark ? D2D1::ColorF(190.0f / 255.0f, 193.0f / 255.0f, 198.0f / 255.0f)
            : D2D1::ColorF(85.0f / 255.0f, 85.0f / 255.0f, 85.0f / 255.0f);
        const D2D1_COLOR_F filenameColor = dark ? D2D1::ColorF(240.0f / 255.0f, 240.0f / 255.0f, 240.0f / 255.0f)
            : D2D1::ColorF(35.0f / 255.0f, 35.0f / 255.0f, 35.0f / 255.0f);
        const D2D1_COLOR_F separatorColor = dark ? D2D1::ColorF(69.0f / 255.0f, 73.0f / 255.0f, 80.0f / 255.0f)
            : D2D1::ColorF(190.0f / 255.0f, 190.0f / 255.0f, 190.0f / 255.0f);
        ComPtr<ID2D1SolidColorBrush> stripBrush;
        ComPtr<ID2D1SolidColorBrush> hoverBrush;
        ComPtr<ID2D1SolidColorBrush> pressedBrush;
        ComPtr<ID2D1SolidColorBrush> closeHoverBrush;
        ComPtr<ID2D1SolidColorBrush> closePressedBrush;
        ComPtr<ID2D1SolidColorBrush> glyphBrush;
        ComPtr<ID2D1SolidColorBrush> closeGlyphBrush;
        ComPtr<ID2D1SolidColorBrush> metadataBrush;
        ComPtr<ID2D1SolidColorBrush> tutorialMetadataBrush;
        ComPtr<ID2D1SolidColorBrush> filenameBrush;
        ComPtr<ID2D1SolidColorBrush> separatorBrush;
        if (FAILED(renderTarget_->CreateSolidColorBrush(stripColor, &stripBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(hoverColor, &hoverBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(pressedColor, &pressedBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(closeHoverColor, &closeHoverBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(closePressedColor, &closePressedBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(glyphColor, &glyphBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &closeGlyphBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(metadataColor, &metadataBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(filenameColor, &filenameBrush)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(separatorColor, &separatorBrush))) return;
        if (tutorialStep_ == TutorialStep::ImageDetails &&
            FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(250.f / 255, 194.f / 255, 72.f / 255) : D2D1::ColorF(93.f / 255, 64.f / 255, 12.f / 255), &tutorialMetadataBrush))) return;

        const D2D1_RECT_F top = D2D1::RectF(0.0f, 0.0f, renderTarget_->GetSize().width, static_cast<float>(frame.titleBarHeight));
        renderTarget_->FillRectangle(top, stripBrush.Get());
        const auto rect = [](const RECT& value) {
            return D2D1::RectF(static_cast<float>(value.left), static_cast<float>(value.top),
                static_cast<float>(value.right), static_cast<float>(value.bottom));
        };
        const auto drawButton = [&](CaptionButton button, const RECT& bounds) {
            if (pressedCaptionButton_ == button) {
                renderTarget_->FillRectangle(rect(bounds), button == CaptionButton::Close ? closePressedBrush.Get() : pressedBrush.Get());
            } else if (hoveredCaptionButton_ == button) {
                renderTarget_->FillRectangle(rect(bounds), button == CaptionButton::Close ? closeHoverBrush.Get() : hoverBrush.Get());
            }
        };
        drawButton(CaptionButton::Minimize, frame.minimize);
        drawButton(CaptionButton::Maximize, frame.maximize);
        drawButton(CaptionButton::Close, frame.close);
        if (hamburgerPressed_) renderTarget_->FillRectangle(rect(frame.hamburger), pressedBrush.Get());
        else if (hamburgerHovered_) renderTarget_->FillRectangle(rect(frame.hamburger), hoverBrush.Get());
        renderTarget_->FillRectangle(rect(frame.hamburgerSeparator), separatorBrush.Get());
        renderTarget_->FillRectangle(rect(frame.resolutionSeparator), separatorBrush.Get());
        renderTarget_->FillRectangle(rect(frame.fileSizeSeparator), separatorBrush.Get());

        const bool tutorialMetadata = tutorialPresentation_ && tutorialStep_ == TutorialStep::ImageDetails;
        const bool hideTutorialMetadata = tutorialPresentation_ && !tutorialMetadata;
        ID2D1Brush* activeMetadataBrush = tutorialMetadata ? tutorialMetadataBrush.Get() : metadataBrush.Get();
        DrawTitleText(tutorialMetadata ? L"1920 x 1080" : hideTutorialMetadata ? L"" : resolutionText_, static_cast<float>(frame.resolutionLeft), static_cast<float>(frame.resolutionWidth), activeMetadataBrush, false, true);
        DrawTitleText(tutorialMetadata ? L"1.2 MB" : hideTutorialMetadata ? L"" : fileSizeText_, static_cast<float>(frame.fileSizeLeft), static_cast<float>(frame.fileSizeWidth), activeMetadataBrush, false, true);
        const float filenameWidth = static_cast<float>(std::max(0L,
            frame.titleBarContent.right - frame.filenameLeft - MulDiv(8, GetDpiForWindow(window_), 96)));
        DrawTitleText(tutorialMetadata ? L"viewtrious.png" : hideTutorialMetadata ? L"" : filenameText_, static_cast<float>(frame.filenameLeft), filenameWidth,
            tutorialMetadata ? tutorialMetadataBrush.Get() : filenameBrush.Get(), true, false);

        const float dpiScale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        if (EmptyStateActive()) {
            const UINT logoHeight = static_cast<UINT>(std::max(1.0f, std::round(std::min(18.0f * dpiScale,
                static_cast<float>(frame.titleBarHeight) - 12.0f * dpiScale))));
            const UINT logoWidth = static_cast<UINT>(std::max(1.0f, std::round(static_cast<float>(logoHeight) * 300.0f / 73.0f)));
            if (EnsureTopBarLogo(logoWidth, logoHeight)) {
                const float logoLeft = std::round((renderTarget_->GetSize().width - static_cast<float>(logoWidth)) * 0.5f);
                const float logoTop = std::round((static_cast<float>(frame.titleBarHeight) - static_cast<float>(logoHeight)) * 0.5f);
                renderTarget_->DrawBitmap(topBarLogo_.Get(), D2D1::RectF(logoLeft, logoTop, logoLeft + logoWidth, logoTop + logoHeight),
                    1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
        }

        const float stroke = 1.0f;
        const auto pixelCenter = [](float value) { return std::floor(value) + 0.5f; };
        renderTarget_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        const D2D1_POINT_2F hamburgerCenter = D2D1::Point2F((frame.hamburger.left + frame.hamburger.right) / 2.0f,
            (frame.hamburger.top + frame.hamburger.bottom) / 2.0f);
        const float hamburgerHalfWidth = 7.0f * dpiScale;
        const float hamburgerSpacing = 4.0f * dpiScale;
        for (int line = -1; line <= 1; ++line) {
            const float y = pixelCenter(hamburgerCenter.y + line * hamburgerSpacing);
            renderTarget_->DrawLine(D2D1::Point2F(pixelCenter(hamburgerCenter.x - hamburgerHalfWidth), y),
                D2D1::Point2F(pixelCenter(hamburgerCenter.x + hamburgerHalfWidth), y), glyphBrush.Get(), stroke);
        }
        renderTarget_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        DrawCaptionGlyph(L'\uE921', frame.minimize, glyphBrush.Get());
        DrawCaptionGlyph(IsZoomed(window_) ? L'\uE923' : L'\uE922', frame.maximize, glyphBrush.Get());
        ID2D1Brush* closeGlyph = (hoveredCaptionButton_ == CaptionButton::Close || pressedCaptionButton_ == CaptionButton::Close)
            ? closeGlyphBrush.Get() : glyphBrush.Get();
        DrawCaptionGlyph(L'\uE8BB', frame.close, closeGlyph);
    }

    void DiscardRenderResources() {
        bitmap_.Reset();
        lanczosBitmap_.Reset();
        aboutLogo_.Reset();
        aboutLogoWidth_ = 0;
        aboutLogoHeight_ = 0;
        topBarLogo_.Reset();
        topBarLogoWidth_ = 0;
        topBarLogoHeight_ = 0;
        checkerboardBrush_.Reset();
        checkerboardBitmap_.Reset();
        checkerboardDpi_ = 0;
        modelViewport_.Destroy();
        renderTarget_.Reset();
        graphicsHost_.Destroy();
    }

    const StartupTimer& timer_;
    HWND window_ = nullptr;
    ComPtr<IWICImagingFactory> wicFactory_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IWICBitmapSource> source_;
    ComPtr<IWICBitmapDecoder> gifDecoder_;
    std::shared_ptr<std::vector<BYTE>> displayedPixels_;
    std::shared_ptr<std::vector<BYTE>> gifCanvas_;
    std::shared_ptr<std::vector<BYTE>> gifPreviousCanvas_;
    std::shared_ptr<std::vector<BYTE>> lanczosPixels_;
    GraphicsHost graphicsHost_;
    VideoPlayer videoPlayer_;
    std::vector<GraphicsAdapterInfo> graphicsAdapters_;
    bool graphicsAdapterAuto_ = true;
    LUID graphicsAdapterLuid_{};
    ComPtr<ID2D1DeviceContext> renderTarget_;
    ComPtr<ID2D1Bitmap> bitmap_;
    ComPtr<ID2D1Bitmap> lanczosBitmap_;
    ComPtr<ID2D1Bitmap> aboutLogo_;
    UINT aboutLogoWidth_ = 0;
    UINT aboutLogoHeight_ = 0;
    ComPtr<ID2D1Bitmap> topBarLogo_;
    UINT topBarLogoWidth_ = 0;
    UINT topBarLogoHeight_ = 0;
    ComPtr<ID2D1Bitmap> checkerboardBitmap_;
    ComPtr<ID2D1BitmapBrush> checkerboardBrush_;
    UINT checkerboardDpi_ = 0;
    ComPtr<IDWriteTextFormat> titleTextFormat_;
    UINT titleTextDpi_ = 0;
    ComPtr<IDWriteTextFormat> captionIconFormat_;
    UINT captionIconDpi_ = 0;
    ComPtr<IDWriteTextFormat> zoomHudFormat_;
    UINT zoomHudDpi_ = 0;
    UINT imageWidth_ = 0;
    UINT imageHeight_ = 0;
    UINT gifCanvasWidth_ = 0;
    UINT gifCanvasHeight_ = 0;
    UINT gifFrameCount_ = 0;
    UINT gifFrameIndex_ = 0;
    UINT gifCompletedLoops_ = 0;
    UINT gifLoopCount_ = 0;
    UINT gifFrameDelayMs_ = 100;
    UINT gifPreviousDisposal_ = 0;
    UINT gifPreviousLeft_ = 0, gifPreviousTop_ = 0, gifPreviousWidth_ = 0, gifPreviousHeight_ = 0;
    UINT lanczosWidth_ = 0;
    UINT lanczosHeight_ = 0;
    D2D1_RECT_F lanczosDestination_{};
    std::wstring currentPath_;
    std::wstring displayedPath_;
    FileIdentity currentFileIdentity_{};
    std::wstring resolutionText_;
    std::wstring videoFramesPerSecondText_;
    uint64_t modelTriangleCount_ = 0;
    std::wstring fileSizeText_;
    std::wstring filenameText_;
    std::wstring error_;
    std::wstring rotationDiagnosticDetail_;
    std::wstring feedbackText_ = L"Copied to Clipboard";
    std::vector<fs::path> navigationFiles_;
    D2D1_POINT_2F pan_ = D2D1::Point2F();
    POINT lastDragPoint_{};
    float zoom_ = 1.0f;
    bool fitToWindow_ = true;
    bool gifPlaying_ = false;
    bool gifPaused_ = false;
    bool gifPlaybackTimerActive_ = false;
    bool gifVisible_ = true;
    bool gifHasLoopExtension_ = false;
    bool committingGifFrame_ = false;
    bool dragging_ = false;
    bool presented_ = false;
    bool navigationBuilt_ = false;
    bool navigationBuildQueued_ = false;
    std::atomic<uint64_t> decodeRequestGeneration_{ 0 };
    std::atomic<bool> decodeShuttingDown_{ false };
    uint64_t navigationFolderGeneration_ = 0;
    std::optional<DecodeRequest> pendingFullDecode_;
    std::vector<std::thread> fullDecodeThreads_;
    int fullDecodeWorkersInFlight_ = 0;
    bool imageDecodePending_ = false;
    uint64_t lanczosGeneration_ = 0;
    uint64_t lanczosViewGeneration_ = 0;
    bool lanczosSelected_ = false;
    bool lanczosRendering_ = false;
    std::optional<LanczosRequest> pendingLanczosRequest_;
    std::shared_ptr<std::atomic_bool> lanczosCancellation_;
    std::thread lanczosThread_;
    HANDLE directoryWatcherHandle_ = INVALID_HANDLE_VALUE;
    std::wstring directoryWatcherFolder_;
    std::thread directoryWatcherThread_;
    std::atomic<bool> directoryWatcherStopping_{ false };
    bool rememberWindowPlacement_ = true;
    bool includeHiddenImages_ = true;
    bool confirmBeforeDeleting_ = true;
    bool deleteWarningSuppressOnConfirm_ = false;
    bool showZoomPercentage_ = true;
    ZoomHudPosition zoomHudPosition_ = ZoomHudPosition::BottomRight;
    bool animationsEnabled_ = true;
    bool reverseMouseWheelZoom_ = false;
    bool spaceMouseEnabled_ = true;
    bool spaceMouseRuntimeAvailable_ = false;
    bool spaceMouseMotionActive_ = false;
    ULONGLONG modelHomeAnimationStartMs_ = 0;
    ULONGLONG modelAnimationDurationMs_ = kModelHomeAnimationDurationMs;
    std::wstring startupPath_;
#if defined(_DEBUG)
    LONGLONG lastModelNavLibTraceQpc_ = 0;
#endif
    std::unique_ptr<SpaceMouseNavigation> spaceMouse_;
    ContentKind contentKind_ = ContentKind::None;
    std::shared_ptr<ModelDocument> modelDocument_;
    D3D11ModelViewport modelViewport_;
    std::vector<ModelLoadWorker> modelLoadWorkers_;
    std::atomic<uint64_t> modelLoadGeneration_{ 0 };
    std::atomic<bool> shuttingDown_{ false };
    bool modelLoading_ = false;
    ULONGLONG modelLoadingStartedAtMs_ = 0;
#if defined(_DEBUG)
    bool offscreenIndicatorWasVisible_ = false;
    int offscreenIndicatorSector_ = -1;
#endif
    ThemePreference themePreference_ = ThemePreference::System;
    ImageScaling imageScaling_ = ImageScaling::Quality;
    ModelProjectionMode modelProjectionMode_ = ModelProjectionMode::Perspective;
    ModelVisualStyle modelVisualStyle_ = ModelVisualStyle::Shaded;
    ModelUpAxis modelUpAxis_ = ModelUpAxis::ZUp;
    ModelBuildPlate modelBuildPlate_ = ModelBuildPlate::Auto;
    ModelRenderingApi modelRenderingApi_ = ModelRenderingApi::Direct3D11;
    AxisIndicatorPosition axisIndicatorPosition_ = AxisIndicatorPosition::BottomRight;
    ModelAntiAliasing modelAntiAliasing_ = ModelAntiAliasing::Msaa4x;
    bool graphicsAdapterMenuOpen_ = false;
    mutable int graphicsAdapterMenuOption_ = -1;
    bool antiAliasingMenuOpen_ = false;
    bool upAxisMenuOpen_ = false;
    bool buildPlateMenuOpen_ = false;
    bool axisIndicatorPositionMenuOpen_ = false;
    bool projectionMenuOpen_ = false;
    bool zoomHudPositionMenuOpen_ = false;
    bool viewBarProjectionMenuOpen_ = false;
    bool viewBarVisualStyleMenuOpen_ = false;
    SettingsPage settingsPage_ = SettingsPage::General;
    float settingsScroll_ = 0.0f;
    bool onboardingRequired_ = false;
    bool tourPending_ = false;
    TutorialStep tutorialStep_ = TutorialStep::None;
    bool tutorialContextMenu_ = false;
    bool tutorialPresentation_ = false;
    bool tutorialPlacementSuppressed_ = false;
    bool tutorialWasFullscreen_ = false;
    bool tutorialWasMaximized_ = false;
    RECT tutorialWindowRect_{};
    float tutorialZoom_ = 1.0f;
    D2D1_POINT_2F tutorialPan_ = D2D1::Point2F();
    bool tutorialFitToWindow_ = true;
    bool fullscreen_ = false;
    CaptionButton hoveredCaptionButton_ = CaptionButton::None;
    CaptionButton pressedCaptionButton_ = CaptionButton::None;
    bool hamburgerHovered_ = false;
    bool hamburgerPressed_ = false;
    OverlayKind overlay_ = OverlayKind::None;
    bool dropdownOpen_ = false;
    bool triangleCountTooltipHovering_ = false;
    bool triangleCountTooltipVisible_ = false;
    DropdownItem dropdownHovered_ = DropdownItem::None;
    DropdownItem dropdownPressed_ = DropdownItem::None;
    bool contextMenuOpen_ = false;
    ContextAction contextHovered_ = ContextAction::None;
    ContextAction contextPressed_ = ContextAction::None;
    POINT contextMenuAnchor_{};
    Float3 selectedFaceNormal_{};
    Float3 selectedFaceHit_{};
    int selectedFacePlane_ = -1;
    bool modelFaceSelected_ = false;
    POINT modelClickStart_{};
    bool modelClickCandidate_ = false;
    std::wstring heifShellRotationCapabilityPath_;
    bool heifShellRotateLeftAvailable_ = false;
    bool heifShellRotateRightAvailable_ = false;
    bool shellRotationPending_ = false;
    ULONGLONG heifRotationCooldownUntil_ = 0;
    bool heifRotationMenuLocked_ = false;
    std::wstring shellRotationPath_;
    WIN32_FILE_ATTRIBUTE_DATA shellRotationInitialState_{};
    bool shellRotationInitialStateValid_ = false;
    WIN32_FILE_ATTRIBUTE_DATA shellRotationLastState_{};
    bool shellRotationLastStateValid_ = false;
    UINT shellRotationStableChecks_ = 0;
    UINT shellRotationProbeCount_ = 0;
    ULONGLONG shellRotationStarted_ = 0;
    bool openWithSubmenuOpen_ = false;
    int openWithHovered_ = -1;
    std::wstring openWithExtension_;
    std::vector<OpenWithHandler> openWithHandlers_;
    bool copyFeedbackActive_ = false;
    bool feedbackIsWallpaper_ = false;
    ULONGLONG copyFeedbackStart_ = 0;
    ButtonKind canvasNavigationHovered_ = ButtonKind::None;
    ButtonKind canvasNavigationPressed_ = ButtonKind::None;
    POINT canvasNavigationPressPoint_{};
    float canvasPreviousOpacity_ = 0.08f;
    float canvasNextOpacity_ = 0.08f;
    float canvasPreviousFadeStartOpacity_ = 0.08f;
    float canvasNextFadeStartOpacity_ = 0.08f;
    float canvasPreviousTargetOpacity_ = 0.08f;
    float canvasNextTargetOpacity_ = 0.08f;
    ULONGLONG canvasNavigationFadeStart_ = 0;
    bool canvasNavigationFadeActive_ = false;
    POINT lastMousePoint_{};
    ButtonKind videoControlsHovered_ = ButtonKind::None;
    bool videoControlsPointerOver_ = false;
    bool videoScrubbing_ = false;
    bool videoCursorHidden_ = false;
    float videoControlsOpacity_ = 0.0f;
    float videoControlsFadeStartOpacity_ = 1.0f;
    double videoScrubSeconds_ = 0.0;
    ULONGLONG videoControlsLastActivity_ = 0;
    ULONGLONG videoControlsFadeStart_ = 0;
    bool videoControlsFadeActive_ = false;
    ButtonKind hoveredButton_ = ButtonKind::None;
    ButtonKind pressedButton_ = ButtonKind::None;
    bool resetInProgress_ = false;
    LONG_PTR fullscreenStyle_ = 0;
    RECT fullscreenRect_{};
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* viewer = reinterpret_cast<Viewer*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        viewer = static_cast<Viewer*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(viewer));
        viewer->SetWindow(window);
    }
    if (!viewer) return DefWindowProcW(window, message, wParam, lParam);

    if (message == WM_GETMINMAXINFO) {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = GetDpiForWindow(window);
        info->ptMinTrackSize.x = MulDiv(640, dpi, 96);
        info->ptMinTrackSize.y = MulDiv(480, dpi, 96);
        return 0;
    }

    if (!viewer->IsFullscreen() && message == WM_NCCALCSIZE) return 0;
    if (!viewer->IsFullscreen() && message == WM_NCHITTEST) {
        const POINT screen{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }; RECT outer{}; GetWindowRect(window, &outer);
        FrameMetrics frame = GetFrameMetrics(window); const int x = screen.x - outer.left; const int y = screen.y - outer.top;
        if (!IsZoomed(window)) {
            const bool left = x < frame.border, right = x >= (outer.right - outer.left - frame.border), top = y < frame.border, bottom = y >= (outer.bottom - outer.top - frame.border);
            if (top && left) return HTTOPLEFT; if (top && right) return HTTOPRIGHT; if (bottom && left) return HTBOTTOMLEFT; if (bottom && right) return HTBOTTOMRIGHT;
            if (top) return HTTOP; if (bottom) return HTBOTTOM; if (left) return HTLEFT; if (right) return HTRIGHT;
        }
        POINT client = screen; ScreenToClient(window, &client);
        switch (CaptionButtonAt(frame, client)) {
        case CaptionButton::Minimize: return HTMINBUTTON;
        case CaptionButton::Maximize: return HTMAXBUTTON;
        case CaptionButton::Close: return HTCLOSE;
        case CaptionButton::None: break;
        }
        if (PtInRect(&frame.hamburger, client)) return HTCLIENT;
        if (client.y >= 0 && client.y < frame.titleBarHeight) return HTCAPTION;
        return HTCLIENT;
    }

    switch (message) {
    case WM_PAINT: viewer->Paint(); return 0;
    case WM_SIZE:
        viewer->Resize();
        viewer->GifPlaybackVisibilityChanged(wParam != SIZE_MINIMIZED);
        ApplyWindowCornerPreference(window, !viewer->IsFullscreen() && !IsZoomed(window));
        return 0;
    case WM_DPICHANGED: {
        if (!viewer->IsFullscreen()) {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_NCMOUSEMOVE: {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(window, &point);
        viewer->UpdateTriangleCountTooltipHover(point);
        viewer->SetCaptionButtonHover(CaptionButtonFromHitTest(wParam));
        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE | TME_NONCLIENT, window, 0 };
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_NCMOUSELEAVE: viewer->UpdateTriangleCountTooltipHover({ -1, -1 }); viewer->SetCaptionButtonHover(CaptionButton::None); return 0;
    case WM_NCLBUTTONDOWN: {
        const CaptionButton button = CaptionButtonFromHitTest(wParam);
        if (button == CaptionButton::None) break;
        viewer->SetCaptionButtonPressed(button);
        SetCapture(window);
        return 0;
    }
    case WM_NCLBUTTONUP: {
        const CaptionButton button = CaptionButtonFromHitTest(wParam);
        const CaptionButton pressed = viewer->PressedCaptionButton();
        viewer->ClearCaptionButtonPressed();
        if (GetCapture() == window) ReleaseCapture();
        if (pressed == button) SendMessageW(window, WM_SYSCOMMAND, SystemCommandForCaptionButton(window, button), 0);
        return 0;
    }
    case WM_DROPFILES: viewer->DropFile(reinterpret_cast<HDROP>(wParam)); return 0;
    case WM_MOUSEWHEEL: {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(window, &point);
        if (viewer->SettingsContains(point)) {
            const float wheelUnits = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
            viewer->ScrollSettings(-wheelUnits * MulDiv(54, GetDpiForWindow(window), 96));
            return 0;
        }
        if (viewer->HasOverlay() || viewer->DropdownOpen() || viewer->ContextMenuOpen() || viewer->ModelViewBarMenuOpen()) return 0;
        const float wheelUnits = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        if (viewer->ModelActive()) { viewer->DollyModel(wheelUnits); return 0; }
        viewer->ZoomAt(point, viewer->WheelZoomFactor(wheelUnits));
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        if (viewer->HasOverlay() || viewer->DropdownOpen() || viewer->ContextMenuOpen()) return 0;
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const ButtonKind navigation = viewer->CanvasNavigationZoneAt(point);
        if (navigation != ButtonKind::None) {
            viewer->BeginCanvasNavigationClick(navigation, point);
            SetCapture(window);
            return 0;
        }
        const ButtonKind button = viewer->ButtonAt(point);
        if (button != ButtonKind::None) {
            viewer->SetButtonPressed(button);
            SetCapture(window);
            return 0;
        }
        const FrameMetrics frame = GetFrameMetrics(window);
        if (!viewer->IsFullscreen() && PtInRect(&frame.hamburger, point)) {
            viewer->SetHamburgerPressed(true);
            SetCapture(window);
            return 0;
        }
        if (viewer->ModelActive()) { viewer->SelectAndFitModelObject(point); return 0; }
        if (viewer->HasImage() && viewer->ImageContains(point)) viewer->ToggleFitActualPixels(point);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (viewer->TutorialActive()) {
            const ButtonKind button = viewer->ButtonAt(point);
            if (button == ButtonKind::TutorialSkip || button == ButtonKind::TutorialNext) {
                viewer->SetButtonPressed(button);
                SetCapture(window);
            }
            return 0;
        }
        if (viewer->OpenWithSubmenuOpen()) {
            const int item = viewer->OpenWithItemAt(point);
            if (item >= 0) { viewer->InvokeOpenWithItem(item); return 0; }
            const ContextAction parent = viewer->ContextActionAt(point);
            if (parent != ContextAction::None) {
                if (parent != ContextAction::OpenWith) viewer->DismissOpenWithSubmenu();
                if (viewer->ContextActionEnabled(parent)) { viewer->SetContextPressed(parent); SetCapture(window); }
                return 0;
            }
            if (!viewer->OpenWithBridgeContains(point)) viewer->DismissContextMenu();
            return 0;
        }
        if (viewer->ContextMenuOpen()) {
            const FrameMetrics frame = GetFrameMetrics(window);
            if (!viewer->IsFullscreen() && PtInRect(&frame.hamburger, point)) {
                viewer->DismissContextMenu();
                viewer->SetHamburgerPressed(true);
                SetCapture(window);
                return 0;
            }
            const ContextAction action = viewer->ContextActionAt(point);
            if (action == ContextAction::None) viewer->DismissContextMenu();
            else if (viewer->ContextActionEnabled(action)) { viewer->SetContextPressed(action); SetCapture(window); }
            return 0;
        }
        if (viewer->DropdownOpen()) {
            const FrameMetrics frame = GetFrameMetrics(window);
            if (PtInRect(&frame.hamburger, point)) {
                viewer->DismissDropdown();
                return 0;
            }
            const DropdownItem item = viewer->DropdownItemAt(point);
            if (item == DropdownItem::None) viewer->DismissDropdown();
            else {
                viewer->SetDropdownPressed(item);
                SetCapture(window);
            }
            return 0;
        }
        if (viewer->HasOverlay()) {
            if (viewer->SettingsImageDropdownMenuOpen() && !viewer->SettingsImageDropdownMenuContains(point) && !viewer->SettingsDropdownControlContains(point)) {
                viewer->DismissSettingsImageDropdownMenu();
                return 0;
            }
            if (viewer->SettingsSimpleDropdownMenuOpen() && !viewer->SettingsSimpleDropdownMenuContains(point) && !viewer->SettingsDropdownControlContains(point)) {
                viewer->DismissSettingsSimpleDropdownMenu();
                return 0;
            }
            if (viewer->SettingsGraphicsAdapterMenuOpen() && !viewer->SettingsGraphicsAdapterMenuContains(point) && !viewer->SettingsDropdownControlContains(point)) {
                viewer->DismissSettingsGraphicsAdapterMenu();
                return 0;
            }
            if (viewer->SettingsAntiAliasingMenuOpen() && !viewer->SettingsAntiAliasingMenuContains(point) && !viewer->SettingsDropdownControlContains(point)) {
                viewer->DismissSettingsAntiAliasingMenu();
                return 0;
            }
            const ButtonKind button = viewer->ButtonAt(point);
            if (button != ButtonKind::None) { viewer->SetButtonPressed(button); SetCapture(window); return 0; }
            if (!viewer->OverlayContains(point) && !viewer->WelcomeOpen()) viewer->DismissOverlay();
            return 0;
        }
        if (viewer->ModelViewBarMenuOpen()) {
            const ButtonKind button = viewer->ButtonAt(point);
            if (button == ButtonKind::ViewBarProjectionPerspective || button == ButtonKind::ViewBarProjectionOrthographic || button == ButtonKind::ViewBarProjectionToggle || button == ButtonKind::ViewBarVisualStyleToggle || button == ButtonKind::ViewBarVisualStyleShaded || button == ButtonKind::ViewBarVisualStyleVisibleEdges || button == ButtonKind::ViewBarVisualStyleWireframe) { viewer->SetButtonPressed(button); SetCapture(window); }
            else viewer->DismissModelViewBarMenu();
            return 0;
        }
        if (viewer->BeginVideoControlsInteraction(point)) {
            SetCapture(window);
            return 0;
        }
        const ButtonKind button = viewer->ButtonAt(point);
        if (button != ButtonKind::None) {
            viewer->SetButtonPressed(button);
            SetCapture(window);
            return 0;
        }
        const FrameMetrics frame = GetFrameMetrics(window);
        if (const ButtonKind navigation = viewer->CanvasNavigationZoneAt(point); navigation != ButtonKind::None) {
            viewer->BeginCanvasNavigationClick(navigation, point);
            SetCapture(window);
        } else if (PtInRect(&frame.hamburger, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) })) {
            viewer->SetHamburgerPressed(true);
            SetCapture(window);
        } else {
            if (viewer->ModelActive()) { viewer->BeginModelOrbit({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }); SetCapture(window); }
            else viewer->BeginPan({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        }
        return 0;
    }
    case WM_MBUTTONDOWN:
        if (!viewer->HasOverlay() && !viewer->DropdownOpen() && !viewer->ContextMenuOpen() && viewer->ModelActive()) {
            viewer->BeginModelPan({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            SetCapture(window);
            return 0;
        }
        break;
    case WM_MBUTTONDBLCLK:
        if (!viewer->HasOverlay() && !viewer->DropdownOpen() && !viewer->ContextMenuOpen() && viewer->ModelActive()) {
            viewer->BeginAnimatedModelHome();
            return 0;
        }
        break;
    case WM_MOUSEMOVE: {
        viewer->UpdateTriangleCountTooltipHover({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        if (viewer->TutorialActive()) {
            viewer->SetButtonHover(viewer->ButtonAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }));
            return 0;
        }
        if (viewer->ContextMenuOpen()) {
            const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const ContextAction parent = viewer->ContextActionAt(point);
            const int child = viewer->OpenWithItemAt(point);
            if (child >= 0) {
                viewer->SetOpenWithHover(child);
                viewer->SetContextHover(ContextAction::None);
            } else {
                viewer->SetOpenWithHover(-1);
                viewer->SetContextHover(parent);
                if (viewer->OpenWithSubmenuOpen() && parent != ContextAction::OpenWith &&
                    parent != ContextAction::None && !viewer->OpenWithBridgeContains(point)) {
                    viewer->DismissOpenWithSubmenu();
                }
            }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        if (viewer->DropdownOpen()) {
            viewer->SetDropdownHover(viewer->DropdownItemAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }));
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        if (viewer->HasOverlay()) {
            viewer->SetButtonHover(viewer->ButtonAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }));
            viewer->SetHamburgerHover(false);
            return 0;
        }
        if (viewer->VideoActive()) {
            const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const FrameMetrics frame = GetFrameMetrics(window);
            viewer->SetHamburgerHover(!viewer->IsFullscreen() && PtInRect(&frame.hamburger, point));
            viewer->UpdateVideoControlsMouse(point);
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        const FrameMetrics frame = GetFrameMetrics(window);
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        viewer->SetButtonHover(viewer->ButtonAt(point));
        viewer->SetCanvasNavigationHover(viewer->CanvasNavigationZoneAt(point));
        viewer->SetHamburgerHover(!viewer->IsFullscreen() && PtInRect(&frame.hamburger, point));
        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
        TrackMouseEvent(&track);
        if (viewer->CanvasNavigationPressed()) {
            viewer->ContinueCanvasNavigationClick(point);
            return 0;
        }
        if (!viewer->HamburgerPressed() && viewer->PressedButton() == ButtonKind::None) {
            if (viewer->ModelActive()) {
                if (wParam & (MK_LBUTTON | MK_MBUTTON)) viewer->ContinueModelDrag(point);
                else viewer->EndModelDrag();
            } else viewer->PanTo(point);
        }
        return 0;
    }
    case WM_MOUSELEAVE: viewer->UpdateTriangleCountTooltipHover({ -1, -1 }); viewer->SetHamburgerHover(false); viewer->SetButtonHover(ButtonKind::None); viewer->SetCanvasNavigationHover(ButtonKind::None); viewer->SetDropdownHover(DropdownItem::None); viewer->SetContextHover(ContextAction::None); viewer->VideoControlsMouseLeave(); return 0;
    case WM_LBUTTONUP: {
        if (viewer->EndVideoControlsInteraction({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) })) {
            if (GetCapture() == window) ReleaseCapture();
            return 0;
        }
        if (viewer->CanvasNavigationPressed()) {
            const ButtonKind navigation = viewer->FinishCanvasNavigationClick({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            if (GetCapture() == window) ReleaseCapture();
            if (navigation != ButtonKind::None) viewer->InvokeButton(navigation);
            return 0;
        }
        if (viewer->PressedButton() != ButtonKind::None) {
            const ButtonKind pressed = viewer->PressedButton();
            const ButtonKind released = viewer->ButtonAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            const bool openingFile = pressed == ButtonKind::EmptyOpenFile && pressed == released;
            viewer->ClearButtonPressed(!openingFile);
            if (GetCapture() == window) ReleaseCapture();
            if (!openingFile) viewer->SetButtonHover(released);
            if (pressed == released) viewer->InvokeButton(pressed);
            return 0;
        }
        if (viewer->PressedContextAction() != ContextAction::None) {
            const ContextAction pressed = viewer->PressedContextAction();
            const ContextAction released = viewer->ContextActionAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            viewer->ClearContextPressed();
            if (GetCapture() == window) ReleaseCapture();
            if (pressed == released && viewer->ContextActionEnabled(pressed)) viewer->InvokeContextAction(pressed);
            return 0;
        }
        if (viewer->PressedDropdownItem() != DropdownItem::None) {
            const DropdownItem pressed = viewer->PressedDropdownItem();
            const DropdownItem released = viewer->DropdownItemAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            const bool openingFile = pressed == DropdownItem::OpenFile && pressed == released;
            viewer->ClearDropdownPressed(!openingFile);
            if (GetCapture() == window) ReleaseCapture();
            if (pressed == released) viewer->InvokeDropdownItem(pressed);
            return 0;
        }
        if (viewer->HamburgerPressed()) {
            const FrameMetrics frame = GetFrameMetrics(window);
            const bool releasedOnHamburger = PtInRect(&frame.hamburger, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            viewer->SetHamburgerPressed(false);
            if (GetCapture() == window) ReleaseCapture();
            if (releasedOnHamburger) viewer->ToggleDropdown();
            return 0;
        }
        const CaptionButton pressed = viewer->PressedCaptionButton();
        if (pressed != CaptionButton::None) {
            const FrameMetrics frame = GetFrameMetrics(window);
            const CaptionButton released = CaptionButtonAt(frame, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            viewer->ClearCaptionButtonPressed();
            if (GetCapture() == window) ReleaseCapture();
            if (pressed == released) SendMessageW(window, WM_SYSCOMMAND, SystemCommandForCaptionButton(window, released), 0);
            return 0;
        }
        if (viewer->ModelActive()) {
            viewer->FinishModelSelectionClick({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            if (GetCapture() == window) ReleaseCapture();
            return 0;
        }
        viewer->EndPan(); viewer->EndModelDrag(); if (GetCapture() == window) ReleaseCapture(); return 0;
    }
    case WM_MBUTTONUP:
        if (viewer->ModelActive()) {
            viewer->EndModelDrag();
            if (GetCapture() == window) ReleaseCapture();
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        viewer->EndPan(); viewer->EndModelDrag(); viewer->CancelCanvasNavigationClick(); viewer->CancelVideoControlsInteraction(); viewer->ClearCaptionButtonPressed(); viewer->ClearButtonPressed(); viewer->SetHamburgerPressed(false); viewer->ClearDropdownPressed(); viewer->ClearContextPressed(); return 0;
    case WM_RBUTTONUP: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (!viewer->TutorialActive()) { if (viewer->ModelActive()) viewer->SelectModelFace(point); viewer->OpenContextMenu(point); }
        return 0;
    }
    case WM_TIMER:
        if (wParam == kGifPlaybackTimer) { viewer->GifPlaybackTimerMessage(); return 0; }
        if (wParam == kCopyFeedbackTimer) { viewer->UpdateCopyFeedback(); return 0; }
        if (wParam == kCanvasNavigationFadeTimer) { viewer->UpdateCanvasNavigationFade(); return 0; }
        if (wParam == kDirectoryChangeDebounceTimer) { KillTimer(window, kDirectoryChangeDebounceTimer); viewer->RefreshNavigationFromFileSystem(); return 0; }
        if (wParam == kNavigationDecodeDebounceTimer) { viewer->NavigationDecodeTimer(); return 0; }
        if (wParam == kShellRotationCheckTimer) { viewer->ShellRotationTimer(); return 0; }
        if (wParam == kHeifRotationMenuRefreshTimer) { viewer->HeifRotationMenuRefreshTimer(); return 0; }
        if (wParam == kLanczosSettleTimer) { KillTimer(window, kLanczosSettleTimer); viewer->LanczosRefinementTimer(); return 0; }
        if (wParam == kModelHomeAnimationTimer) { viewer->UpdateAnimatedModelHome(); return 0; }
        if (wParam == kModelLoadingAnimationTimer) { viewer->ModelLoadingAnimationTimerMessage(); return 0; }
        if (wParam == kTriangleCountTooltipTimer) { viewer->TriangleCountTooltipTimerMessage(); return 0; }
        if (wParam == kVideoPlaybackTimer) { viewer->VideoPlaybackTimerMessage(); return 0; }
        if (wParam == kVideoControlsTimer) { viewer->UpdateVideoControlsFade(); return 0; }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {
            viewer->RefreshNavigationFromFileSystem();
            viewer->ResumePendingTour();
        }
        break;
    case WM_SHOWWINDOW:
        viewer->GifPlaybackVisibilityChanged(wParam != FALSE && !IsIconic(window));
        break;
    case WM_SETTINGCHANGE: ApplyTitleBarTheme(window); return 0;
    case kBuildNavigationMessage: viewer->BuildNavigation(); return 0;
    case kDirectoryChangedMessage: viewer->QueueDirectoryRefreshFromWatcher(); return 0;
    case kFullDecodeCompleteMessage: viewer->FullDecodeCompleteMessage(reinterpret_cast<FullDecodeResult*>(lParam)); return 0;
    case kLanczosCompleteMessage: viewer->LanczosCompleteMessage(reinterpret_cast<LanczosResult*>(lParam)); return 0;
    case kDecodeWorkerFinishedMessage: viewer->DecodeWorkerFinishedMessage(reinterpret_cast<DecodeWorkerFinished*>(lParam)); return 0;
    case kModelLoadCompleteMessage: viewer->ModelLoadCompleteMessage(reinterpret_cast<ModelLoadResult*>(lParam)); return 0;
    case kVideoMediaEngineEventMessage: viewer->VideoMediaEngineEvent(static_cast<DWORD>(wParam)); return 0;
    case WM_KEYDOWN:
        if (viewer->TutorialActive()) { if (wParam == VK_ESCAPE) viewer->StopTutorial(); return 0; }
        if (viewer->OpenWithSubmenuOpen()) { if (wParam == VK_ESCAPE) viewer->DismissOpenWithSubmenu(); return 0; }
        if (viewer->ModelViewBarMenuOpen()) { if (wParam == VK_ESCAPE) viewer->DismissModelViewBarMenu(); return 0; }
        if (viewer->ContextMenuOpen()) {
            if (wParam == VK_ESCAPE) viewer->DismissContextMenu();
            return 0;
        }
        if (viewer->DropdownOpen()) {
            if (wParam == VK_ESCAPE) viewer->DismissDropdown();
            return 0;
        }
        if (viewer->HasOverlay()) {
            if (wParam == VK_ESCAPE && !viewer->WelcomeOpen()) viewer->DismissOverlay();
            return 0;
        }
        if (GetKeyState(VK_CONTROL) < 0 && wParam == L'O') { viewer->OpenFile(); return 0; }
        if (GetKeyState(VK_CONTROL) < 0 && wParam == L'C') { viewer->InvokeContextAction(ContextAction::Copy); return 0; }
        if (GetKeyState(VK_CONTROL) < 0 && wParam == L'P') { viewer->InvokeContextAction(ContextAction::Print); return 0; }
        if (wParam == VK_SPACE && viewer->VideoActive()) { viewer->ToggleVideoPlayPause(); return 0; }
        if (wParam == VK_DELETE) { viewer->InvokeContextAction(ContextAction::Delete); return 0; }
        if (wParam == VK_ESCAPE) { if (viewer->IsFullscreen()) viewer->ToggleFullscreen(); else DestroyWindow(window); return 0; }
        if (wParam == VK_F11) { viewer->ToggleFullscreen(); return 0; }
        if (wParam == VK_RIGHT || wParam == VK_LEFT) {
            const bool autoRepeat = (static_cast<DWORD_PTR>(lParam) & (DWORD_PTR{ 1 } << 30)) != 0;
            viewer->DrainQueuedFullDecodeResults();
            viewer->Navigate(wParam == VK_RIGHT ? 1 : -1, autoRepeat);
            return 0;
        }
        if (wParam == VK_OEM_PLUS || wParam == VK_ADD || wParam == L'=') { viewer->ZoomCentered(kWheelZoomStep); return 0; }
        if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) { viewer->ZoomCentered(1.0f / kWheelZoomStep); return 0; }
        if (wParam == L'0' || wParam == VK_NUMPAD0) { if (viewer->ModelActive()) viewer->FitModel(); else viewer->FitToWindow(); return 0; }
        break;
    case WM_DESTROY: viewer->SaveWindowPlacement(); viewer->Shutdown(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    StartupTimer timer;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;

    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    const std::wstring path = (arguments && argumentCount > 1) ? arguments[1] : L"";
    if (arguments) LocalFree(arguments);

    Viewer viewer(timer);
    viewer.Initialize(path);

    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.style = CS_DBLCLKS;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.style = CS_DBLCLKS;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    windowClass.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(101));
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassExW(&windowClass);

    const SIZE client = RememberWindowPlacementEnabled() ? viewer.SuggestedClientSize() : SIZE{ 800, 600 };
    RECT bounds{ 0, 0, client.cx, client.cy };
    AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0);
    SavedPlacement savedPlacement{};
    const bool hasSavedPlacement = LoadPlacement(savedPlacement);
    if (hasSavedPlacement) {
        bounds = savedPlacement.rect;
        MakePlacementVisible(bounds);
    } else {
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const LONG width = std::min(bounds.right - bounds.left, monitor.rcWork.right - monitor.rcWork.left);
        const LONG height = std::min(bounds.bottom - bounds.top, monitor.rcWork.bottom - monitor.rcWork.top);
        bounds.left = monitor.rcWork.left + ((monitor.rcWork.right - monitor.rcWork.left) - width) / 2;
        bounds.top = monitor.rcWork.top + ((monitor.rcWork.bottom - monitor.rcWork.top) - height) / 2;
        bounds.right = bounds.left + width;
        bounds.bottom = bounds.top + height;
    }
    const DWORD windowStyle = WS_OVERLAPPEDWINDOW;
    HWND window = CreateWindowExW(0, kWindowClass, kWindowTitle, windowStyle,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, &viewer);
    if (!window) { CoUninitialize(); return 1; }

    SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(windowClass.hIcon));
    SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(windowClass.hIconSm));
    DragAcceptFiles(window, TRUE);
    ApplyTitleBarTheme(window);
    ApplyWindowCornerPreference(window, true);
    viewer.ShowWelcomeIfNeeded();
    ShowWindow(window, hasSavedPlacement && savedPlacement.maximized ? SW_MAXIMIZE : showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
