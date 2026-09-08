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
#include <ole2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <SpaceMouse/CNavigation3D.hpp>

#include "lanczos_resampler.h"
#include "d3d11_model_viewport.h"
#include "video_player.h"
#include "shell_thumbnail_reader.h"
#include "video_hover_frame_stream.h"
#include "stl_loader.h"
#include "three_mf_loader.h"
#include "model_importer.h"
#include "ai_addon_loader.h"
#include "adjustment_persistence.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <unordered_map>
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
constexpr UINT kVideoPlaybackWakeMessage = WM_APP + 10;
constexpr UINT kAiAnalysisCompleteMessage = WM_APP + 11;
constexpr UINT kImageAdjustmentPersistenceCompleteMessage = WM_APP + 15;
constexpr UINT kFilmstripThumbnailCompleteMessage = WM_APP + 12;
constexpr UINT kFilmstripScrollWakeMessage = WM_APP + 13;
constexpr UINT kFilmstripHoverPreviewCompleteMessage = WM_APP + 14;
// A small pool prevents one expensive WIC decode (for example HEIC or DNG) from
// holding up every later visible filmstrip thumbnail, without creating an
// unbounded background decode workload.
constexpr size_t kFilmstripThumbnailWorkerCount = 2;
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is available on Windows 10 version 1803 and later.
constexpr DWORD kHighResolutionWaitableTimerFlag = 0x00000002;
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
constexpr UINT_PTR kVideoControlsTimer = 15;
constexpr UINT_PTR kVideoStepHoldTimer = 16;
constexpr UINT_PTR kStillDissolveTimer = 17;
constexpr UINT kVideoStepHoldThresholdMs = 250;
constexpr UINT kVideoStepHoldIntervalMs = 16;
constexpr UINT kShellRotationCheckIntervalMs = 100;
constexpr ULONGLONG kShellRotationTimeoutMs = 10000;
constexpr ULONGLONG kHeifRotationCooldownMs = 0;
constexpr int kTopBarLogoResourceId = 102;
constexpr int kFilmstripVideoIconGroupResourceId = 104;
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
constexpr ULONGLONG kStillDissolveDurationMs = 320;
constexpr ULONGLONG kStillDissolvePreviewWaitMaxMs = 450;
constexpr UINT_PTR kFilmstripVisibilityTimer = 3;
constexpr UINT_PTR kFilmstripHoverPreviewTimer = 18;
constexpr UINT_PTR kFilmstripHoverPreviewDwellTimer = 19;
constexpr UINT_PTR kFilmstripVideoHoverFadeTimer = 20;
constexpr UINT_PTR kImageAdjustmentPersistenceTimer = 21;
constexpr UINT_PTR kFilmstripHoverPreviewFadeTimer = 22;
constexpr ULONGLONG kFilmstripVideoHoverFadeDurationMs = 175;
constexpr ULONGLONG kFilmstripHoverPreviewFadeDurationMs = kStillDissolveDurationMs;
constexpr double kFilmstripWheelImpulseDipsPerSecond = 1500.0;
constexpr double kFilmstripMaximumVelocityDipsPerSecond = 4800.0;
constexpr double kFilmstripVelocityDampingPerSecond = 28.0;
constexpr double kFilmstripVelocityStopEpsilon = 20.0;
constexpr std::array<DWORD, 6> kVideoPlaybackRatePercents{ 25, 50, 100, 125, 150, 200 };

float SmoothTransitionProgress(float progress) {
    progress = std::clamp(progress, 0.0f, 1.0f);
    return progress * progress * (3.0f - 2.0f * progress);
}
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


enum class OverlayKind { None, KeyboardShortcuts, About, Settings, ResetConfirm, DeleteConfirm, Welcome, DefaultAppsHelper, Feedback, Help, PrintError, RegistrationError };
enum class DropdownItem { None, OpenFile, Settings, QuickTour, KeyboardShortcuts, Help, About, Feedback, Close };
enum class ContextAction { None, Fullscreen, RotateLeft, RotateRight, OpenWith, Copy, Print, SetBackground, Delete, SnapViewToFace };
enum class ButtonKind { None, EmptyOpenFile, CanvasPrevious, CanvasNext, SettingsGeneralPage, SettingsImage2DPage, SettingsVideoPage, SettingsModel3DPage, SettingsRememberPlacement, SettingsIncludeHidden,
    SettingsConfirmDelete, SettingsSwipeToNavigateWhenFit, SettingsShowZoomHud, SettingsAnimations, SettingsReverseWheelZoom, SettingsAlwaysShowFilmstrip, SettingsThemeSystem, SettingsThemeLight, SettingsThemeDark,
    SettingsZoomHudPositionToggle, SettingsZoomHudBottomLeft, SettingsZoomHudBottomRight, SettingsZoomHudTopLeft, SettingsZoomHudTopRight, SettingsImageScalingToggle, SettingsScrollUp, SettingsScrollDown,
    SettingsSpaceMouse, SettingsUpAxisToggle, SettingsUpAxisZ, SettingsUpAxisY, SettingsUpAxisX, SettingsBuildPlateToggle, SettingsBuildPlateAuto, SettingsBuildPlateOn, SettingsBuildPlateOff, SettingsAxisIndicatorPositionToggle, SettingsAxisIndicatorBottomLeft, SettingsAxisIndicatorBottomRight, SettingsAxisIndicatorTopLeft, SettingsAxisIndicatorTopRight, SettingsProjectionToggle, SettingsProjectionPerspective, SettingsProjectionOrthographic, SettingsGraphicsAdapterToggle, SettingsGraphicsAdapterOption, SettingsAntiAliasingToggle, SettingsAntiAliasingOff, SettingsAntiAliasing2x, SettingsAntiAliasing4x, SettingsAntiAliasing8x, SettingsAntiAliasingSsaa1_5x, SettingsAntiAliasingSsaa2x, ModelOffscreenIndicator, ViewBarProjectionToggle, ViewBarProjectionPerspective, ViewBarProjectionOrthographic, ViewBarVisualStyleToggle, ViewBarVisualStyleShaded, ViewBarVisualStyleVisibleEdges, ViewBarVisualStyleWireframe, SettingsScalingPerformance, SettingsScalingQuality, SettingsDefaultApps, SettingsReset, ResetCancel, ResetConfirm, DeleteWarningSuppress, DeleteCancel, DeleteConfirm, WelcomeSecondary, WelcomePrimary, FeedbackBug,
    DefaultAppsHelperCancel, DefaultAppsHelperOpen, FeedbackFeature, HelpClose, HelpTopic, PrintErrorDismiss, TutorialSkip, TutorialNext, VideoPlayPause, VideoStepBackward, VideoStepForward, VideoMute, VideoAdjustments, VideoPlaybackSpeed, VideoFullscreen, ImageAdjustments };
enum class TutorialStep { None, OpenImages, ResizeWindow, MenuSettings, ImageDetails, ContextMenu, Shortcuts };
enum class ThemePreference : DWORD { System = 0, Light = 1, Dark = 2 };
enum class ImageScaling : DWORD { Performance = 0, Quality = 1 };
enum class ModelRenderingApi : DWORD { Direct3D11 = 0 };
enum class AxisIndicatorPosition : DWORD { BottomLeft = 0, BottomRight = 1, TopLeft = 2, TopRight = 3 };
enum class ZoomHudPosition : DWORD { BottomLeft = 0, BottomRight = 1, TopLeft = 2, TopRight = 3 };
enum class SettingsPage { General, Image2D, Video2D, Model3D };
enum class ContentKind { None, Image2D, Model3D, Video2D };
enum class FilmstripVisibilityState { Hidden, Revealing, Holding, Fading };

struct ShortcutEntry { const wchar_t* shortcut; const wchar_t* description; };
struct HelpSection { const wchar_t* heading; const wchar_t* body; };
struct HelpTopic { const wchar_t* title; const HelpSection* sections; size_t sectionCount; const wchar_t* body; };
constexpr std::array<HelpSection, 3> kGettingStartedSections{{
    { L"open a file", L"use open file, drag and drop a supported file into Viewtrious, or open an associated file from Windows Explorer." },
    { L"browse the folder", L"after opening a file, Viewtrious can move between other supported sibling files in that folder." },
    { L"learn the controls", L"use quick tutorial for the visual walkthrough and keyboard shortcuts for the complete shortcut reference." },
}};
constexpr std::array<HelpSection, 4> kImageViewingSections{{
    { L"fit and zoom", L"images open fitted to the available viewing area. use the mouse wheel to zoom, and double-click the image to switch between fitted view and 100% scale." },
    { L"pan", L"when zoomed in, drag the image to pan." },
    { L"folder navigation", L"the left and right viewer controls move between supported files in the current folder." },
    { L"transparency", L"transparent image areas use the Viewtrious checkerboard background." },
}};
constexpr std::array<HelpSection, 4> kVideoAndAnimationSections{{
    { L"playback", L"supported video files play inside the normal Viewtrious viewer. use the playback controls or Space to play and pause." },
    { L"seeking", L"use the scrubber to seek through video; the controls show elapsed time and duration." },
    { L"folder navigation", L"compatible 2D media stays together, so images, video, and animated media can be browsed naturally from the same folder." },
    { L"animation", L"GIF files are handled as animated media rather than static images." },
}};
constexpr std::array<HelpSection, 4> kModelViewingSections{{
    { L"orbit", L"use the mouse to orbit a supported 3D model." },
    { L"pan", L"use the middle mouse button to pan the model view." },
    { L"zoom", L"use the mouse wheel to move closer to or farther from the model." },
    { L"reset view", L"press 0 to reset the model view." },
}};
constexpr std::array<HelpSection, 3> kSpaceMouseSections{{
    { L"enable SpaceMouse", L"enable or disable compatible 3Dconnexion SpaceMouse devices under 3D settings." },
    { L"3D navigation", L"in the 3D viewer, SpaceMouse provides analog model navigation." },
    { L"2D navigation", L"in the 2D image viewer, supported motion can pan and zoom the image." },
}};
constexpr std::array<HelpSection, 1> kKeyboardShortcutSections{{
    { L"full shortcut list", L"keyboard shortcuts remains a direct main-menu item and contains the complete shortcut reference." },
}};
constexpr std::array<HelpSection, 1> kQuickTutorialSections{{
    { L"interactive walkthrough", L"quick tutorial is a short visual introduction to the main Viewtrious controls; help provides the more complete reference." },
}};
constexpr std::array<HelpSection, 3> kSupportedFileTypeSections{{
    { L"images", L"PNG, JPEG, BMP, TIFF, ICO, WebP, HEIC, HEIF, AVIF, DNG, CR2, CR3, NEF, ARW, RAF" },
    { L"video", L"MP4, MOV, MKV, GIF" },
    { L"3D", L"STL, 3MF; STEP and STP when the optional Open CASCADE Technology add-on is installed." },
}};
constexpr std::array<HelpSection, 2> kFileAssociationSections{{
    { L"choose defaults", L"use the Viewtrious setup flow or Windows Settings to choose which supported file types open with Viewtrious." },
    { L"change them later", L"changing a file association does not modify the file; it only changes which application Windows uses to open it." },
}};
constexpr std::array<HelpSection, 3> kDeletingFileSections{{
    { L"confirmation", L"when deletion confirmation is enabled, Viewtrious asks before deleting a file. change this option in general settings." },
    { L"after deletion", L"after a file is deleted successfully, Viewtrious continues to an appropriate neighboring file when one is available." },
    { L"delete behavior", L"deleted files are moved to the Windows Recycle Bin rather than permanently deleted." },
}};
constexpr std::array<HelpSection, 4> kSettingsSections{{
    { L"general", L"application-wide behavior." },
    { L"2D settings", L"options affecting image and other 2D viewing." },
    { L"video settings", L"Video2D-specific options will appear here as they are added." },
    { L"3D settings", L"options affecting model viewing, navigation, and SpaceMouse support." },
}};
constexpr std::array<HelpSection, 2> kFeedbackAndAboutSections{{
    { L"feedback", L"use feedback from the main menu for the current Viewtrious feedback and project links." },
    { L"about", L"use about for the Viewtrious version and application information." },
}};
constexpr std::array<HelpSection, 5> kTroubleshootingSections{{
    { L"a file will not open", L"confirm that the file type is supported and that the file itself can be read normally by Windows." },
    { L"video will not play", L"a video file can contain a codec unavailable through the Windows media components used by Viewtrious. a supported extension does not guarantee every codec can be decoded." },
    { L"a 3D model will not open", L"confirm that the format is supported and that the file contains valid model geometry." },
    { L"SpaceMouse does not respond", L"confirm that SpaceMouse is enabled under 3D settings and that 3Dconnexion software recognizes the device." },
    { L"Viewtrious behaves unexpectedly", L"use feedback from the main menu and include the file type and steps to reproduce the problem." },
}};
constexpr std::array<HelpTopic, 14> kHelpTopics{{
    { L"getting started", kGettingStartedSections.data(), kGettingStartedSections.size(), L"" },
    { L"image viewing", kImageViewingSections.data(), kImageViewingSections.size(), L"" },
    { L"video and animation", kVideoAndAnimationSections.data(), kVideoAndAnimationSections.size(), L"" },
    { L"3D viewing", kModelViewingSections.data(), kModelViewingSections.size(), L"" },
    { L"SpaceMouse", kSpaceMouseSections.data(), kSpaceMouseSections.size(), L"" },
    { L"keyboard shortcuts", kKeyboardShortcutSections.data(), kKeyboardShortcutSections.size(), L"" },
    { L"quick tutorial", kQuickTutorialSections.data(), kQuickTutorialSections.size(), L"" },
    { L"supported file types", kSupportedFileTypeSections.data(), kSupportedFileTypeSections.size(), L"" },
    { L"file associations", kFileAssociationSections.data(), kFileAssociationSections.size(), L"" },
    { L"deleting files", kDeletingFileSections.data(), kDeletingFileSections.size(), L"" },
    { L"settings", kSettingsSections.data(), kSettingsSections.size(), L"" },
    { L"troubleshooting", kTroubleshootingSections.data(), kTroubleshootingSections.size(), L"" },
    { L"feedback and about", kFeedbackAndAboutSections.data(), kFeedbackAndAboutSections.size(), L"" },
    { L"third-party notices", nullptr, 0, L"3D input device development tools and related technology are provided under license from 3Dconnexion. (c) 3Dconnexion 1992 - 2025. All rights reserved.\n\nOpen CASCADE Technology support is provided by the optional STEP add-on under GNU LGPL version 2.1 with the Open CASCADE exception." },
}};
struct OpenWithHandler { std::wstring name; ComPtr<IAssocHandler> handler; };
struct PixelBuffer {
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    bool hasTransparency = false;
    std::shared_ptr<std::vector<BYTE>> pixels;
};

bool PixelsHaveTransparency(const std::vector<BYTE>& pixels) {
    for (size_t offset = 3; offset < pixels.size(); offset += 4) {
        if (pixels[offset] != 255) return true;
    }
    return false;
}
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
struct AiAnalysisResult { uint64_t generation = 0; std::wstring path; ContentKind contentKind = ContentKind::None; std::thread::id workerId{}; ViewtriousAiAdjustmentResultV1 adjustments{}; bool succeeded = false; };
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
struct FilmstripThumbnailRequest {
    std::wstring path;
    uint64_t folderGeneration = 0;
    uint64_t itemGeneration = 0;
    UINT targetHeight = 0;
};
struct FilmstripThumbnailResult : PixelBuffer {
    FilmstripThumbnailRequest request;
    float aspect = 1.0f;
    HRESULT result = E_FAIL;
};
struct FilmstripThumbnailEntry : PixelBuffer {
    std::wstring path;
    uint64_t itemGeneration = 0;
    float aspect = 1.0f;
    // Derived solely from pixels; it cannot retain the source file or WIC objects.
    ComPtr<ID2D1Bitmap> bitmap;
};
struct FilmstripHoverPreviewRequest {
    std::wstring path;
    uint64_t folderGeneration = 0;
    uint64_t itemGeneration = 0;
    uint64_t hoverGeneration = 0;
};
struct FilmstripHoverPreviewResult : PixelBuffer {
    FilmstripHoverPreviewRequest request;
    float aspect = 1.0f;
    HRESULT result = E_FAIL;
    bool videoFrame = false;
    bool videoFinished = false;
    LONGLONG videoTimestamp = 0;
};
struct FilmstripHoverPreviewEntry : PixelBuffer {
    std::wstring path;
    uint64_t itemGeneration = 0;
    float aspect = 1.0f;
    uint64_t lastUse = 0;
    // Derived solely from copied RAM pixels; it never retains source ownership.
    ComPtr<ID2D1Bitmap> bitmap;
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
constexpr std::array<ShortcutEntry, 12> kKeyboardShortcutEntries{{
    { L"Ctrl + O", L"Open file" }, { L"Ctrl + C", L"Copy media" }, { L"Ctrl + P", L"Print" }, { L"Delete", L"Move media to Recycle Bin" },
    { L"Esc", L"Exit fullscreen, or close Viewtrious" }, { L"Left Arrow", L"Previous media" }, { L"Right Arrow", L"Next media" }, { L"+", L"Zoom in" },
    { L"-", L"Zoom out" }, { L"0", L"Reset zoom to center" }, { L"F11", L"Fullscreen" }, { L"Space", L"Play / pause video" },
}};
constexpr std::array<ShortcutEntry, 4> kMouseNavigationEntries{{
    { L"Mouse Wheel (2D)", L"Zoom in / out" }, { L"Left Mouse Drag (2D)", L"Pan" },
    { L"Right Mouse Click", L"Open right-click menu" }, { L"Double-click video", L"Toggle fullscreen" },
}};
constexpr size_t kShortcutEntryCount = kKeyboardShortcutEntries.size() + kMouseNavigationEntries.size();
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
        extension == L".nef" || extension == L".arw" || extension == L".raf" || extension == L".mp4" || extension == L".mov" || extension == L".mkv" || extension == L".stl" || extension == L".3mf" || extension == L".step" || extension == L".stp";
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
bool IsVideoPath(const std::wstring& path) { const std::wstring extension = LowercaseExtension(path); return extension == L".mp4" || extension == L".mov" || extension == L".mkv"; }
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

void TraceRegistryFailure(const wchar_t* operation, const wchar_t* path, const wchar_t* name, LONG error);

bool DeleteSettingsValues() {
    HKEY key = nullptr;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) return true;
    if (result != ERROR_SUCCESS) { TraceRegistryFailure(L"open for reset", kSettingsKey, L"", result); return false; }

    DWORD maximumNameLength = 0;
    result = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &maximumNameLength, nullptr, nullptr, nullptr);
    if (result != ERROR_SUCCESS) {
        RegCloseKey(key);
        TraceRegistryFailure(L"query for reset", kSettingsKey, L"", result);
        return false;
    }

    std::vector<wchar_t> name(maximumNameLength + 1);
    while (true) {
        DWORD nameLength = static_cast<DWORD>(name.size());
        result = RegEnumValueW(key, 0, name.data(), &nameLength, nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS) break;
        if (result != ERROR_SUCCESS) break;
        result = RegDeleteValueW(key, name.data());
        if (result != ERROR_SUCCESS) break;
    }
    RegCloseKey(key);
    if (result == ERROR_NO_MORE_ITEMS) return true;
    TraceRegistryFailure(L"delete setting", kSettingsKey, L"", result);
    return false;
}

void TraceRegistryFailure(const wchar_t* operation, const wchar_t* path, const wchar_t* name, LONG error) {
#ifdef _DEBUG
    std::wstring message = L"Viewtrious registry " + std::wstring(operation) + L" failed: path=" + path +
        std::wstring(L", value=") + (name && *name ? name : L"(Default)") + L", error=" + std::to_wstring(error) + L"\n";
    OutputDebugStringW(message.c_str());
#else
    (void)operation; (void)path; (void)name; (void)error;
#endif
}

bool WriteRegistryString(HKEY root, const wchar_t* path, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    const LONG createResult = RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (createResult != ERROR_SUCCESS) { TraceRegistryFailure(L"create", path, name, createResult); return false; }
    const LONG result = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) TraceRegistryFailure(L"write", path, name, result);
    return result == ERROR_SUCCESS;
}

bool VerifyRegistryString(HKEY root, const wchar_t* path, const wchar_t* name, const std::wstring& expected) {
    DWORD size = 0;
    LONG result = RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, nullptr, &size);
    if (result != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        TraceRegistryFailure(L"read", path, name, result == ERROR_SUCCESS ? ERROR_INVALID_DATA : result);
        return false;
    }
    std::vector<wchar_t> value(size / sizeof(wchar_t));
    result = RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, value.data(), &size);
    if (result != ERROR_SUCCESS || expected != value.data()) {
        TraceRegistryFailure(L"verify", path, name, result == ERROR_SUCCESS ? ERROR_INVALID_DATA : result);
        return false;
    }
    return true;
}

bool ReadRegistryStringIfPresent(HKEY root, const wchar_t* path, const wchar_t* name, std::wstring& value) {
    DWORD size = 0;
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) return false;
    std::vector<wchar_t> buffer(size / sizeof(wchar_t));
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, buffer.data(), &size) != ERROR_SUCCESS) return false;
    value = buffer.data();
    return true;
}

bool CommandOpensExecutable(const std::wstring& command, const std::wstring& executable) {
    const size_t first = command.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return false;
    std::wstring commandExecutable;
    if (command[first] == L'\"') {
        const size_t end = command.find(L'\"', first + 1);
        if (end == std::wstring::npos) return false;
        commandExecutable = command.substr(first + 1, end - first - 1);
    } else {
        const size_t end = command.find_first_of(L" \t", first);
        commandExecutable = command.substr(first, end == std::wstring::npos ? std::wstring::npos : end - first);
    }
    return CompareStringOrdinal(commandExecutable.c_str(), static_cast<int>(commandExecutable.size()),
        executable.c_str(), static_cast<int>(executable.size()), TRUE) == CSTR_EQUAL;
}

bool DeleteRegistryTreeIfPresent(HKEY root, const wchar_t* path) {
    const LONG result = RegDeleteTreeW(root, path);
    if (result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) return true;
    TraceRegistryFailure(L"delete", path, L"", result);
    return false;
}

bool VerifyRegistryKeyAbsent(HKEY root, const wchar_t* path) {
    HKEY key = nullptr;
    const LONG result = RegOpenKeyExW(root, path, 0, KEY_READ, &key);
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) return true;
    if (result == ERROR_SUCCESS) RegCloseKey(key);
    TraceRegistryFailure(L"verify absent", path, L"", result == ERROR_SUCCESS ? ERROR_ALREADY_EXISTS : result);
    return false;
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
    RECT currentTime;
    RECT scrubber;
    RECT duration;
    RECT playPause;
    RECT stepBackward;
    RECT stepForward;
    RECT mute;
    RECT playbackSpeed;
    RECT adjustments;
    RECT fullscreen;
};

struct VideoAdjustmentsPanelLayout {
    RECT panel;
    std::array<RECT, 4> sliders;
    RECT autoButton;
    RECT resetButton;
};

struct ImageZoomHudLayout {
    RECT combined;
    RECT zoom;
    RECT adjustments;
    bool hasZoom;
};

struct ImageAdjustmentsPanelLayout {
    RECT panel;
    std::array<RECT, 6> sliders;
    RECT autoButton;
    RECT resetButton;
};

struct VideoPlaybackSpeedPanelLayout {
    RECT panel;
    std::array<RECT, kVideoPlaybackRatePercents.size()> rates;
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

double PlaybackRateFromPercent(DWORD percent) { return static_cast<double>(percent) / 100.0; }
bool IsVideoPlaybackRatePercent(DWORD percent) { return std::find(kVideoPlaybackRatePercents.begin(), kVideoPlaybackRatePercents.end(), percent) != kVideoPlaybackRatePercents.end(); }
std::wstring FormatPlaybackRate(double rate) {
    if (std::abs(rate - std::round(rate)) < 0.001) return std::to_wstring(static_cast<int>(std::lround(rate))) + L"\x00D7";
    wchar_t text[16]{};
    swprintf_s(text, L"%.2g\x00D7", rate);
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
        DWORD swipeToNavigateWhenFit = 0;
        ReadSetting(L"SwipeToNavigateWhenFit", swipeToNavigateWhenFit);
        swipeToNavigateWhenFit_ = swipeToNavigateWhenFit != 0;
        DWORD showZoomHud = 1;
        ReadSetting(L"ShowZoomPercentage", showZoomHud);
        showZoomPercentage_ = showZoomHud != 0;
        DWORD zoomHudPosition = static_cast<DWORD>(ZoomHudPosition::BottomRight);
        ReadSetting(L"ZoomHudPosition", zoomHudPosition);
        zoomHudPosition_ = zoomHudPosition <= static_cast<DWORD>(ZoomHudPosition::TopRight) ? static_cast<ZoomHudPosition>(zoomHudPosition) : ZoomHudPosition::BottomRight;
        DWORD animations = 1;
        ReadSetting(L"AnimationsAndFadeEffects", animations);
        animationsEnabled_ = animations != 0;
        DWORD alwaysShowFilmstrip = 0;
        ReadSetting(L"AlwaysShowFilmstrip", alwaysShowFilmstrip);
        alwaysShowFilmstrip_ = alwaysShowFilmstrip != 0;
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
        aiAddon_.Initialize();
        startupPath_ = path;
        return S_OK;
    }

    HRESULT LoadContent(const std::wstring& path, bool resetNavigation = true) {
        if (!currentPath_.empty() && !PathsEqual(fs::path(path), fs::path(currentPath_))) FlushImageAdjustmentPersistence();
        ++aiRequestGeneration_;
        ClearStillDissolve();
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
            SuppressFilmstripHoverPreviewForCurrentMedia();
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
        adjustmentPersistence_.Start([this](ImageAdjustmentPersistenceResult&& result) {
            auto* message = new ImageAdjustmentPersistenceResult(std::move(result));
            if (!PostMessageW(window_, kImageAdjustmentPersistenceCompleteMessage, 0, reinterpret_cast<LPARAM>(message))) delete message;
        });
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
        ShowVideoControls();
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
        item = hit(DropdownItem::Help); if (item != DropdownItem::None) return item;
        item = hit(DropdownItem::Feedback); if (item != DropdownItem::None) return item;
        item = hit(DropdownItem::About); if (item != DropdownItem::None) return item;
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
        else if (item == DropdownItem::Help) ShowOverlay(OverlayKind::Help);
        else if (item == DropdownItem::About) ShowOverlay(OverlayKind::About);
        else if (item == DropdownItem::Feedback) ShowOverlay(OverlayKind::Feedback);
        else if (item == DropdownItem::Close) SendMessageW(window_, WM_SYSCOMMAND, SC_CLOSE, 0);
    }
    void OpenFile() {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
        static const COMDLG_FILTERSPEC filters[] = {
            { L"Supported files", StepAddonPresent() ? L"*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.ico;*.webp;*.heic;*.heif;*.avif;*.dng;*.cr2;*.cr3;*.nef;*.arw;*.raf;*.mp4;*.mov;*.mkv;*.stl;*.3mf;*.step;*.stp" : L"*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.ico;*.webp;*.heic;*.heif;*.avif;*.dng;*.cr2;*.cr3;*.nef;*.arw;*.raf;*.mp4;*.mov;*.mkv;*.stl;*.3mf" },
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
        StopVideoStepHold();
        videoPlayer_.TogglePlayPause();
        if (videoPlayer_.Playing()) videoPausedSeekRefreshPending_ = false;
        ShowVideoControls();
        ScheduleVideoPlaybackTimer();
        if (!videoPlayer_.Playing()) videoPlayer_.FlushFramePacingDiagnostics();
        InvalidateRect(window_, nullptr, FALSE);
    }
    void StopVideoStepHold() {
        KillTimer(window_, kVideoStepHoldTimer);
        videoStepHoldDirection_ = 0;
        videoStepHoldActive_ = false;
        videoStepHoldAnchorSeconds_ = 0.0;
        videoStepHoldDurationSeconds_ = 0.0;
        videoStepHoldStartQpc_ = 0;
        videoStepHoldQpcFrequency_ = 0;
    }
    bool BeginVideoStepHold(int direction) {
        if (!VideoActive() || !direction) return false;
        StopVideoStepHold();
        NudgeVideoPosition(direction);
        if (!VideoActive() || videoPlayer_.Playing()) return false;
        double current = 0.0, duration = 0.0;
        if (!videoPlayer_.GetPlaybackTimes(current, duration)) return false;
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return false;
        videoStepHoldDirection_ = direction < 0 ? -1 : 1;
        videoStepHoldAnchorSeconds_ = videoPausedSeekRefreshPending_ ? videoScrubSeconds_ : current;
        videoStepHoldDurationSeconds_ = duration;
        videoStepHoldQpcFrequency_ = frequency.QuadPart;
        SetTimer(window_, kVideoStepHoldTimer, kVideoStepHoldThresholdMs, nullptr);
        ShowVideoControls();
        return true;
    }
    void UpdateVideoStepHold() {
        KillTimer(window_, kVideoStepHoldTimer);
        if (!videoStepHoldDirection_ || !VideoActive() || videoPlayer_.Playing()) { StopVideoStepHold(); return; }
        LARGE_INTEGER now{};
        if (!QueryPerformanceCounter(&now) || videoStepHoldQpcFrequency_ <= 0) { StopVideoStepHold(); return; }
        if (!videoStepHoldActive_) {
            videoStepHoldActive_ = true;
            videoStepHoldStartQpc_ = now.QuadPart;
        }
        const double elapsedSeconds = static_cast<double>(now.QuadPart - videoStepHoldStartQpc_) / static_cast<double>(videoStepHoldQpcFrequency_);
        const double target = std::clamp(videoStepHoldAnchorSeconds_ + videoStepHoldDirection_ * elapsedSeconds, 0.0, videoStepHoldDurationSeconds_);
        videoScrubSeconds_ = target;
        if (videoPlayer_.Seek(target) && !videoPlayer_.Playing()) videoPausedSeekRefreshPending_ = true;
        InvalidateRect(window_, nullptr, FALSE);
        if (target > 0.0 && target < videoStepHoldDurationSeconds_)
            SetTimer(window_, kVideoStepHoldTimer, kVideoStepHoldIntervalMs, nullptr);
    }
    void NudgeVideoPosition(int direction) {
        if (!VideoActive() || !direction) return;
        if (videoPlayer_.Playing()) {
            ToggleVideoPlayPause();
            videoPausedSeekRefreshPending_ = false;
            if (videoPlayer_.Playing()) return;
        }
        double current = 0.0, duration = 0.0;
        if (!videoPlayer_.GetPlaybackTimes(current, duration)) return;
        float framesPerSecond = 0.0f;
        const double stepSeconds = videoPlayer_.TryGetFramesPerSecond(framesPerSecond) && std::isfinite(framesPerSecond) && framesPerSecond >= 1.0f && framesPerSecond <= 240.0f
            ? 1.0 / static_cast<double>(framesPerSecond) : 1.0 / 30.0;
        const double anchor = videoPausedSeekRefreshPending_ ? videoScrubSeconds_ : current;
        videoScrubSeconds_ = std::clamp(anchor + direction * stepSeconds, 0.0, duration);
        if (videoPlayer_.Seek(videoScrubSeconds_) && !videoPlayer_.Playing()) videoPausedSeekRefreshPending_ = true;
        ShowVideoControls();
        InvalidateRect(window_, nullptr, FALSE);
    }
    VideoControlsLayout GetVideoControlsLayout() const {
        const RECT canvas = ModelCanvasBounds();
        const UINT dpi = GetDpiForWindow(window_);
        const int margin = MulDiv(16, dpi, 96);
        const int bottomGap = MulDiv(24, dpi, 96);
        const int height = MulDiv(84, dpi, 96);
        const int preferredWidth = MulDiv(600, dpi, 96);
        const LONG availableWidth = std::max(1L, canvas.right - canvas.left - margin * 2);
        const int width = std::min(preferredWidth, static_cast<int>(availableWidth));
        const int left = static_cast<int>(canvas.left + (canvas.right - canvas.left - width) / 2);
        const int top = static_cast<int>(std::max(canvas.top, canvas.bottom - bottomGap - height));
        const int padding = std::min(MulDiv(10, dpi, 96), std::max(2, width / 24));
        const int timelineHeight = MulDiv(28, dpi, 96);
        const int rowGap = std::min(MulDiv(5, dpi, 96), std::max(2, height / 16));
        const int buttonWidth = std::min(MulDiv(32, dpi, 96), std::max(MulDiv(24, dpi, 96), height - padding * 2 - timelineHeight - rowGap));
        const int gap = std::min(MulDiv(8, dpi, 96), std::max(3, width / 80));
        const int minimumTrackWidth = MulDiv(40, dpi, 96);
        const int maximumTimeWidth = std::max(MulDiv(16, dpi, 96), (width - padding * 2 - gap * 2 - minimumTrackWidth) / 2);
        const int timeWidth = std::min(MulDiv(48, dpi, 96), maximumTimeWidth);
        const int currentLeft = left + padding;
        const int durationRight = left + width - padding;
        const int scrubberLeft = currentLeft + timeWidth + gap;
        const int scrubberRight = std::max(scrubberLeft, durationRight - timeWidth - gap);
        const int controlTop = top + padding + timelineHeight + rowGap;
        const int playLeft = left + padding;
        const int stepBackwardLeft = playLeft + buttonWidth + gap;
        const int stepForwardLeft = stepBackwardLeft + buttonWidth + gap;
        const int muteLeft = stepForwardLeft + buttonWidth + gap;
        const int speedWidth = std::min(MulDiv(46, dpi, 96), std::max(MulDiv(34, dpi, 96), buttonWidth + gap));
        const int fullscreenLeft = left + width - padding - buttonWidth;
        const int adjustmentsLeft = fullscreenLeft - gap - buttonWidth;
        const int speedLeft = adjustmentsLeft - gap - speedWidth;
        return { { left, top, left + width, top + height },
            { currentLeft, top + padding, currentLeft + timeWidth, top + padding + timelineHeight },
            { scrubberLeft, top + padding, scrubberRight, top + padding + timelineHeight },
            { durationRight - timeWidth, top + padding, durationRight, top + padding + timelineHeight },
            { playLeft, controlTop, playLeft + buttonWidth, controlTop + buttonWidth },
            { stepBackwardLeft, controlTop, stepBackwardLeft + buttonWidth, controlTop + buttonWidth },
            { stepForwardLeft, controlTop, stepForwardLeft + buttonWidth, controlTop + buttonWidth },
            { muteLeft, controlTop, muteLeft + buttonWidth, controlTop + buttonWidth },
            { speedLeft, controlTop, speedLeft + speedWidth, controlTop + buttonWidth },
            { adjustmentsLeft, controlTop, adjustmentsLeft + buttonWidth, controlTop + buttonWidth },
            { fullscreenLeft, controlTop, fullscreenLeft + buttonWidth, controlTop + buttonWidth } };
    }
    VideoPlaybackSpeedPanelLayout GetVideoPlaybackSpeedPanelLayout() const {
        const VideoControlsLayout controls = GetVideoControlsLayout();
        const UINT dpi = GetDpiForWindow(window_);
        const int gap = MulDiv(8, dpi, 96);
        const int rowHeight = MulDiv(29, dpi, 96);
        const int width = MulDiv(90, dpi, 96);
        const int height = rowHeight * static_cast<int>(kVideoPlaybackRatePercents.size()) + MulDiv(8, dpi, 96);
        const LONG right = controls.playbackSpeed.right;
        const LONG left = right - width;
        const LONG bottom = controls.island.top - gap;
        const LONG top = bottom - height;
        std::array<RECT, kVideoPlaybackRatePercents.size()> rates{};
        for (size_t index = 0; index < rates.size(); ++index) {
            const LONG rowTop = top + MulDiv(4, dpi, 96) + static_cast<LONG>(index) * rowHeight;
            rates[index] = { left + MulDiv(4, dpi, 96), rowTop, right - MulDiv(4, dpi, 96), rowTop + rowHeight };
        }
        return { { left, top, right, bottom }, rates };
    }
    VideoAdjustmentsPanelLayout GetVideoAdjustmentsPanelLayout() const {
        const VideoControlsLayout controls = GetVideoControlsLayout();
        const RECT canvas = ModelCanvasBounds();
        const UINT dpi = GetDpiForWindow(window_);
        const int gap = MulDiv(8, dpi, 96);
        const LONG width = std::min<LONG>(MulDiv(300, dpi, 96), std::max<LONG>(MulDiv(220, dpi, 96), canvas.right - canvas.left - MulDiv(24, dpi, 96)));
        const LONG height = MulDiv(218, dpi, 96);
        const LONG right = std::min<LONG>(controls.adjustments.right, canvas.right - MulDiv(8, dpi, 96));
        const LONG left = std::max<LONG>(canvas.left + MulDiv(8, dpi, 96), right - width);
        const LONG bottom = std::max<LONG>(canvas.top + height, controls.island.top - gap);
        const LONG top = bottom - height;
        const int labelWidth = MulDiv(72, dpi, 96);
        const int valueWidth = MulDiv(38, dpi, 96);
        const int rowHeight = MulDiv(30, dpi, 96);
        const int sliderLeft = left + labelWidth;
        const int sliderRight = right - valueWidth - MulDiv(12, dpi, 96);
        std::array<RECT, 4> sliders{};
        for (int index = 0; index < 4; ++index) {
            const int y = top + MulDiv(18, dpi, 96) + index * rowHeight;
            sliders[index] = { sliderLeft, y, sliderRight, y + MulDiv(20, dpi, 96) };
        }
        const int buttonTop = top + MulDiv(150, dpi, 96);
        const int buttonWidth = MulDiv(74, dpi, 96);
        const RECT reset{ right - buttonWidth, buttonTop, right, buttonTop + MulDiv(30, dpi, 96) };
        const RECT ai = aiAddon_.Available() ? RECT{ right - buttonWidth * 2 - gap, buttonTop, right - buttonWidth - gap, buttonTop + MulDiv(30, dpi, 96) } : RECT{};
        return { { left, top, right, bottom }, sliders, ai, reset };
    }
    bool VideoAdjustmentsPanelContains(POINT point) const {
        if (!videoAdjustmentsPanelOpen_) return false;
        const RECT panel = GetVideoAdjustmentsPanelLayout().panel;
        return PtInRect(&panel, point) != FALSE;
    }
    bool VideoPlaybackSpeedPanelContains(POINT point) const {
        if (!videoPlaybackSpeedPanelOpen_) return false;
        const RECT panel = GetVideoPlaybackSpeedPanelLayout().panel;
        return PtInRect(&panel, point) != FALSE;
    }
    bool VideoPlaybackSpeedPanelOpen() const { return videoPlaybackSpeedPanelOpen_; }
    void SetVideoPlaybackSpeedPanelOpen(bool open) {
        videoPlaybackSpeedPanelOpen_ = open;
        if (open) videoAdjustmentsPanelOpen_ = false;
        if (!open) videoControlsPointerOver_ = false;
        ShowVideoControls();
    }
    void SelectVideoPlaybackRate(DWORD percent) {
        if (!IsVideoPlaybackRatePercent(percent)) return;
        const double requested = PlaybackRateFromPercent(percent);
        videoPreferredPlaybackRatePercent_ = percent;
        if (!VideoActive() || videoPlayer_.SetPreferredPlaybackRate(requested)) {
            videoEffectivePlaybackRate_ = VideoActive() ? videoPlayer_.EffectivePlaybackRate() : requested;
            if (VideoActive() && videoPlayer_.Playing()) ScheduleVideoPlaybackTimer(true);
        }
        InvalidateRect(window_, nullptr, FALSE);
    }
    void ApplyVideoAdjustments() {
        videoPlayer_.SetDisplayAdjustments(videoAdjustments_);
        InvalidateRect(window_, nullptr, FALSE);
    }
    bool VideoAdjustmentsPanelOpen() const { return videoAdjustmentsPanelOpen_; }
    void SetVideoAdjustmentsPanelOpen(bool open) {
        videoAdjustmentsPanelOpen_ = open;
        if (open) videoPlaybackSpeedPanelOpen_ = false;
        videoAdjustmentsDragging_ = -1;
        if (!open) videoControlsPointerOver_ = false;
        ShowVideoControls();
    }
    void ResetVideoAdjustments() { videoAdjustments_ = {}; ApplyVideoAdjustments(); }
    void UpdateVideoAdjustmentSlider(int index, POINT point) {
        if (index < 0 || index >= 4) return;
        const RECT slider = GetVideoAdjustmentsPanelLayout().sliders[index];
        const float value = std::clamp(static_cast<float>(point.x - slider.left) / static_cast<float>(std::max(1L, slider.right - slider.left)), 0.0f, 1.0f) * 2.0f - 1.0f;
        if (index == 0) videoAdjustments_.brightness = value;
        else if (index == 1) videoAdjustments_.contrast = value;
        else if (index == 2) videoAdjustments_.shadows = value;
        else videoAdjustments_.highlights = value;
        ApplyVideoAdjustments();
    }
    ImageZoomHudLayout GetImageZoomHudLayout() const {
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        const int zoomWidth = showZoomPercentage_ ? static_cast<int>(std::lround(72.0f * scale)) : 0;
        const int height = static_cast<int>(std::lround(30.0f * scale));
        const int buttonWidth = height;
        const int gap = static_cast<int>(std::lround(2.0f * scale));
        const int margin = static_cast<int>(std::lround(14.0f * scale));
        const D2D1_SIZE_F target = renderTarget_->GetSize();
        const bool left = zoomHudPosition_ == ZoomHudPosition::BottomLeft || zoomHudPosition_ == ZoomHudPosition::TopLeft;
        const bool top = zoomHudPosition_ == ZoomHudPosition::TopLeft || zoomHudPosition_ == ZoomHudPosition::TopRight;
        const int totalWidth = buttonWidth + (zoomWidth ? gap + zoomWidth : 0);
        const int x = left ? margin : static_cast<int>(target.width) - margin - totalWidth;
        const int canvasTop = fullscreen_ ? 0 : GetFrameMetrics(window_).titleBarHeight;
        const int y = top ? canvasTop + margin : static_cast<int>(target.height) - margin - height;
        const RECT combined{ x, y, x + totalWidth, y + height };
        const RECT zoom = left ? RECT{ x, y, x + zoomWidth, y + height } : RECT{ combined.right - zoomWidth, y, combined.right, y + height };
        const RECT adjustments = left ? RECT{ zoom.right + (zoomWidth ? gap : 0), y, combined.right, y + height } : RECT{ x, y, x + buttonWidth, y + height };
        return { combined, zoom, adjustments, zoomWidth != 0 };
    }
    ImageAdjustmentsPanelLayout GetImageAdjustmentsPanelLayout() const {
        const ImageZoomHudLayout hud = GetImageZoomHudLayout();
        const D2D1_RECT_F canvas = ImageCanvasBounds();
        const UINT dpi = GetDpiForWindow(window_);
        const int gap = MulDiv(8, dpi, 96);
        const int width = std::min(MulDiv(300, dpi, 96), std::max(MulDiv(220, dpi, 96), static_cast<int>(canvas.right - canvas.left) - MulDiv(24, dpi, 96)));
        const int height = MulDiv(278, dpi, 96);
        const bool left = zoomHudPosition_ == ZoomHudPosition::BottomLeft || zoomHudPosition_ == ZoomHudPosition::TopLeft;
        const bool top = zoomHudPosition_ == ZoomHudPosition::TopLeft || zoomHudPosition_ == ZoomHudPosition::TopRight;
        const int panelLeft = left ? std::max(static_cast<int>(canvas.left) + gap, static_cast<int>(hud.combined.left)) : std::min(static_cast<int>(canvas.right) - gap - width, static_cast<int>(hud.combined.right) - width);
        int panelTop = top ? hud.combined.bottom + gap : hud.combined.top - gap - height;
        panelTop = std::clamp(panelTop, static_cast<int>(canvas.top) + gap, std::max(static_cast<int>(canvas.top) + gap, static_cast<int>(canvas.bottom) - gap - height));
        const RECT panel{ panelLeft, panelTop, panelLeft + width, panelTop + height };
        const int labelWidth = MulDiv(72, dpi, 96);
        const int valueWidth = MulDiv(38, dpi, 96);
        const int rowHeight = MulDiv(30, dpi, 96);
        std::array<RECT, 6> sliders{};
        for (int index = 0; index < 6; ++index) {
            const int y = panel.top + MulDiv(18, dpi, 96) + index * rowHeight;
            sliders[index] = { panel.left + labelWidth, y, panel.right - valueWidth - MulDiv(12, dpi, 96), y + MulDiv(20, dpi, 96) };
        }
        const int buttonTop = panel.top + MulDiv(210, dpi, 96);
        const int buttonWidth = MulDiv(74, dpi, 96);
        const RECT reset{ panel.right - buttonWidth, buttonTop, panel.right, buttonTop + MulDiv(30, dpi, 96) };
        const RECT ai = aiAddon_.Available() ? RECT{ panel.right - buttonWidth * 2 - gap, buttonTop, panel.right - buttonWidth - gap, buttonTop + MulDiv(30, dpi, 96) } : RECT{};
        return { panel, sliders, ai, reset };
    }
    void ApplyImageAdjustments() {
        imageAdjustedBitmap_.Reset();
        InvalidateRect(window_, nullptr, FALSE);
    }
    void QueueImageAdjustmentPersistence() {
        ++imageAdjustmentEditGeneration_;
        if (imageAdjustmentHashResolved_) SetTimer(window_, kImageAdjustmentPersistenceTimer, 300, nullptr);
        else pendingImageAdjustmentSaves_[imageAdjustmentMediaGeneration_] = imageAdjustments_;
    }
    void FlushImageAdjustmentPersistence() {
        KillTimer(window_, kImageAdjustmentPersistenceTimer);
        if (imageAdjustmentHashResolved_) adjustmentPersistence_.Save(imageAdjustmentHash_, imageAdjustments_);
    }
    void ResetImageAdjustments() { imageAdjustments_ = {}; ApplyImageAdjustments(); QueueImageAdjustmentPersistence(); }
    bool ImageAdjustmentsPanelOpen() const { return imageAdjustmentsPanelOpen_; }
    bool ImageAdjustmentsPanelContains(POINT point) const {
        if (!imageAdjustmentsPanelOpen_) return false;
        const RECT panel = GetImageAdjustmentsPanelLayout().panel;
        return PtInRect(&panel, point) != FALSE;
    }
    void SetImageAdjustmentsPanelOpen(bool open) { imageAdjustmentsPanelOpen_ = open; imageAdjustmentsDragging_ = -1; InvalidateRect(window_, nullptr, FALSE); }
    void UpdateImageAdjustmentSlider(int index, POINT point) {
        if (index < 0 || index >= 6) return;
        const RECT slider = GetImageAdjustmentsPanelLayout().sliders[index];
        const float position = std::clamp(static_cast<float>(point.x - slider.left) / static_cast<float>(std::max(1L, slider.right - slider.left)), 0.0f, 1.0f);
        const float value = position * 2.0f - 1.0f;
        if (index == 0) imageAdjustments_.exposure = position * 4.0f - 2.0f;
        else if (index == 1) imageAdjustments_.brightness = value;
        else if (index == 2) imageAdjustments_.contrast = value;
        else if (index == 3) imageAdjustments_.shadows = value;
        else if (index == 4) imageAdjustments_.highlights = value;
        else imageAdjustments_.saturation = value;
        ApplyImageAdjustments();
        QueueImageAdjustmentPersistence();
    }
    bool BuildAiImage(AiImageBuffer& image) {
        std::vector<BYTE> sourcePixels;
        UINT sourceWidth = 0, sourceHeight = 0;
        if (VideoActive()) {
            if (!videoPlayer_.CopyCurrentFrameBgra(sourcePixels, sourceWidth, sourceHeight)) return false;
        } else {
            if (!source_ || FAILED(source_->GetSize(&sourceWidth, &sourceHeight)) || !sourceWidth || !sourceHeight) return false;
            sourcePixels.resize(static_cast<size_t>(sourceWidth) * sourceHeight * 4);
            if (FAILED(source_->CopyPixels(nullptr, sourceWidth * 4, static_cast<UINT>(sourcePixels.size()), sourcePixels.data()))) return false;
        }
        constexpr UINT maxEdge = 640;
        const float scale = std::min(1.0f, static_cast<float>(maxEdge) / std::max(sourceWidth, sourceHeight));
        image.width = std::max(1u, static_cast<UINT>(std::lround(sourceWidth * scale)));
        image.height = std::max(1u, static_cast<UINT>(std::lround(sourceHeight * scale)));
        image.stride = image.width * 4; image.pixels.resize(static_cast<size_t>(image.stride) * image.height);
        for (UINT y = 0; y < image.height; ++y) for (UINT x = 0; x < image.width; ++x) {
            const UINT sx = std::min(sourceWidth - 1, static_cast<UINT>(x / scale));
            const UINT sy = std::min(sourceHeight - 1, static_cast<UINT>(y / scale));
            std::memcpy(image.pixels.data() + static_cast<size_t>(y) * image.stride + x * 4, sourcePixels.data() + (static_cast<size_t>(sy) * sourceWidth + sx) * 4, 4);
        }
        return true;
    }

    void StartAiAnalysis() {
        if (!aiAddon_.Available()) return;
        if (aiAnalysisRunning_.exchange(true)) return;
        AiImageBuffer image;
        if (!BuildAiImage(image)) { aiAnalysisRunning_ = false; return; }
        if (aiAnalysisThread_.joinable()) aiAnalysisThread_.join();
        const uint64_t generation = ++aiRequestGeneration_;
        const std::wstring path = currentPath_; const ContentKind kind = contentKind_;
        aiAnalysisThread_ = std::thread([this, generation, path, kind, image = std::move(image)]() mutable {
            auto* result = new AiAnalysisResult{}; result->generation = generation; result->path = path; result->contentKind = kind; result->workerId = std::this_thread::get_id();
            result->succeeded = aiAddon_.Analyze(image, result->adjustments);
            if (!PostMessageW(window_, kAiAnalysisCompleteMessage, 0, reinterpret_cast<LPARAM>(result))) { delete result; aiAnalysisRunning_ = false; }
        });
    }

    void AiAnalysisCompleteMessage(AiAnalysisResult* result) {
        std::unique_ptr<AiAnalysisResult> owned(result);
        if (!result) return;
        if (aiAnalysisThread_.joinable() && aiAnalysisThread_.get_id() == result->workerId) aiAnalysisThread_.join();
        aiAnalysisRunning_ = false;
        if (!result->succeeded || result->generation != aiRequestGeneration_ || result->contentKind != contentKind_ || !PathsEqual(fs::path(result->path), fs::path(currentPath_)) || result->adjustments.confidence < .15f) return;
        MediaAdjustments adjusted{ std::clamp(result->adjustments.brightness, -.35f, .45f), std::clamp(result->adjustments.contrast, -.35f, .30f), std::clamp(result->adjustments.shadows, -.20f, .65f), std::clamp(result->adjustments.highlights, -.50f, .25f) };
        if (VideoActive()) { videoAdjustments_ = adjusted; ApplyVideoAdjustments(); }
        else { imageAdjustments_.exposure = 0.0f; imageAdjustments_.brightness = adjusted.brightness; imageAdjustments_.contrast = adjusted.contrast; imageAdjustments_.shadows = adjusted.shadows; imageAdjustments_.highlights = adjusted.highlights; imageAdjustments_.saturation = 0.0f; ApplyImageAdjustments(); QueueImageAdjustmentPersistence(); }
    }
    bool BeginImageAdjustmentsInteraction(POINT point) {
        if (!source_) return false;
        if (imageAdjustmentsPanelOpen_) {
            const ImageAdjustmentsPanelLayout panel = GetImageAdjustmentsPanelLayout();
            if (PtInRect(&panel.panel, point)) {
                for (int index = 0; index < static_cast<int>(panel.sliders.size()); ++index) {
                    const RECT hit{ panel.sliders[index].left, panel.sliders[index].top - MulDiv(6, GetDpiForWindow(window_), 96), panel.sliders[index].right, panel.sliders[index].bottom + MulDiv(6, GetDpiForWindow(window_), 96) };
                    if (PtInRect(&hit, point)) { imageAdjustmentsDragging_ = index; UpdateImageAdjustmentSlider(index, point); return true; }
                }
                if (aiAddon_.Available() && PtInRect(&panel.autoButton, point)) { StartAiAnalysis(); return true; }
                if (PtInRect(&panel.resetButton, point)) { ResetImageAdjustments(); return true; }
                return true;
            }
            if (ButtonAt(point) != ButtonKind::ImageAdjustments) { SetImageAdjustmentsPanelOpen(false); return true; }
        }
        return false;
    }
    bool ContinueImageAdjustmentsInteraction(POINT point) {
        if (imageAdjustmentsDragging_ < 0) return false;
        UpdateImageAdjustmentSlider(imageAdjustmentsDragging_, point);
        return true;
    }
    bool EndImageAdjustmentsInteraction(POINT point) {
        if (imageAdjustmentsDragging_ < 0) return false;
        UpdateImageAdjustmentSlider(imageAdjustmentsDragging_, point);
        imageAdjustmentsDragging_ = -1;
        FlushImageAdjustmentPersistence();
        return true;
    }
    bool VideoControlsInteractive() const { return VideoActive() && videoControlsOpacity_ > 0.05f; }
    ButtonKind VideoControlAt(POINT point) const {
        if (!VideoControlsInteractive()) return ButtonKind::None;
        const VideoControlsLayout layout = GetVideoControlsLayout();
        if (PtInRect(&layout.playPause, point)) return ButtonKind::VideoPlayPause;
        if (PtInRect(&layout.stepBackward, point)) return ButtonKind::VideoStepBackward;
        if (PtInRect(&layout.stepForward, point)) return ButtonKind::VideoStepForward;
        if (PtInRect(&layout.mute, point)) return ButtonKind::VideoMute;
        if (PtInRect(&layout.playbackSpeed, point)) return ButtonKind::VideoPlaybackSpeed;
        if (PtInRect(&layout.adjustments, point)) return ButtonKind::VideoAdjustments;
        if (PtInRect(&layout.fullscreen, point)) return ButtonKind::VideoFullscreen;
        return ButtonKind::None;
    }
    bool VideoScrubberContains(POINT point) const {
        if (!VideoControlsInteractive()) return false;
        const VideoControlsLayout layout = GetVideoControlsLayout();
        const RECT hit{ layout.scrubber.left, layout.scrubber.top + (layout.scrubber.bottom - layout.scrubber.top) / 2 - MulDiv(12, GetDpiForWindow(window_), 96),
            layout.scrubber.right, layout.scrubber.top + (layout.scrubber.bottom - layout.scrubber.top) / 2 + MulDiv(12, GetDpiForWindow(window_), 96) };
        return hit.right > hit.left && PtInRect(&hit, point);
    }
    bool VideoCanvasContains(POINT point) const {
        if (!VideoActive()) return false;
        const RECT canvas = ModelCanvasBounds();
        return PtInRect(&canvas, point) != FALSE;
    }
    bool VideoControlsContains(POINT point) const {
        if (!VideoControlsInteractive()) return false;
        const RECT island = GetVideoControlsLayout().island;
        return VideoAdjustmentsPanelContains(point) || VideoPlaybackSpeedPanelContains(point) || PtInRect(&island, point);
    }
    bool VideoCursorMayHide() const { return !HasOverlay() && !TutorialActive() && !videoAdjustmentsPanelOpen_ && !videoPlaybackSpeedPanelOpen_; }
    void RestoreVideoCursor() {
        if (!videoCursorHidden_) return;
        ShowCursor(TRUE);
        videoCursorHidden_ = false;
    }
    void HideVideoCursorIfAppropriate() {
        if (videoCursorHidden_ || !VideoCursorMayHide() || !VideoActive() || !videoPlayer_.Playing() || videoControlsOpacity_ > 0.01f) return;
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
        if (videoPlayer_.Playing() && !videoControlsPointerOver_ && !videoScrubbing_ && !videoAdjustmentsPanelOpen_ && !videoPlaybackSpeedPanelOpen_)
            SetTimer(window_, kVideoControlsTimer, static_cast<UINT>(kVideoControlsIdleDelayMs), nullptr);
        InvalidateRect(window_, nullptr, FALSE);
    }
    void ResetVideoControls() {
        KillTimer(window_, kVideoControlsTimer);
        StopVideoStepHold();
        videoControlsOpacity_ = 1.0f;
        videoControlsFadeActive_ = false;
        videoControlsPointerOver_ = false;
        videoScrubbing_ = false;
        videoWasPlayingBeforeScrub_ = false;
        videoAdjustmentsPanelOpen_ = false;
        videoPlaybackSpeedPanelOpen_ = false;
        videoAdjustmentsDragging_ = -1;
        videoControlsHovered_ = ButtonKind::None;
        videoControlsLastActivity_ = GetTickCount64();
        RestoreVideoCursor();
    }
    void StopVideoControls() {
        KillTimer(window_, kVideoControlsTimer);
        StopVideoStepHold();
        videoScrubbing_ = false;
        videoWasPlayingBeforeScrub_ = false;
        videoAdjustmentsPanelOpen_ = false;
        videoPlaybackSpeedPanelOpen_ = false;
        videoAdjustmentsDragging_ = -1;
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
        const bool seekStarted = videoPlayer_.Seek(videoScrubSeconds_);
        if (seekStarted && !videoPlayer_.Playing()) videoPausedSeekRefreshPending_ = true;
        InvalidateRect(window_, nullptr, FALSE);
    }
    bool BeginVideoControlsInteraction(POINT point) {
        if (!VideoActive()) return false;
        ShowVideoControls();
        if (videoPlaybackSpeedPanelOpen_) {
            const VideoPlaybackSpeedPanelLayout panel = GetVideoPlaybackSpeedPanelLayout();
            if (PtInRect(&panel.panel, point)) {
                for (size_t index = 0; index < panel.rates.size(); ++index) {
                    if (PtInRect(&panel.rates[index], point)) {
                        const DWORD percent = kVideoPlaybackRatePercents[index];
                        if (videoPlayer_.PlaybackRateSupported(PlaybackRateFromPercent(percent))) {
                            SelectVideoPlaybackRate(percent);
                            SetVideoPlaybackSpeedPanelOpen(false);
                        }
                        return true;
                    }
                }
                return true;
            }
            if (!VideoControlsContains(point)) { SetVideoPlaybackSpeedPanelOpen(false); return true; }
        }
        if (videoAdjustmentsPanelOpen_) {
            const VideoAdjustmentsPanelLayout panel = GetVideoAdjustmentsPanelLayout();
            if (PtInRect(&panel.panel, point)) {
                for (int index = 0; index < static_cast<int>(panel.sliders.size()); ++index) {
                    const RECT hit{ panel.sliders[index].left, panel.sliders[index].top - MulDiv(6, GetDpiForWindow(window_), 96), panel.sliders[index].right, panel.sliders[index].bottom + MulDiv(6, GetDpiForWindow(window_), 96) };
                    if (PtInRect(&hit, point)) { videoAdjustmentsDragging_ = index; UpdateVideoAdjustmentSlider(index, point); return true; }
                }
                if (aiAddon_.Available() && PtInRect(&panel.autoButton, point)) { StartAiAnalysis(); return true; }
                if (PtInRect(&panel.resetButton, point)) { ResetVideoAdjustments(); return true; }
                return true;
            }
            if (!VideoControlsContains(point)) { SetVideoAdjustmentsPanelOpen(false); return true; }
        }
        if (!VideoControlsContains(point)) return false;
        videoControlsPointerOver_ = true;
        KillTimer(window_, kVideoControlsTimer);
        if (VideoScrubberContains(point)) {
            StopVideoStepHold();
            videoWasPlayingBeforeScrub_ = videoPlayer_.Playing();
            if (videoWasPlayingBeforeScrub_) ToggleVideoPlayPause();
            videoScrubbing_ = true;
            UpdateVideoScrub(point);
            return true;
        }
        const ButtonKind control = VideoControlAt(point);
        if (control == ButtonKind::VideoPlayPause) ToggleVideoPlayPause();
        else if (control == ButtonKind::VideoStepBackward) BeginVideoStepHold(-1);
        else if (control == ButtonKind::VideoStepForward) BeginVideoStepHold(1);
        else if (control == ButtonKind::VideoMute) { videoPlayer_.ToggleMute(); ShowVideoControls(); }
        else if (control == ButtonKind::VideoPlaybackSpeed) SetVideoPlaybackSpeedPanelOpen(!videoPlaybackSpeedPanelOpen_);
        else if (control == ButtonKind::VideoAdjustments) { if (videoAdjustmentsPanelOpen_) SetVideoAdjustmentsPanelOpen(false); else { videoPlaybackSpeedPanelOpen_ = false; SetVideoAdjustmentsPanelOpen(true); } }
        else if (control == ButtonKind::VideoFullscreen) { videoFullscreenToggleTick_ = GetTickCount64(); ToggleVideoFullscreen(); }
        return true;
    }
    bool ContinueVideoControlsInteraction(POINT point) {
        if (videoAdjustmentsDragging_ >= 0) { UpdateVideoAdjustmentSlider(videoAdjustmentsDragging_, point); return true; }
        if (!videoScrubbing_) return false;
        ShowVideoControls();
        videoScrubbing_ = true;
        UpdateVideoScrub(point);
        return true;
    }
    bool EndVideoControlsInteraction(POINT point) {
        if (videoAdjustmentsDragging_ >= 0) { UpdateVideoAdjustmentSlider(videoAdjustmentsDragging_, point); videoAdjustmentsDragging_ = -1; return true; }
        if (videoStepHoldDirection_) {
            StopVideoStepHold();
            ShowVideoControls();
            return true;
        }
        if (!videoScrubbing_) return false;
        UpdateVideoScrub(point);
        videoScrubbing_ = false;
        const bool resumePlayback = videoWasPlayingBeforeScrub_;
        videoWasPlayingBeforeScrub_ = false;
        if (resumePlayback && VideoActive() && !videoPlayer_.Playing()) ToggleVideoPlayPause();
        else ShowVideoControls();
        return true;
    }
    void UpdateVideoControlsMouse(POINT point) {
        if (!VideoActive()) return;
        lastMousePoint_ = point;
        ShowVideoControls();
        videoControlsPointerOver_ = VideoControlsContains(point);
        videoControlsHovered_ = VideoControlAt(point);
        if (videoControlsPointerOver_) KillTimer(window_, kVideoControlsTimer);
        else if (videoPlayer_.Playing() && !videoScrubbing_ && !videoAdjustmentsPanelOpen_ && !videoPlaybackSpeedPanelOpen_) SetTimer(window_, kVideoControlsTimer, static_cast<UINT>(kVideoControlsIdleDelayMs), nullptr);
        if (videoAdjustmentsDragging_ >= 0) UpdateVideoAdjustmentSlider(videoAdjustmentsDragging_, point);
        if (videoScrubbing_) UpdateVideoScrub(point);
    }
    void VideoControlsMouseLeave() {
        if (!VideoActive()) return;
        videoControlsPointerOver_ = false;
        videoControlsHovered_ = ButtonKind::None;
        if (videoPlayer_.Playing() && !videoScrubbing_ && !videoAdjustmentsPanelOpen_ && !videoPlaybackSpeedPanelOpen_) SetTimer(window_, kVideoControlsTimer, static_cast<UINT>(kVideoControlsIdleDelayMs), nullptr);
    }
    void CancelVideoControlsInteraction() {
        StopVideoStepHold();
        videoAdjustmentsDragging_ = -1;
        if (!videoScrubbing_) return;
        videoScrubbing_ = false;
        const bool resumePlayback = videoWasPlayingBeforeScrub_;
        videoWasPlayingBeforeScrub_ = false;
        if (resumePlayback && VideoActive() && !videoPlayer_.Playing()) ToggleVideoPlayPause();
        else ShowVideoControls();
    }
    void UpdateVideoControlsFade() {
        if (!VideoActive()) { StopVideoControls(); return; }
        if (!videoPlayer_.Playing() || videoControlsPointerOver_ || videoScrubbing_ || videoAdjustmentsPanelOpen_ || videoPlaybackSpeedPanelOpen_) { KillTimer(window_, kVideoControlsTimer); return; }
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
    bool DeleteConfirmationOpen() const { return overlay_ == OverlayKind::DeleteConfirm; }
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
        StopVideoStepHold();
        DismissTriangleCountTooltip(false);
        DismissDropdown();
        DismissContextMenu();
        if (overlay == OverlayKind::DeleteConfirm) deleteWarningSuppressOnConfirm_ = false;
        if (overlay == OverlayKind::Settings) {
            settingsPage_ = SettingsPage::General;
            settingsScroll_ = 0.0f;
        }
        if (overlay == OverlayKind::Help) { helpTopic_ = 0; helpTopicHover_ = -1; helpScroll_ = 0.0f; }
        overlay_ = overlay;
        EndPan();
        ShowVideoControls();
        InvalidateRect(window_, nullptr, FALSE);
    }
    void DismissOverlay() {
        if (!HasOverlay()) return;
        overlay_ = OverlayKind::None;
        if (alwaysShowFilmstrip_) StartFilmstripHold(UINT_MAX);
        ShowVideoControls();
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
    int MeasureHelpTextHeight(const wchar_t* text, int width, float size, DWRITE_FONT_WEIGHT weight) const {
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
    RECT GetHelpCloseBounds() const {
        const RECT bounds = GetOverlayBounds(); const int dpi = GetDpiForWindow(window_);
        const int size = MulDiv(32, dpi, 96), inset = MulDiv(14, dpi, 96);
        return { bounds.right - inset - size, bounds.top + inset, bounds.right - inset, bounds.top + inset + size };
    }
    bool HelpCloseContains(POINT point) const {
        const RECT bounds = GetHelpCloseBounds();
        return overlay_ == OverlayKind::Help && PtInRect(&bounds, point);
    }
    RECT GetHelpRailBounds() const {
        const RECT bounds = GetOverlayBounds(); const int dpi = GetDpiForWindow(window_);
        const int left = bounds.left + MulDiv(18, dpi, 96);
        const int top = bounds.top + MulDiv(64, dpi, 96);
        return { left, top, left + MulDiv(178, dpi, 96), bounds.bottom - MulDiv(18, dpi, 96) };
    }
    RECT GetHelpContentBounds() const {
        const RECT bounds = GetOverlayBounds(); const int dpi = GetDpiForWindow(window_);
        const int left = bounds.left + MulDiv(220, dpi, 96);
        return { left, bounds.top + MulDiv(64, dpi, 96), bounds.right - MulDiv(24, dpi, 96), bounds.bottom - MulDiv(24, dpi, 96) };
    }
    int GetHelpTopicRowHeight() const {
        const RECT rail = GetHelpRailBounds();
        return std::max(MulDiv(18, GetDpiForWindow(window_), 96), static_cast<int>((rail.bottom - rail.top) / static_cast<LONG>(kHelpTopics.size())));
    }
    RECT GetHelpTopicBounds(int topic) const {
        const RECT rail = GetHelpRailBounds(); const int height = GetHelpTopicRowHeight();
        const int top = rail.top + topic * height;
        return { rail.left, top, rail.right, std::min<LONG>(rail.bottom, static_cast<LONG>(top) + height) };
    }
    int HelpTopicAt(POINT point) const {
        if (overlay_ != OverlayKind::Help) return -1;
        const RECT rail = GetHelpRailBounds();
        if (!PtInRect(&rail, point)) return -1;
        const int topic = (point.y - rail.top) / GetHelpTopicRowHeight();
        return topic >= 0 && topic < static_cast<int>(kHelpTopics.size()) ? topic : -1;
    }
    bool HelpContentContains(POINT point) const {
        const RECT bounds = GetHelpContentBounds();
        return overlay_ == OverlayKind::Help && PtInRect(&bounds, point);
    }
    void SetHelpTopic(int topic) {
        if (topic < 0 || topic >= static_cast<int>(kHelpTopics.size())) return;
        helpTopic_ = topic;
        helpScroll_ = 0.0f;
        InvalidateRect(window_, nullptr, FALSE);
    }
    int HelpContentHeight() const {
        if (overlay_ != OverlayKind::Help) return 0;
        const RECT content = GetHelpContentBounds(); const UINT dpi = GetDpiForWindow(window_);
        const int width = static_cast<int>(std::max<LONG>(1, content.right - content.left));
        int height = MeasureHelpTextHeight(kHelpTopics[helpTopic_].title, width, 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD) + MulDiv(14, dpi, 96);
        const HelpTopic& topic = kHelpTopics[helpTopic_];
        if (topic.sectionCount == 0)
            return height + MeasureHelpTextHeight(topic.body, width, 14.0f, DWRITE_FONT_WEIGHT_NORMAL);
        for (size_t index = 0; index < topic.sectionCount; ++index) {
            const HelpSection& section = topic.sections[index];
            height += MeasureHelpTextHeight(section.heading, width, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            height += MulDiv(4, dpi, 96) + MeasureHelpTextHeight(section.body, width, 14.0f, DWRITE_FONT_WEIGHT_NORMAL);
            height += MulDiv(14, dpi, 96);
        }
        return height - MulDiv(14, dpi, 96);
    }
    float HelpMaximumScroll() const {
        if (overlay_ != OverlayKind::Help) return 0.0f;
        const RECT bounds = GetHelpContentBounds();
        const float viewport = static_cast<float>(bounds.bottom - bounds.top);
        return std::max(0.0f, static_cast<float>(HelpContentHeight()) - viewport);
    }
    void ScrollHelp(float delta) {
        if (overlay_ != OverlayKind::Help) return;
        helpScroll_ = std::clamp(helpScroll_ + delta, 0.0f, HelpMaximumScroll());
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
        if (settingsPage_ == SettingsPage::Video2D) return GetSettingsVideoPlaceholderBounds().bottom - GetOverlayBounds().top;
        return GetSettingsSpaceMouseBounds().bottom - GetOverlayBounds().top;
    }
    RECT GetSettingsVideoPlaceholderBounds() const {
        const RECT bounds = GetOverlayBounds();
        const int top = bounds.top + MulDiv(static_cast<int>(kSettingsFirstRowTopDips), GetDpiForWindow(window_), 96);
        const wchar_t* text = L"video-specific settings will appear here as they are added.";
        const int height = MeasureSettingsTextHeight(text, SettingsContentRight() - SettingsContentLeft(), 16.0f, DWRITE_FONT_WEIGHT_NORMAL);
        return { SettingsContentLeft(), top, SettingsContentRight(), top + height };
    }
    RECT GetSettingsOptionBounds(int option) const {
        const RECT bounds = GetOverlayBounds();
        const int firstTop = bounds.top + MulDiv(static_cast<int>(kSettingsFirstRowTopDips), GetDpiForWindow(window_), 96);
        if (settingsPage_ == SettingsPage::General) {
            const RECT remember = GetSettingsSingleColumnBounds(firstTop, L"remember application position and size");
            const RECT include = GetSettingsSingleColumnBounds(remember.bottom + SettingsStackGap(), L"include hidden images in folder");
            const RECT confirm = GetSettingsSingleColumnBounds(include.bottom + SettingsStackGap(), L"confirm before deleting images");
            const RECT swipe = GetSettingsSingleColumnBounds(confirm.bottom + SettingsStackGap(), L"swipe to navigate when fit");
            return option == 0 ? remember : option == 1 ? include : option == 2 ? confirm : swipe;
        }
        if (settingsPage_ == SettingsPage::Image2D) {
            const RECT animations = GetSettingsSingleColumnBounds(firstTop, L"animations and face effects");
            const RECT reverse = GetSettingsSingleColumnBounds(animations.bottom + SettingsStackGap(), L"reverse mouse wheel zoom direction");
            const RECT filmstrip = GetSettingsSingleColumnBounds(reverse.bottom + SettingsStackGap(), L"always show filmstrip");
            if (option == 4) return animations;
            if (option == 5) return reverse;
            if (option == 6) return filmstrip;
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
    RECT GetSettingsThemeBounds(ThemePreference preference) const {
        const RECT swipe = GetSettingsOptionBounds(3);
        const int buttonWidth = MulDiv(76, GetDpiForWindow(window_), 96), gap = MulDiv(8, GetDpiForWindow(window_), 96);
        const int left = SettingsContentLeft() + static_cast<int>(preference) * (buttonWidth + gap);
        RECT result{ left, swipe.bottom + SettingsSectionGap() + MulDiv(static_cast<int>(kSettingsSectionHeadingHeightDips + kSettingsLabelToControlGapDips), GetDpiForWindow(window_), 96), left + buttonWidth, 0 };
        result.bottom = result.top + MulDiv(static_cast<int>(kSettingsControlHeightDips), GetDpiForWindow(window_), 96);
        return result;
    }
    RECT GetSettingsScalingBounds(ImageScaling scaling) const {
        const RECT reverse = GetSettingsOptionBounds(6);
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
    void ToggleSwipeToNavigateWhenFit() {
        swipeToNavigateWhenFit_ = !swipeToNavigateWhenFit_;
        WriteSetting(L"SwipeToNavigateWhenFit", swipeToNavigateWhenFit_ ? 1 : 0);
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
    void ToggleAlwaysShowFilmstrip() {
        alwaysShowFilmstrip_ = !alwaysShowFilmstrip_;
        WriteSetting(L"AlwaysShowFilmstrip", alwaysShowFilmstrip_ ? 1 : 0);
        if (alwaysShowFilmstrip_) StartFilmstripHold(UINT_MAX);
        else BeginFilmstripFadeSequence();
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
        const int primaryWidth = MulDiv(76, dpi, 96);
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
        const int horizontalPadding = MulDiv(32, dpi, 96);
        const int openWidth = std::max(MulDiv(128, dpi, 96),
            MeasureSettingsTextWidth(L"choose defaults", 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD) + horizontalPadding);
        const int cancelWidth = std::max(MulDiv(92, dpi, 96),
            MeasureSettingsTextWidth(L"cancel", 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD) + horizontalPadding);
        const int height = MulDiv(40, dpi, 96), gap = MulDiv(12, dpi, 96);
        const int groupWidth = cancelWidth + gap + openWidth;
        const int left = bounds.left + (bounds.right - bounds.left - groupWidth) / 2;
        const int top = bounds.bottom - MulDiv(22, dpi, 96) - height;
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
    RECT GetPrintErrorDismissButtonBounds() const {
        const RECT bounds = GetOverlayBounds();
        const UINT dpi = GetDpiForWindow(window_);
        const int width = MulDiv(96, dpi, 96), height = MulDiv(36, dpi, 96);
        const int left = bounds.left + (bounds.right - bounds.left - width) / 2;
        const int top = bounds.bottom - MulDiv(24, dpi, 96) - height;
        return { left, top, left + width, top + height };
    }
    bool PrintErrorDismissButtonContains(POINT point) const {
        const RECT button = GetPrintErrorDismissButtonBounds();
        return (overlay_ == OverlayKind::PrintError || overlay_ == OverlayKind::RegistrationError) && PtInRect(&button, point);
    }
    bool CanvasNavigationButtonsVisible() const {
        return (source_ || VideoActive()) && navigationBuilt_ && navigationFiles_.size() > 1 &&
            !HasOverlay() && !TutorialActive() && !dropdownOpen_ && !contextMenuOpen_;
    }
    RECT GetCanvasNavigationZoneBounds(bool next) const {
        const D2D1_RECT_F canvas = ImageCanvasBounds();
        const UINT dpi = GetDpiForWindow(window_);
        const int canvasLeft = static_cast<int>(canvas.left);
        const int canvasTop = static_cast<int>(canvas.top);
        const int canvasRight = static_cast<int>(canvas.right);
        const int canvasBottom = static_cast<int>(canvas.bottom);
        const int canvasWidth = std::max(1, canvasRight - canvasLeft);
        const int canvasHeight = std::max(1, canvasBottom - canvasTop);
        const int desiredWidth = MulDiv(112, dpi, 96);
        const int minimumCenterWidth = MulDiv(160, dpi, 96);
        const int zoneWidth = std::min(desiredWidth, std::max(0, (canvasWidth - minimumCenterWidth) / 2));
        const int requestedVerticalInset = MulDiv(100, dpi, 96);
        const int maximumVerticalInset = std::min((canvasHeight - 1) / 2, (canvasHeight * 2) / 5);
        const int verticalInset = std::min(requestedVerticalInset, maximumVerticalInset);
        const int navigationTop = canvasTop + verticalInset;
        const int navigationBottom = std::max(navigationTop + 1, canvasBottom - verticalInset);
        return next ? RECT{ canvasRight - zoneWidth, navigationTop, canvasRight, navigationBottom } :
            RECT{ canvasLeft, navigationTop, canvasLeft + zoneWidth, navigationBottom };
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
    bool EmptyStatePresentationActive() const { return TutorialActive() || EmptyStateActive(); }
    ButtonKind ButtonAt(POINT point) const {
        const auto contains = [&point](RECT bounds) { return PtInRect(&bounds, point) != FALSE; };
        if (HelpCloseContains(point)) return ButtonKind::HelpClose;
        const int helpTopic = HelpTopicAt(point);
        if (helpTopic >= 0) { helpTopicHit_ = helpTopic; return ButtonKind::HelpTopic; }
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
            if (containsNavigation(SettingsPage::Video2D)) return ButtonKind::SettingsVideoPage;
            if (containsNavigation(SettingsPage::Model3D)) return ButtonKind::SettingsModel3DPage;
            POINT settingsPoint = point;
            settingsPoint.y += static_cast<LONG>(std::lround(settingsScroll_));
            const auto settingsContains = [&settingsPoint](RECT bounds) { return PtInRect(&bounds, settingsPoint) != FALSE; };
            if (settingsPage_ == SettingsPage::General) {
                if (settingsContains(GetSettingsOptionBounds(0))) return ButtonKind::SettingsRememberPlacement;
                if (settingsContains(GetSettingsOptionBounds(1))) return ButtonKind::SettingsIncludeHidden;
                if (settingsContains(GetSettingsOptionBounds(2))) return ButtonKind::SettingsConfirmDelete;
                if (settingsContains(GetSettingsOptionBounds(3))) return ButtonKind::SettingsSwipeToNavigateWhenFit;
                if (settingsContains(GetSettingsThemeBounds(ThemePreference::System))) return ButtonKind::SettingsThemeSystem;
                if (settingsContains(GetSettingsThemeBounds(ThemePreference::Light))) return ButtonKind::SettingsThemeLight;
                if (settingsContains(GetSettingsThemeBounds(ThemePreference::Dark))) return ButtonKind::SettingsThemeDark;
                if (SettingsDefaultAppsButtonContains(point)) return ButtonKind::SettingsDefaultApps;
            } else if (settingsPage_ == SettingsPage::Image2D) {
                if (zoomHudPositionMenuOpen_) { const RECT menu = GetSettingsZoomHudMenuBounds(); if (PtInRect(&menu, settingsPoint)) { const int row = MulDiv(30, GetDpiForWindow(window_), 96); return settingsPoint.y < menu.top + row ? ButtonKind::SettingsZoomHudBottomLeft : settingsPoint.y < menu.top + row * 2 ? ButtonKind::SettingsZoomHudBottomRight : settingsPoint.y < menu.top + row * 3 ? ButtonKind::SettingsZoomHudTopLeft : ButtonKind::SettingsZoomHudTopRight; } }
                if (SettingsScrollUpVisible() && SettingsScrollIndicatorContains(point, true)) return ButtonKind::SettingsScrollUp;
                if (SettingsScrollDownVisible() && SettingsScrollIndicatorContains(point, false)) return ButtonKind::SettingsScrollDown;
                if (settingsContains(GetSettingsOptionBounds(4))) return ButtonKind::SettingsAnimations;
                if (settingsContains(GetSettingsOptionBounds(5))) return ButtonKind::SettingsReverseWheelZoom;
                if (settingsContains(GetSettingsOptionBounds(6))) return ButtonKind::SettingsAlwaysShowFilmstrip;
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
        if (source_ && renderTarget_) { const ImageZoomHudLayout hud = GetImageZoomHudLayout(); if (PtInRect(&hud.adjustments, point)) return ButtonKind::ImageAdjustments; }
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
        if (PrintErrorDismissButtonContains(point)) return ButtonKind::PrintErrorDismiss;
        return ButtonKind::None;
    }
    void SetButtonHover(ButtonKind button) {
        const int helpTopic = button == ButtonKind::HelpTopic ? helpTopicHit_ : -1;
        if (hoveredButton_ == button && helpTopicHover_ == helpTopic) return;
        hoveredButton_ = button;
        helpTopicHover_ = helpTopic;
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
        else if (button == ButtonKind::SettingsVideoPage) { settingsPage_ = SettingsPage::Video2D; settingsScroll_ = 0.0f; InvalidateRect(window_, nullptr, FALSE); }
        else if (button == ButtonKind::SettingsModel3DPage) { settingsPage_ = SettingsPage::Model3D; settingsScroll_ = 0.0f; InvalidateRect(window_, nullptr, FALSE); }
        else if (button == ButtonKind::SettingsRememberPlacement) ToggleRememberWindowPlacement();
        else if (button == ButtonKind::SettingsIncludeHidden) ToggleIncludeHiddenImages();
        else if (button == ButtonKind::SettingsConfirmDelete) ToggleConfirmBeforeDeleting();
        else if (button == ButtonKind::SettingsSwipeToNavigateWhenFit) ToggleSwipeToNavigateWhenFit();
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
        else if (button == ButtonKind::SettingsAlwaysShowFilmstrip) ToggleAlwaysShowFilmstrip();
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
        else if (button == ButtonKind::ImageAdjustments) SetImageAdjustmentsPanelOpen(!imageAdjustmentsPanelOpen_);
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
            if (!RegisterDefaultAppCapabilities()) { ShowOverlay(OverlayKind::RegistrationError); return; }
            overlay_ = OverlayKind::Welcome;
            CompleteWelcome(true);
            OpenRegisteredDefaultApps(false);
        }
        else if (button == ButtonKind::FeedbackBug || button == ButtonKind::FeedbackFeature) {
            DismissOverlay();
            const wchar_t* url = button == ButtonKind::FeedbackBug ? kBugReportUrl : kFeatureRequestUrl;
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", url, nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                ShowActionError(L"Viewtrious couldn't open the feedback page.");
        }
        else if (button == ButtonKind::HelpClose) DismissOverlay();
        else if (button == ButtonKind::HelpTopic) SetHelpTopic(helpTopicHit_);
        else if (button == ButtonKind::PrintErrorDismiss) DismissOverlay();
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
        if (!DeleteSettingsValues()) {
            resetInProgress_ = false;
            ShowActionError(L"Viewtrious could not reset its preferences.");
            return;
        }
        CleanupWallpaperStaging();
        rememberWindowPlacement_ = true;
        includeHiddenImages_ = true;
        confirmBeforeDeleting_ = true;
        swipeToNavigateWhenFit_ = false;
        showZoomPercentage_ = true;
        animationsEnabled_ = true;
        alwaysShowFilmstrip_ = false;
        reverseMouseWheelZoom_ = false;
        themePreference_ = ThemePreference::System;
        graphicsAdapterAuto_ = true;
        graphicsAdapterLuid_ = {};
        videoPreferredPlaybackRatePercent_ = 100;
        videoEffectivePlaybackRate_ = 1.0;
        wchar_t modulePath[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath))) { DestroyWindow(window_); return; }
        std::wstring command = L"\"" + std::wstring(modulePath) + L"\"";
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
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
    void ToggleVideoFullscreen() {
        if (!VideoActive()) return;
        ToggleFullscreen();
    }
    bool ConsumeVideoFullscreenButtonDoubleClick() {
        if (videoFullscreenToggleTick_ == 0) return false;
        const ULONGLONG elapsed = GetTickCount64() - videoFullscreenToggleTick_;
        videoFullscreenToggleTick_ = 0;
        return elapsed <= GetDoubleClickTime();
    }
    void ToggleFullscreen() {
        DismissContextMenu();
        if (!fullscreen_) DismissOverlay();
        if (VideoActive()) {
            SetVideoPlaybackSpeedPanelOpen(false);
            SetVideoAdjustmentsPanelOpen(false);
        }
        if (!fullscreen_) {
            fullscreenStyle_ = GetWindowLongPtrW(window_, GWL_STYLE);
            fullscreenExStyle_ = GetWindowLongPtrW(window_, GWL_EXSTYLE);
            fullscreenPlacement_ = { sizeof(fullscreenPlacement_) };
            if (!GetWindowPlacement(window_, &fullscreenPlacement_)) return;
            GetWindowRect(window_, &fullscreenRect_);
            MONITORINFO monitor{ sizeof(monitor) };
            GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
            SetWindowLongPtrW(window_, GWL_STYLE, fullscreenStyle_ & ~WS_OVERLAPPEDWINDOW);
            SetWindowLongPtrW(window_, GWL_EXSTYLE, fullscreenExStyle_);
            fullscreen_ = true;
            SetWindowPos(window_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            ApplyWindowCornerPreference(window_, false);
        } else {
            SetWindowLongPtrW(window_, GWL_STYLE, fullscreenStyle_);
            SetWindowLongPtrW(window_, GWL_EXSTYLE, fullscreenExStyle_);
            fullscreen_ = false;
            SetWindowPos(window_, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER);
            SetWindowPlacement(window_, &fullscreenPlacement_);
            ApplyWindowCornerPreference(window_, !IsZoomed(window_));
        }
        if (VideoActive()) ShowVideoControls();
        InvalidateRect(window_, nullptr, FALSE);
    }
    void Paint() {
        PAINTSTRUCT paint{};
        BeginPaint(window_, &paint);
        const bool videoPaint = VideoActive();
        if (videoPaint) videoPlayer_.RecordFramePacingPaint();
        EnsureRenderTarget();
        if (renderTarget_ && graphicsHost_.Ready()) {
            if (ModelActive() && !TutorialActive()) modelViewport_.Render(graphicsHost_, ModelCanvasBounds());
            graphicsHost_.BeginDraw();
            if (contentKind_ != ContentKind::Model3D || !ModelActive() || TutorialActive()) renderTarget_->Clear(kViewerBackground);
            if (VideoActive() && !tutorialPresentation_) {
                // A paused scrub explicitly owns this retry; ordinary paints stay cache-only.
                if (videoPausedSeekRefreshPending_ && videoPlayer_.UpdateFrame(VideoPlayer::FrameAcquisitionReason::Seek))
                    videoPausedSeekRefreshPending_ = false;
                videoPlayer_.Draw(renderTarget_.Get(), ModelCanvasBounds(), VideoCurrentScale(), videoPan_);
                DrawCanvasNavigationButtons();
                DrawVideoPlaybackControls();
            }
            if (source_ && !tutorialPresentation_) {
                EnsureBitmap();
                if (bitmap_) { if (dissolveActive_) DrawStillDissolve(); else DrawImage(); DrawZoomHud(); DrawCanvasNavigationButtons(); DrawFilmstrip(); }
            } else if (EmptyStatePresentationActive()) DrawEmptyState();
            if (ModelActive() && !tutorialPresentation_) { DrawModelAxisIndicator(); TraceOffscreenModelIndicatorState(); DrawOffscreenModelIndicator(); DrawModelViewBar(); }
            if (!tutorialPresentation_) DrawModelLoadingOverlay();
            if (!tutorialPresentation_) DrawRevisionLabel();
            DrawTitleBar();
            DrawTriangleCountTooltip();
            DrawDropdown();
            DrawContextMenu();
            DrawOpenWithSubmenu();
            DrawOverlay();
            if (!tutorialPresentation_) DrawCopyFeedback();
            DrawTutorial();
            const HRESULT hr = graphicsHost_.EndDraw();
            if (SUCCEEDED(hr) && (bitmap_ || VideoActive()) && !tutorialPresentation_) MarkFirstPresentation();
            if (hr == D2DERR_RECREATE_TARGET) DiscardRenderResources();
            else if (SUCCEEDED(hr)) {
                const HRESULT present = graphicsHost_.Present();
                if (videoPaint) videoPlayer_.RecordFramePacingPresent(present);
            }
        }
        EndPaint(window_, &paint);
    }

    void Resize() {
        if (graphicsHost_.Ready()) {
            RECT client{};
            GetClientRect(window_, &client);
            std::wstring error;
            if (!graphicsHost_.Resize(std::max(1L, client.right - client.left), std::max(1L, client.bottom - client.top), static_cast<float>(GetDpiForWindow(window_)), error)) error_ = error;
            renderTarget_ = graphicsHost_.D2DContext();
            bitmap_.Reset(); lanczosBitmap_.Reset(); imageAdjustedBitmap_.Reset(); aboutLogo_.Reset(); aboutLogoWidth_ = 0; aboutLogoHeight_ = 0;
            topBarLogo_.Reset(); topBarLogoWidth_ = 0; topBarLogoHeight_ = 0; checkerboardBrush_.Reset(); checkerboardBitmap_.Reset();
            filmstripVideoIcon_.Reset(); filmstripVideoIconSize_ = 0;
            if (VideoActive()) videoPlayer_.HandleRenderTargetResize();
        }
        if (!tutorialPresentation_ && !fitToWindow_ && zoom_ < MinimumScale()) CenterAtMinimumScale();
        settingsScroll_ = std::min(settingsScroll_, SettingsMaximumScroll());
        helpScroll_ = std::min(helpScroll_, HelpMaximumScroll());
        ClampPan();
        ClampVideoPan();
        filmstripPreviewGeometryValid_ = false;
        RebuildFilmstripLayout();
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
            const std::vector<fs::path> previousNavigationFiles = navigationFiles_;
            CancelQueuedFilmstripThumbnails();
            CancelQueuedFilmstripHoverPreviews();
            ++filmstripHoverPreviewGeneration_;
            videoHoverPreviewGeneration_.store(filmstripHoverPreviewGeneration_, std::memory_order_release);
            CancelFilmstripVideoHoverFade();
            filmstripHoveredIndex_ = -1;
            HideFilmstripHoverPreviewImmediately();
            ++navigationFolderGeneration_;
            filmstripThumbnailFolderGeneration_.store(navigationFolderGeneration_, std::memory_order_release);
            navigationFiles_ = std::move(scannedFiles);
            filmstripClickedRevealTarget_.reset();
            filmstripThumbnailGenerations_.assign(navigationFiles_.size(), ++filmstripThumbnailGenerationSeed_);
            RemapFilmstripAspectMetadata(previousNavigationFiles);
            filmstripThumbnails_.clear();
            filmstripThumbnailFailures_.clear();
            filmstripHoverPreviews_.clear();
            filmstripScroll_ = 0.0;
            filmstripDemandFirst_ = filmstripDemandLast_ = filmstripDemandCurrent_ = std::numeric_limits<size_t>::max();
            filmstripLayoutRebuildPending_ = false;
            StopFilmstripScrollAnimation();
        }
        if ((changed || currentRenamed) && imageDecodePending_) {
            ++decodeRequestGeneration_;
            pendingFullDecode_ = DecodeRequest{ currentPath_, decodeRequestGeneration_, navigationFolderGeneration_ };
            QueueLatestFullDecode();
        }
        navigationBuilt_ = true;
        RebuildFilmstripLayout();
        InvalidateRect(window_, nullptr, FALSE);
    }

    bool FilmstripEligible() const {
        return source_ && navigationBuilt_ && navigationFiles_.size() > 1 && !HasOverlay() && !TutorialActive() && !tutorialPresentation_;
    }
    bool FilmstripVisible() const { return FilmstripEligible() && filmstripOpacity_ > 0.001f; }
    int FilmstripHeight() const {
        if (!FilmstripEligible()) return 0;
        RECT client{};
        GetClientRect(window_, &client);
        const int canvasTop = fullscreen_ ? 0 : GetFrameMetrics(window_).titleBarHeight;
        return client.bottom - canvasTop < MulDiv(560, GetDpiForWindow(window_), 96) ?
            MulDiv(96, GetDpiForWindow(window_), 96) : MulDiv(112, GetDpiForWindow(window_), 96);
    }
    int FilmstripThumbnailHeight() const {
        return std::max(1, std::min(MulDiv(92, GetDpiForWindow(window_), 96), FilmstripHeight() - MulDiv(20, GetDpiForWindow(window_), 96)));
    }
    int FilmstripThumbnailMinimumWidth() const { return static_cast<int>(std::lround(FilmstripThumbnailHeight() * 2.0f / 3.0f)); }
    int FilmstripThumbnailMaximumWidth() const { return static_cast<int>(std::lround(FilmstripThumbnailHeight() * 16.0f / 9.0f)); }
    int FilmstripGap() const { return MulDiv(22, GetDpiForWindow(window_), 96); }
    int FilmstripPadding() const { return MulDiv(14, GetDpiForWindow(window_), 96); }
    int FindFilmstripThumbnail(const std::wstring& path, uint64_t itemGeneration) const {
        const auto found = std::find_if(filmstripThumbnails_.begin(), filmstripThumbnails_.end(), [&](const FilmstripThumbnailEntry& entry) {
            return entry.itemGeneration == itemGeneration && PathsEqual(fs::path(entry.path), fs::path(path));
        });
        return found == filmstripThumbnails_.end() ? -1 : static_cast<int>(std::distance(filmstripThumbnails_.begin(), found));
    }
    bool FilmstripThumbnailPending(const std::wstring& path, uint64_t itemGeneration) const {
        return std::any_of(filmstripThumbnailPending_.begin(), filmstripThumbnailPending_.end(), [&](const FilmstripThumbnailRequest& request) {
            return request.itemGeneration == itemGeneration && PathsEqual(fs::path(request.path), fs::path(path));
        });
    }
    bool FilmstripThumbnailFailed(const std::wstring& path, uint64_t itemGeneration) const {
        return std::any_of(filmstripThumbnailFailures_.begin(), filmstripThumbnailFailures_.end(), [&](const FilmstripThumbnailRequest& request) {
            return request.itemGeneration == itemGeneration && PathsEqual(fs::path(request.path), fs::path(path));
        });
    }
    float FilmstripPlaceholderAspect(size_t index) const {
        if (index < filmstripLayoutAspects_.size()) return filmstripLayoutAspects_[index];
        return index < navigationFiles_.size() && IsVideoPath(navigationFiles_[index].wstring()) ? 16.0f / 9.0f : 1.0f;
    }
    RECT GetFilmstripBounds() const {
        RECT client{};
        GetClientRect(window_, &client);
        const int height = FilmstripHeight();
        if (!height) return {};
        const int dpi = GetDpiForWindow(window_);
        const int minimumWidth = MulDiv(180, dpi, 96);
        const int desiredMargin = MulDiv(150, dpi, 96);
        const int sideMargin = std::min(desiredMargin, std::max(MulDiv(16, dpi, 96), (static_cast<int>(client.right) - minimumWidth) / 2));
        const int maximumWidth = std::max(minimumWidth, static_cast<int>(client.right) - sideMargin * 2);
        const float contentWidth = filmstripItemOffsets_.empty() ? static_cast<float>(minimumWidth) :
            filmstripItemOffsets_.back() - static_cast<float>(FilmstripGap()) + static_cast<float>(FilmstripPadding());
        const int width = std::min(maximumWidth, std::max(minimumWidth, static_cast<int>(std::ceil(contentWidth))));
        const int left = (client.right - width) / 2;
        const int bottomMargin = MulDiv(16, dpi, 96);
        return { left, client.bottom - bottomMargin - height, left + width, client.bottom - bottomMargin };
    }
    bool FilmstripContains(POINT point) const { const RECT bounds = GetFilmstripBounds(); return FilmstripVisible() && PtInRect(&bounds, point); }
    RECT GetFilmstripHoverDelaySliderBounds() const {
        RECT client{};
        GetClientRect(window_, &client);
        const int dpi = GetDpiForWindow(window_);
        const int width = MulDiv(280, dpi, 96), height = MulDiv(24, dpi, 96);
        const int top = (fullscreen_ ? 0 : GetFrameMetrics(window_).titleBarHeight) + MulDiv(10, dpi, 96);
        return { (client.right - width) / 2, top, (client.right + width) / 2, top + height };
    }
    bool BeginFilmstripHoverDelaySlider(POINT point) {
        const RECT bounds = GetFilmstripHoverDelaySliderBounds();
        if (!FilmstripEligible() || !PtInRect(&bounds, point)) return false;
        filmstripHoverDelaySliderDragging_ = true;
        UpdateFilmstripHoverDelaySlider(point);
        return true;
    }
    bool FilmstripHoverDelaySliderDragging() const { return filmstripHoverDelaySliderDragging_; }
    void UpdateFilmstripHoverDelaySlider(POINT point) {
        if (!filmstripHoverDelaySliderDragging_) return;
        const RECT bounds = GetFilmstripHoverDelaySliderBounds();
        const int dpi = GetDpiForWindow(window_);
        const float trackLeft = static_cast<float>(bounds.left + MulDiv(112, dpi, 96));
        const float trackRight = static_cast<float>(bounds.right - MulDiv(12, dpi, 96));
        const float position = std::clamp((point.x - trackLeft) / std::max(1.0f, trackRight - trackLeft), 0.0f, 1.0f);
        filmstripHoverPreviewDelayMs_ = static_cast<UINT>(std::lround(100.0f + position * 900.0f));
        InvalidateRect(window_, nullptr, FALSE);
    }
    void EndFilmstripHoverDelaySlider() { filmstripHoverDelaySliderDragging_ = false; }
    float FilmstripThumbnailWidth(size_t index) const { return filmstripItemWidths_[index]; }
    float FilmstripMaximumScroll() const {
        const RECT bounds = GetFilmstripBounds();
        const float contentWidth = filmstripItemOffsets_.empty() ? 0.0f : filmstripItemOffsets_.back() - FilmstripGap() + FilmstripPadding();
        return std::max(0.0f, contentWidth - static_cast<float>(bounds.right - bounds.left));
    }
    size_t CurrentNavigationIndex() const {
        const fs::path current(currentPath_);
        const auto found = std::find_if(navigationFiles_.begin(), navigationFiles_.end(), [&current](const fs::path& path) { return PathsEqual(path, current); });
        return found == navigationFiles_.end() ? 0 : static_cast<size_t>(std::distance(navigationFiles_.begin(), found));
    }
    void RebuildFilmstripLayout(bool clampScroll = true, bool queueThumbnails = true) {
        const size_t count = navigationFiles_.size();
        filmstripItemWidths_.resize(count);
        filmstripItemOffsets_.resize(count + 1);
        const float height = static_cast<float>(FilmstripThumbnailHeight());
        const float minimum = static_cast<float>(FilmstripThumbnailMinimumWidth());
        const float maximum = static_cast<float>(FilmstripThumbnailMaximumWidth());
        float offset = static_cast<float>(FilmstripPadding());
        for (size_t index = 0; index < count; ++index) {
            filmstripItemOffsets_[index] = offset;
            filmstripItemWidths_[index] = std::clamp(height * FilmstripPlaceholderAspect(index), minimum, maximum);
            offset += filmstripItemWidths_[index] + FilmstripGap();
        }
        if (!filmstripItemOffsets_.empty()) filmstripItemOffsets_.back() = offset;
        if (clampScroll) {
            filmstripScroll_ = std::clamp(filmstripScroll_, 0.0, static_cast<double>(FilmstripMaximumScroll()));
            if ((filmstripScroll_ <= 0.0 && filmstripScrollVelocity_ < 0.0) ||
                (filmstripScroll_ >= FilmstripMaximumScroll() && filmstripScrollVelocity_ > 0.0)) {
                filmstripScrollVelocity_ = 0.0;
            }
        }
        if (queueThumbnails) QueueFilmstripThumbnails();
    }
    struct FilmstripLayoutAnchor {
        size_t index = 0;
        float contentX = 0.0f;
        double renderedX = 0.0;
    };
    float FilmstripContentWidth() const {
        return filmstripItemOffsets_.empty() ? 0.0f : filmstripItemOffsets_.back() - FilmstripGap() + FilmstripPadding();
    }
    std::optional<FilmstripLayoutAnchor> CaptureFilmstripLayoutAnchor() const {
        if (navigationFiles_.empty() || filmstripItemOffsets_.size() != navigationFiles_.size() + 1 ||
            filmstripItemWidths_.size() != navigationFiles_.size()) return std::nullopt;
        const RECT bounds = GetFilmstripBounds();
        const double visibleLeft = filmstripScroll_;
        const double visibleRight = visibleLeft + static_cast<double>(bounds.right - bounds.left);
        const auto makeAnchor = [&](size_t index) {
            return FilmstripLayoutAnchor{ index, filmstripItemOffsets_[index],
                static_cast<double>(bounds.left) + filmstripItemOffsets_[index] - filmstripScroll_ };
        };
        for (size_t index = 0; index < navigationFiles_.size(); ++index) {
            const double left = filmstripItemOffsets_[index];
            const double right = left + FilmstripThumbnailWidth(index);
            if (left >= visibleLeft && right <= visibleRight &&
                (index >= filmstripAspectRelayoutPending_.size() || !filmstripAspectRelayoutPending_[index])) return makeAnchor(index);
        }
        for (size_t index = 0; index < navigationFiles_.size(); ++index) {
            const double left = filmstripItemOffsets_[index];
            const double right = left + FilmstripThumbnailWidth(index);
            if (left >= visibleLeft && right <= visibleRight) return makeAnchor(index);
        }
        for (size_t index = 0; index < navigationFiles_.size(); ++index) {
            const double left = filmstripItemOffsets_[index];
            const double right = left + FilmstripThumbnailWidth(index);
            if (right > visibleLeft && left < visibleRight) return makeAnchor(index);
        }
        return std::nullopt;
    }
#ifdef _DEBUG
    void TraceFilmstripAspectRelayoutBegin(size_t pendingCount, const std::optional<FilmstripLayoutAnchor>& anchor,
        double oldScroll, float oldContentWidth) const {
        wchar_t message[768]{};
        const std::wstring path = anchor && anchor->index < navigationFiles_.size() ? navigationFiles_[anchor->index].wstring() : L"";
        swprintf_s(message, L"[Viewtrious] FILMSTRIP_ASPECT_RELAYOUT_BEGIN pending=%zu anchor=%zu path=%ls oldScroll=%.3f oldContentX=%.3f oldRenderedX=%.3f oldContentWidth=%.3f\n",
            pendingCount, anchor ? anchor->index : static_cast<size_t>(-1), path.c_str(), oldScroll,
            anchor ? anchor->contentX : 0.0f, anchor ? anchor->renderedX : 0.0, oldContentWidth);
        OutputDebugStringW(message);
    }
    void TraceFilmstripAspectRelayoutEnd(const std::optional<FilmstripLayoutAnchor>& anchor, double scrollBeforeCompensation,
        double compensation, double compensatedScroll, double clampedScroll, double renderedX, float contentWidth, bool boundPrevented) const {
        wchar_t message[768]{};
        const double renderedDelta = anchor ? renderedX - anchor->renderedX : 0.0;
        const float contentX = anchor && anchor->index < filmstripItemOffsets_.size() ? filmstripItemOffsets_[anchor->index] : 0.0f;
        swprintf_s(message, L"[Viewtrious] FILMSTRIP_ASPECT_RELAYOUT_END newScrollBefore=%.3f newContentX=%.3f compensation=%.3f compensatedScroll=%.3f clampedScroll=%.3f newRenderedX=%.3f renderedDelta=%.3f newContentWidth=%.3f boundPrevented=%d\n",
            scrollBeforeCompensation, contentX, compensation, compensatedScroll, clampedScroll, renderedX, renderedDelta, contentWidth,
            boundPrevented ? 1 : 0);
        OutputDebugStringW(message);
    }
#endif
    void ApplyFilmstripAspectRelayout(bool queueThumbnails = true, bool force = false) {
        const size_t pendingCount = static_cast<size_t>(std::count(filmstripAspectRelayoutPending_.begin(), filmstripAspectRelayoutPending_.end(), true));
        if (!pendingCount && !force) return;
        const std::optional<FilmstripLayoutAnchor> anchor = CaptureFilmstripLayoutAnchor();
        const RECT oldBounds = GetFilmstripBounds();
        const double oldScroll = filmstripScroll_;
#ifdef _DEBUG
        const float oldContentWidth = FilmstripContentWidth();
        TraceFilmstripAspectRelayoutBegin(pendingCount, anchor, oldScroll, oldContentWidth);
#endif
        for (size_t index = 0; index < filmstripLayoutAspects_.size() && index < filmstripKnownAspects_.size(); ++index) {
            if (index < filmstripAspectRelayoutPending_.size() && filmstripAspectRelayoutPending_[index])
                filmstripLayoutAspects_[index] = filmstripKnownAspects_[index];
        }
        std::fill(filmstripAspectRelayoutPending_.begin(), filmstripAspectRelayoutPending_.end(), false);
        filmstripLayoutRebuildPending_ = false;
        RebuildFilmstripLayout(false, queueThumbnails);
#ifdef _DEBUG
        const double scrollBeforeCompensation = filmstripScroll_;
#endif
        double compensatedScroll = filmstripScroll_;
        if (anchor && anchor->index < filmstripItemOffsets_.size()) {
            const RECT newBounds = GetFilmstripBounds();
            compensatedScroll = oldScroll + (static_cast<double>(newBounds.left) - oldBounds.left) +
                (static_cast<double>(filmstripItemOffsets_[anchor->index]) - anchor->contentX);
        }
        const double clampedScroll = std::clamp(compensatedScroll, 0.0, static_cast<double>(FilmstripMaximumScroll()));
        filmstripScroll_ = clampedScroll;
        if ((filmstripScroll_ <= 0.0 && filmstripScrollVelocity_ < 0.0) ||
            (filmstripScroll_ >= FilmstripMaximumScroll() && filmstripScrollVelocity_ > 0.0)) filmstripScrollVelocity_ = 0.0;
#ifdef _DEBUG
        const bool boundPrevented = std::abs(clampedScroll - compensatedScroll) > 0.01;
        const double renderedX = anchor && anchor->index < filmstripItemOffsets_.size()
            ? static_cast<double>(GetFilmstripBounds().left) + filmstripItemOffsets_[anchor->index] - filmstripScroll_ : 0.0;
        TraceFilmstripAspectRelayoutEnd(anchor, scrollBeforeCompensation, compensatedScroll - scrollBeforeCompensation,
            compensatedScroll, clampedScroll, renderedX, FilmstripContentWidth(), boundPrevented);
        if (filmstripPostStopPosition_ && !filmstripScrollAnimating_) filmstripPostStopPosition_ = filmstripScroll_;
#endif
    }
    bool UpdateFilmstripKnownAspect(size_t index, float aspect) {
        if (index >= filmstripKnownAspects_.size() || index >= filmstripAspectAuthoritative_.size() ||
            index >= filmstripAspectRelayoutPending_.size() || !std::isfinite(aspect) || aspect <= 0.0f) return false;
        const bool changed = std::abs(filmstripKnownAspects_[index] - aspect) > 0.0001f;
        filmstripKnownAspects_[index] = aspect;
        filmstripAspectAuthoritative_[index] = true;
        if (changed) filmstripAspectRelayoutPending_[index] = true;
        return changed;
    }
    void ApplyDeferredFilmstripLayout() {
        if (!filmstripLayoutRebuildPending_) return;
        ApplyFilmstripAspectRelayout();
    }
    std::pair<size_t, size_t> FilmstripVisibleRange() const {
        if (navigationFiles_.empty() || filmstripItemOffsets_.empty()) return { 0, 0 };
        const RECT bounds = GetFilmstripBounds();
        const double visibleLeft = filmstripScroll_;
        const double visibleRight = visibleLeft + static_cast<double>(bounds.right - bounds.left);
        size_t first = 0;
        while (first < navigationFiles_.size() && filmstripItemOffsets_[first] + FilmstripThumbnailWidth(first) < visibleLeft) ++first;
        size_t last = first;
        while (last < navigationFiles_.size() && filmstripItemOffsets_[last] <= visibleRight) ++last;
        return { first, last };
    }
    void TraceFilmstripThumbnailJob(const wchar_t* event, const FilmstripThumbnailRequest& request, HRESULT result = S_OK) const {
#ifdef _DEBUG
        wchar_t message[768]{};
        swprintf_s(message, L"[Viewtrious] %ls tid=%lu hr=0x%08X path=%ls\n", event, GetCurrentThreadId(),
            static_cast<unsigned int>(result), request.path.c_str());
        OutputDebugStringW(message);
#else
        (void)event; (void)request; (void)result;
#endif
    }
#ifdef _DEBUG
    void TraceFilmstripThumbnailStage(const wchar_t* event, const std::wstring& path, ULONGLONG started, HRESULT result) const {
        wchar_t message[768]{};
        swprintf_s(message, L"[Viewtrious] %ls tid=%lu elapsed=%llums hr=0x%08X path=%ls\n", event, GetCurrentThreadId(),
            static_cast<unsigned long long>(GetTickCount64() - started), static_cast<unsigned int>(result), path.c_str());
        OutputDebugStringW(message);
    }
    void TraceFilmstripThumbnailPublication(size_t index, const FilmstripThumbnailEntry& entry, bool layoutChanged, bool layoutDeferred) const {
        const float slotWidth = index < filmstripItemWidths_.size() ? filmstripItemWidths_[index] : 0.0f;
        const float contentWidth = filmstripItemOffsets_.empty() ? 0.0f : filmstripItemOffsets_.back() - FilmstripGap() + FilmstripPadding();
        wchar_t message[512]{};
        const wchar_t* layout = layoutDeferred ? L"deferred" : layoutChanged ? L"rebuild" : L"unchanged";
        swprintf_s(message, L"[Viewtrious] THUMB_RAM_PUBLISHED_UI index=%zu slotWidth=%.2f bitmap=%ux%u aspect=%.3f contentWidth=%.2f scroll=%.3f layout=%ls path=%ls\n",
            index, slotWidth, entry.width, entry.height, entry.aspect, contentWidth, filmstripScroll_,
            layout, entry.path.c_str());
        OutputDebugStringW(message);
    }
#endif
    void StartFilmstripThumbnailWorker() {
        if (shuttingDown_ || filmstripThumbnailStopping_.load(std::memory_order_acquire) || filmstripThumbnailWorkers_.front().joinable()) return;
        filmstripThumbnailStopping_.store(false, std::memory_order_release);
        for (std::thread& worker : filmstripThumbnailWorkers_) {
            worker = std::thread([this] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                for (;;) {
                    FilmstripThumbnailRequest request;
                    {
                        std::unique_lock<std::mutex> lock(filmstripThumbnailMutex_);
                        filmstripThumbnailWake_.wait(lock, [&] {
                            return filmstripThumbnailStopping_.load(std::memory_order_acquire) || !filmstripThumbnailQueue_.empty();
                        });
                        if (filmstripThumbnailStopping_.load(std::memory_order_acquire)) break;
                        request = std::move(filmstripThumbnailQueue_.front());
                        filmstripThumbnailQueue_.pop_front();
                    }
                    if (filmstripThumbnailStopping_.load(std::memory_order_acquire)) continue;
                    if (request.folderGeneration != filmstripThumbnailFolderGeneration_.load(std::memory_order_acquire)) continue;
                    TraceFilmstripThumbnailJob(L"THUMB_JOB_DEQUEUED", request);
                    auto* result = new FilmstripThumbnailResult{};
                    result->request = request;
                    if (IsVideoPath(request.path)) {
                        ShellThumbnailPixels decoded;
                        result->result = DecodeShellVideoThumbnailPixels(request.path, 256, decoded, result->aspect);
                        result->width = decoded.width;
                        result->height = decoded.height;
                        result->stride = decoded.stride;
                        result->pixels = std::move(decoded.pixels);
                    } else {
                        result->result = DecodeFilmstripThumbnailPixels(request.path, request.targetHeight, *result, result->aspect);
                    }
                    // Both decode paths release all source objects before returning, so only copied
                    // Viewtrious-owned RAM pixels can cross onto the UI thread.
                    TraceFilmstripThumbnailJob(SUCCEEDED(result->result) ? L"THUMB_JOB_SUCCESS" : L"THUMB_JOB_FAILED", request, result->result);
                    if (filmstripThumbnailStopping_.load(std::memory_order_acquire)) delete result;
                    else if (PostMessageW(window_, kFilmstripThumbnailCompleteMessage, 0, reinterpret_cast<LPARAM>(result)))
                        TraceFilmstripThumbnailJob(L"THUMB_RAM_PUBLISHED", request, result->result);
                    else delete result;
                }
                if (SUCCEEDED(apartment)) CoUninitialize();
            });
        }
    }
    void CancelQueuedFilmstripThumbnails() {
        std::lock_guard<std::mutex> lock(filmstripThumbnailMutex_);
        filmstripThumbnailQueue_.clear();
        filmstripThumbnailPending_.clear();
    }
    void StopFilmstripThumbnailWorker() {
        filmstripThumbnailStopping_.store(true, std::memory_order_release);
        CancelQueuedFilmstripThumbnails();
        filmstripThumbnailWake_.notify_all();
        for (std::thread& worker : filmstripThumbnailWorkers_)
            if (worker.joinable()) worker.join();
        filmstripThumbnailPending_.clear();
        filmstripThumbnailGenerations_.clear();
        filmstripThumbnails_.clear();
        filmstripThumbnailFailures_.clear();
        MSG message{};
        while (PeekMessageW(&message, window_, kFilmstripThumbnailCompleteMessage, kFilmstripThumbnailCompleteMessage, PM_REMOVE))
            delete reinterpret_cast<FilmstripThumbnailResult*>(message.lParam);
    }
    int FindFilmstripHoverPreview(const std::wstring& path, uint64_t itemGeneration) const {
        const auto found = std::find_if(filmstripHoverPreviews_.begin(), filmstripHoverPreviews_.end(), [&](const FilmstripHoverPreviewEntry& entry) {
            return entry.itemGeneration == itemGeneration && PathsEqual(fs::path(entry.path), fs::path(path));
        });
        return found == filmstripHoverPreviews_.end() ? -1 : static_cast<int>(std::distance(filmstripHoverPreviews_.begin(), found));
    }
    void StartFilmstripHoverPreviewWorker() {
        if (shuttingDown_ || filmstripHoverPreviewStopping_.load(std::memory_order_acquire) || filmstripHoverPreviewWorker_.joinable()) return;
        filmstripHoverPreviewStopping_.store(false, std::memory_order_release);
        filmstripHoverPreviewWorker_ = std::thread([this] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            for (;;) {
                FilmstripHoverPreviewRequest request;
                {
                    std::unique_lock<std::mutex> lock(filmstripHoverPreviewMutex_);
                    filmstripHoverPreviewWake_.wait(lock, [&] {
                        return filmstripHoverPreviewStopping_.load(std::memory_order_acquire) || !filmstripHoverPreviewQueue_.empty();
                    });
                    if (filmstripHoverPreviewStopping_.load(std::memory_order_acquire)) break;
                    request = std::move(filmstripHoverPreviewQueue_.front());
                    filmstripHoverPreviewQueue_.clear(); // Only the newest hover can be relevant.
                }
                auto* result = new FilmstripHoverPreviewResult{};
                result->request = request;
                if (IsVideoPath(request.path)) {
                    VideoHoverFrameStream stream;
                    const HRESULT opened = stream.Open({ request.path, request.hoverGeneration, 768 }, &videoHoverPreviewGeneration_);
                    constexpr LONGLONG kVideoHoverHnsPerSecond = 10000000;
                    constexpr LONGLONG kVideoHoverPresentationInterval = kVideoHoverHnsPerSecond / 24;
                    LARGE_INTEGER qpcFrequency{};
                    LARGE_INTEGER qpcStart{};
                    QueryPerformanceFrequency(&qpcFrequency);
                    UINT decodedFrames = 0;
                    UINT publishedFrames = 0;
                    UINT droppedFrames = 0;
                    LONGLONG lastSourceTimestamp = 0;
                    const auto waitForSourceTime = [&](LONGLONG sourceTime) {
                        for (;;) {
                            if (filmstripHoverPreviewStopping_.load(std::memory_order_acquire) ||
                                videoHoverPreviewGeneration_.load(std::memory_order_acquire) != request.hoverGeneration) return false;
                            LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
                            const LONGLONG deadline = qpcStart.QuadPart + sourceTime * qpcFrequency.QuadPart / kVideoHoverHnsPerSecond;
                            if (now.QuadPart >= deadline) return true;
                            const LONGLONG remainingTicks = deadline - now.QuadPart;
                            const DWORD waitMs = static_cast<DWORD>(std::clamp<LONGLONG>((remainingTicks * 1000 + qpcFrequency.QuadPart - 1) / qpcFrequency.QuadPart, 1, 4));
                            Sleep(waitMs);
                        }
                    };
                    bool timelineStarted = false;
                    LONGLONG timelineStart = 0;
                    LONGLONG nextPresentationTime = 0;
                    VideoHoverPreviewFrame pendingFrame;
                    const auto publish = [&](VideoHoverPreviewFrame&& frame, LONGLONG presentationTime) {
                        if (!waitForSourceTime(presentationTime)) return false;
                        auto* video = result ? result : new FilmstripHoverPreviewResult{};
                        video->request = request; video->result = S_OK; video->videoFrame = true; video->videoTimestamp = frame.timestamp; video->aspect = static_cast<float>(frame.width) / std::max(1u, frame.height);
                        video->width = frame.width; video->height = frame.height; video->stride = frame.stride; video->pixels = std::move(frame.pixels);
                        if (!PostMessageW(window_, kFilmstripHoverPreviewCompleteMessage, 0, reinterpret_cast<LPARAM>(video))) { delete video; return false; }
                        result = nullptr;
                        ++publishedFrames;
#ifdef _DEBUG
                        if (publishedFrames <= 3 || publishedFrames % 24 == 0) {
                            LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
                            const double sourceMs = static_cast<double>(frame.timestamp - timelineStart) / 10000.0;
                            const double wallMs = static_cast<double>(now.QuadPart - qpcStart.QuadPart) * 1000.0 / qpcFrequency.QuadPart;
                            wchar_t trace[256]{}; swprintf_s(trace, L"[Viewtrious] VIDEO_HOVER_FRAME_TIMING frame=%u source=%.1fms wall=%.1fms action=publish\\n", publishedFrames, sourceMs, wallMs); OutputDebugStringW(trace);
                        }
#endif
                        return true;
                    };
                    for (; SUCCEEDED(opened) && !filmstripHoverPreviewStopping_.load(std::memory_order_acquire);) {
                        VideoHoverPreviewFrame frame;
                        const HRESULT next = stream.ReadNext(frame);
                        if (next != S_OK) break;
                        ++decodedFrames;
                        lastSourceTimestamp = frame.timestamp;
                        if (!timelineStarted) {
                            timelineStarted = true;
                            timelineStart = frame.timestamp;
                            nextPresentationTime = timelineStart + kVideoHoverPresentationInterval;
                            QueryPerformanceCounter(&qpcStart);
#ifdef _DEBUG
                            wchar_t trace[512]{}; swprintf_s(trace, L"[Viewtrious] VIDEO_HOVER_CLOCK_START timestamp=%lld qpc=%lld path=%ls\\n", timelineStart, qpcStart.QuadPart, request.path.c_str()); OutputDebugStringW(trace);
#endif
                            if (!publish(std::move(frame), 0)) break;
                            continue;
                        }
                        const LONGLONG sourceTime = std::max<LONGLONG>(0, frame.timestamp - timelineStart);
                        if (stream.DurationSeconds() > 4.0 && sourceTime >= 3 * kVideoHoverHnsPerSecond) break;
                        if (frame.timestamp < nextPresentationTime) {
                            if (pendingFrame.pixels) ++droppedFrames;
                            pendingFrame = std::move(frame); // Keep only the newest source frame before the 24 fps presentation deadline.
                            continue;
                        }
                        if (pendingFrame.pixels) {
                            if (!publish(std::move(pendingFrame), nextPresentationTime - timelineStart)) break;
                            nextPresentationTime += kVideoHoverPresentationInterval;
                            if (frame.timestamp < nextPresentationTime) {
                                pendingFrame = std::move(frame);
                                continue;
                            }
                        }
                        if (!publish(std::move(frame), sourceTime)) break;
                        nextPresentationTime = timelineStart + sourceTime + kVideoHoverPresentationInterval;
                    }
                    if (pendingFrame.pixels && !filmstripHoverPreviewStopping_.load(std::memory_order_acquire) &&
                        videoHoverPreviewGeneration_.load(std::memory_order_acquire) == request.hoverGeneration)
                        publish(std::move(pendingFrame), nextPresentationTime - timelineStart);
                    stream.Close();
#ifdef _DEBUG
                    LARGE_INTEGER qpcEnd{}; QueryPerformanceCounter(&qpcEnd);
                    const double sourceElapsedMs = timelineStarted ? static_cast<double>(lastSourceTimestamp - timelineStart) / 10000.0 : 0.0;
                    const double wallElapsedMs = timelineStarted && qpcFrequency.QuadPart > 0 ? static_cast<double>(qpcEnd.QuadPart - qpcStart.QuadPart) * 1000.0 / qpcFrequency.QuadPart : 0.0;
                    wchar_t summary[320]{}; swprintf_s(summary, L"[Viewtrious] VIDEO_HOVER_SPEED_SUMMARY decoded=%u published=%u dropped=%u source=%.1fms wall=%.1fms\\n", decodedFrames, publishedFrames, droppedFrames, sourceElapsedMs, wallElapsedMs); OutputDebugStringW(summary);
                    OutputDebugStringW(L"[Viewtrious] VIDEO_HOVER_STREAM_RELEASED_BEFORE_FADE\\n");
#endif
                    auto* finished = new FilmstripHoverPreviewResult{};
                    finished->request = request; finished->result = S_OK; finished->videoFinished = true;
                    if (!PostMessageW(window_, kFilmstripHoverPreviewCompleteMessage, 0, reinterpret_cast<LPARAM>(finished))) delete finished;
                    if (result) delete result;
                    continue;
                }
#ifdef _DEBUG
                const ULONGLONG started = GetTickCount64();
                wchar_t begin[768]{};
                swprintf_s(begin, L"[Viewtrious] FILMSTRIP_HD_PREVIEW_JOB_BEGIN index-generation=%llu hover-generation=%llu path=%ls\n",
                    static_cast<unsigned long long>(request.itemGeneration), static_cast<unsigned long long>(request.hoverGeneration), request.path.c_str());
                OutputDebugStringW(begin);
#endif
                result->result = DecodeFilmstripHoverPreviewPixels(request.path, *result, result->aspect);
#ifdef _DEBUG
                wchar_t finished[768]{};
                swprintf_s(finished, L"[Viewtrious] FILMSTRIP_HD_PREVIEW_JOB_%ls elapsed=%llums size=%ux%u hr=0x%08X path=%ls\n",
                    SUCCEEDED(result->result) ? L"SUCCESS" : L"FAILED", static_cast<unsigned long long>(GetTickCount64() - started),
                    result->width, result->height, static_cast<unsigned int>(result->result), request.path.c_str());
                OutputDebugStringW(finished);
#endif
                if (filmstripHoverPreviewStopping_.load(std::memory_order_acquire)) delete result;
                else if (!PostMessageW(window_, kFilmstripHoverPreviewCompleteMessage, 0, reinterpret_cast<LPARAM>(result))) delete result;
            }
            if (SUCCEEDED(apartment)) CoUninitialize();
        });
    }
    void CancelQueuedFilmstripHoverPreviews() {
        std::lock_guard<std::mutex> lock(filmstripHoverPreviewMutex_);
        filmstripHoverPreviewQueue_.clear();
    }
    void StopFilmstripHoverPreviewWorker() {
        KillTimer(window_, kFilmstripHoverPreviewTimer);
        KillTimer(window_, kFilmstripHoverPreviewDwellTimer);
        KillTimer(window_, kFilmstripHoverPreviewFadeTimer);
        filmstripHoverPreviewStopping_.store(true, std::memory_order_release);
        CancelQueuedFilmstripHoverPreviews();
        filmstripHoverPreviewWake_.notify_all();
        if (filmstripHoverPreviewWorker_.joinable()) filmstripHoverPreviewWorker_.join();
        filmstripHoverPreviews_.clear();
        CancelFilmstripVideoHoverFade();
        MSG message{};
        while (PeekMessageW(&message, window_, kFilmstripHoverPreviewCompleteMessage, kFilmstripHoverPreviewCompleteMessage, PM_REMOVE))
            delete reinterpret_cast<FilmstripHoverPreviewResult*>(message.lParam);
    }
    void PruneFilmstripHoverPreviews() {
        constexpr size_t kMaximumEntries = 6;
        constexpr size_t kMaximumBytes = 24u * 1024u * 1024u;
        const auto bytes = [&] {
            size_t total = 0;
            for (const FilmstripHoverPreviewEntry& entry : filmstripHoverPreviews_) if (entry.pixels) total += entry.pixels->size();
            return total;
        };
        while ((filmstripHoverPreviews_.size() > kMaximumEntries || bytes() > kMaximumBytes) && !filmstripHoverPreviews_.empty()) {
            const auto oldest = std::min_element(filmstripHoverPreviews_.begin(), filmstripHoverPreviews_.end(), [](const auto& left, const auto& right) {
                return left.lastUse < right.lastUse;
            });
            filmstripHoverPreviews_.erase(oldest);
        }
    }
    void QueueFilmstripHoverPreview(size_t index) {
        if (shuttingDown_ || filmstripHoverPreviewStopping_.load(std::memory_order_acquire) || index >= navigationFiles_.size() ||
            index >= filmstripThumbnailGenerations_.size()) return;
        const std::wstring path = navigationFiles_[index].wstring();
        const uint64_t itemGeneration = filmstripThumbnailGenerations_[index];
        if (!IsVideoPath(path) && FindFilmstripHoverPreview(path, itemGeneration) >= 0) {
#ifdef _DEBUG
            OutputDebugStringW(L"[Viewtrious] FILMSTRIP_HD_PREVIEW_CACHE_HIT\n");
#endif
            return;
        }
        FilmstripHoverPreviewRequest request{ path, navigationFolderGeneration_, itemGeneration, filmstripHoverPreviewGeneration_ };
        {
            std::lock_guard<std::mutex> lock(filmstripHoverPreviewMutex_);
            filmstripHoverPreviewQueue_.clear();
            filmstripHoverPreviewQueue_.push_back(std::move(request));
        }
        StartFilmstripHoverPreviewWorker();
        filmstripHoverPreviewWake_.notify_one();
    }
    void BeginFilmstripHoverPreviewDecode() {
        KillTimer(window_, kFilmstripHoverPreviewDwellTimer);
        if (FilmstripHoverPreviewEligible(filmstripHoveredIndex_) && !filmstripDragging_ && !filmstripScrollAnimating_) {
#ifdef _DEBUG
            OutputDebugStringW(L"[Viewtrious] FILMSTRIP_HD_PREVIEW_DWELL_READY\n");
#endif
            QueueFilmstripHoverPreview(static_cast<size_t>(filmstripHoveredIndex_));
        }
    }
    ID2D1Bitmap* FilmstripHoverPreviewBitmap(size_t index) {
        if (!renderTarget_ || index >= navigationFiles_.size() || index >= filmstripThumbnailGenerations_.size()) return nullptr;
        const int cached = FindFilmstripHoverPreview(navigationFiles_[index].wstring(), filmstripThumbnailGenerations_[index]);
        if (cached < 0) return nullptr;
        FilmstripHoverPreviewEntry& entry = filmstripHoverPreviews_[cached];
        entry.lastUse = ++filmstripHoverPreviewUseSeed_;
        if (!entry.bitmap && entry.pixels) {
            const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), RenderTargetDpi(), RenderTargetDpi());
            if (FAILED(renderTarget_->CreateBitmap(D2D1::SizeU(entry.width, entry.height), entry.pixels->data(), entry.stride, properties, &entry.bitmap))) return nullptr;
        }
        return entry.bitmap.Get();
    }
    ID2D1Bitmap* FilmstripVideoHoverPreviewBitmap(size_t index) {
        if (!renderTarget_ || !filmstripVideoHoverPreview_ || index >= navigationFiles_.size() || index >= filmstripThumbnailGenerations_.size()) return nullptr;
        FilmstripHoverPreviewEntry& entry = *filmstripVideoHoverPreview_;
        if (entry.itemGeneration != filmstripThumbnailGenerations_[index] || !PathsEqual(fs::path(entry.path), navigationFiles_[index])) return nullptr;
        if (!entry.bitmap && entry.pixels) {
            const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), RenderTargetDpi(), RenderTargetDpi());
            if (FAILED(renderTarget_->CreateBitmap(D2D1::SizeU(entry.width, entry.height), entry.pixels->data(), entry.stride, properties, &entry.bitmap))) return nullptr;
#ifdef _DEBUG
            wchar_t trace[192]{}; swprintf_s(trace, L"[Viewtrious] VIDEO_HOVER_D2D_BITMAP_REPLACED timestamp=%lld alpha=%u\\n", filmstripVideoHoverTimestamp_, (*entry.pixels)[3]); OutputDebugStringW(trace);
#endif
        }
        return entry.bitmap.Get();
    }
    void CancelFilmstripVideoHoverFade(bool discardFrame = true) {
        KillTimer(window_, kFilmstripVideoHoverFadeTimer);
        filmstripVideoHoverFadeActive_ = false;
        if (discardFrame) filmstripVideoHoverPreview_.reset();
    }
    float FilmstripVideoHoverFadeProgress() const {
        if (!filmstripVideoHoverFadeActive_ || filmstripVideoHoverFadeQpcFrequency_ <= 0) return 0.0f;
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        const double elapsedMs = static_cast<double>(now.QuadPart - filmstripVideoHoverFadeStartQpc_) * 1000.0 / filmstripVideoHoverFadeQpcFrequency_;
        return static_cast<float>(std::clamp(elapsedMs / static_cast<double>(kFilmstripVideoHoverFadeDurationMs), 0.0, 1.0));
    }
    void StartFilmstripVideoHoverFade(size_t index) {
        if (!filmstripVideoHoverPreview_ || !FilmstripHoverPreviewEligible(static_cast<int>(index)) || !FilmstripThumbnailBitmap(index)) {
            CancelFilmstripVideoHoverFade();
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        LARGE_INTEGER frequency{}, now{};
        if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&now) || frequency.QuadPart <= 0) {
            CancelFilmstripVideoHoverFade();
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        filmstripVideoHoverFadeQpcFrequency_ = frequency.QuadPart;
        filmstripVideoHoverFadeStartQpc_ = now.QuadPart;
        filmstripVideoHoverFadeActive_ = true;
        SetTimer(window_, kFilmstripVideoHoverFadeTimer, 16, nullptr);
#ifdef _DEBUG
        OutputDebugStringW(L"[Viewtrious] VIDEO_HOVER_FADE_BEGIN duration=175ms source-released=1\n");
#endif
        InvalidateRect(window_, nullptr, FALSE);
    }

    void RemapFilmstripAspectMetadata(const std::vector<fs::path>& previousNavigationFiles) {
        std::vector<float> previousLayoutAspects = std::move(filmstripLayoutAspects_);
        std::vector<float> previousKnownAspects = std::move(filmstripKnownAspects_);
        std::vector<bool> previousAuthoritative = std::move(filmstripAspectAuthoritative_);
        std::vector<bool> previousRelayoutPending = std::move(filmstripAspectRelayoutPending_);
        filmstripLayoutAspects_.resize(navigationFiles_.size());
        filmstripKnownAspects_.resize(navigationFiles_.size());
        filmstripAspectAuthoritative_.assign(navigationFiles_.size(), false);
        filmstripAspectRelayoutPending_.assign(navigationFiles_.size(), false);
        for (size_t index = 0; index < navigationFiles_.size(); ++index) {
            const auto previous = std::find_if(previousNavigationFiles.begin(), previousNavigationFiles.end(), [&](const fs::path& path) {
                return PathsEqual(path, navigationFiles_[index]);
            });
            const size_t previousIndex = previous == previousNavigationFiles.end() ? previousNavigationFiles.size() :
                static_cast<size_t>(std::distance(previousNavigationFiles.begin(), previous));
            const bool reusable = previousIndex < previousLayoutAspects.size() && previousIndex < previousKnownAspects.size() &&
                previousIndex < previousAuthoritative.size() && previousIndex < previousRelayoutPending.size();
            const float placeholder = IsVideoPath(navigationFiles_[index].wstring()) ? 16.0f / 9.0f : 1.0f;
            if (!reusable) {
                filmstripLayoutAspects_[index] = placeholder;
                filmstripKnownAspects_[index] = placeholder;
                continue;
            }
            const float known = previousKnownAspects[previousIndex];
            const bool authoritative = previousAuthoritative[previousIndex] && std::isfinite(known) && known > 0.0f;
            filmstripLayoutAspects_[index] = authoritative ? known : previousLayoutAspects[previousIndex];
            filmstripKnownAspects_[index] = authoritative ? known : previousKnownAspects[previousIndex];
            filmstripAspectAuthoritative_[index] = authoritative;
            filmstripAspectRelayoutPending_[index] = !authoritative && previousRelayoutPending[previousIndex];
        }
    }
    void UpdateFilmstripVideoHoverFade() {
        if (!filmstripVideoHoverFadeActive_) { KillTimer(window_, kFilmstripVideoHoverFadeTimer); return; }
        if (!FilmstripVisible() || !FilmstripHoverPreviewEligible(filmstripPreviewIndex_) || filmstripDragging_ || filmstripScrollAnimating_) {
            CancelFilmstripVideoHoverFade();
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (FilmstripVideoHoverFadeProgress() >= 1.0f) {
            CancelFilmstripVideoHoverFade();
#ifdef _DEBUG
            OutputDebugStringW(L"[Viewtrious] VIDEO_HOVER_FADE_COMPLETE static-thumbnail-only=1\n");
#endif
        }
        InvalidateRect(window_, nullptr, FALSE);
    }
    void HandleFilmstripHoverPreviewResult(FilmstripHoverPreviewResult* result) {
        if (!result) return;
        const auto item = std::find_if(navigationFiles_.begin(), navigationFiles_.end(), [&](const fs::path& path) {
            return PathsEqual(path, fs::path(result->request.path));
        });
        const size_t index = item == navigationFiles_.end() ? navigationFiles_.size() : static_cast<size_t>(std::distance(navigationFiles_.begin(), item));
        const bool currentItem = result->request.folderGeneration == navigationFolderGeneration_ && index < filmstripThumbnailGenerations_.size() &&
            filmstripThumbnailGenerations_[index] == result->request.itemGeneration;
        const bool active = currentItem && result->request.hoverGeneration == filmstripHoverPreviewGeneration_ && FilmstripHoverPreviewEligible(filmstripPreviewIndex_) && filmstripPreviewIndex_ == static_cast<int>(index) &&
            !filmstripDragging_ && !filmstripScrollAnimating_;
        if (currentItem && result->videoFinished) {
            if (active) {
                filmstripVideoHoverLoading_ = false;
                StartFilmstripVideoHoverFade(index);
            }
            delete result; return;
        }
        if (result->videoFrame) {
            if (active && SUCCEEDED(result->result) && result->pixels && result->width && result->height) {
                FilmstripHoverPreviewEntry entry{};
                entry.path = result->request.path; entry.itemGeneration = result->request.itemGeneration; entry.aspect = result->aspect;
                entry.width = result->width; entry.height = result->height; entry.stride = result->stride; entry.pixels = std::move(result->pixels);
                filmstripVideoHoverTimestamp_ = result->videoTimestamp;
                filmstripVideoHoverPreview_ = std::move(entry);
                filmstripVideoHoverLoading_ = false;
                SetFilmstripHoverPreviewGeometry(index);
#ifdef _DEBUG
                wchar_t trace[192]{}; swprintf_s(trace, L"[Viewtrious] VIDEO_HOVER_UI_FRAME_ACCEPTED timestamp=%lld alpha=%u\\n", filmstripVideoHoverTimestamp_, (*filmstripVideoHoverPreview_->pixels)[3]); OutputDebugStringW(trace);
#endif
                InvalidateRect(window_, nullptr, FALSE);
#ifdef _DEBUG
                OutputDebugStringW(L"[Viewtrious] VIDEO_HOVER_REPAINT_REQUESTED\\n");
#endif
            }
            delete result; return;
        }
        if (currentItem && SUCCEEDED(result->result) && result->pixels && result->width && result->height) {
            filmstripHoverPreviews_.erase(std::remove_if(filmstripHoverPreviews_.begin(), filmstripHoverPreviews_.end(), [&](const FilmstripHoverPreviewEntry& entry) {
                return entry.itemGeneration == result->request.itemGeneration && PathsEqual(fs::path(entry.path), fs::path(result->request.path));
            }), filmstripHoverPreviews_.end());
            FilmstripHoverPreviewEntry entry{};
            entry.path = result->request.path;
            entry.itemGeneration = result->request.itemGeneration;
            entry.aspect = result->aspect;
            entry.lastUse = ++filmstripHoverPreviewUseSeed_;
            entry.width = result->width; entry.height = result->height; entry.stride = result->stride;
            entry.pixels = std::move(result->pixels);
            filmstripHoverPreviews_.push_back(std::move(entry));
            PruneFilmstripHoverPreviews();
            if (active) SetFilmstripHoverPreviewGeometry(index);
#ifdef _DEBUG
            OutputDebugStringW(active ? L"[Viewtrious] FILMSTRIP_HD_PREVIEW_PUBLISHED\n" : L"[Viewtrious] FILMSTRIP_HD_PREVIEW_JOB_STALE\n");
            if (active) OutputDebugStringW(L"[Viewtrious] FILMSTRIP_HD_PREVIEW_SWAP\n");
#endif
            if (active) InvalidateRect(window_, nullptr, FALSE);
        }
#ifdef _DEBUG
        else OutputDebugStringW(L"[Viewtrious] FILMSTRIP_HD_PREVIEW_JOB_STALE\n");
#endif
        delete result;
    }
    void QueueFilmstripThumbnails() {
        if (shuttingDown_ || filmstripThumbnailStopping_.load(std::memory_order_acquire) || !FilmstripEligible() ||
            navigationFiles_.empty() || filmstripThumbnailGenerations_.size() != navigationFiles_.size()) return;
        const auto [visibleFirst, visibleLast] = FilmstripVisibleRange();
        const size_t first = visibleFirst > 2 ? visibleFirst - 2 : 0;
        const size_t last = std::min(navigationFiles_.size(), visibleLast + 2);
        std::vector<size_t> requested;
        for (size_t index = first; index < last; ++index) requested.push_back(index);
        const size_t current = CurrentNavigationIndex();
        if (current < navigationFiles_.size() && std::find(requested.begin(), requested.end(), current) == requested.end()) requested.push_back(current);
        for (size_t index : requested) {
            const std::wstring path = navigationFiles_[index].wstring();
            const uint64_t itemGeneration = filmstripThumbnailGenerations_[index];
            if (FindFilmstripThumbnail(path, itemGeneration) >= 0 ||
                FilmstripThumbnailPending(path, itemGeneration) || FilmstripThumbnailFailed(path, itemGeneration)) continue;
            FilmstripThumbnailRequest request{};
            request.path = path;
            request.folderGeneration = navigationFolderGeneration_;
            request.itemGeneration = itemGeneration;
            request.targetHeight = static_cast<UINT>(FilmstripThumbnailHeight());
            filmstripThumbnailPending_.push_back(request);
            {
                std::lock_guard<std::mutex> lock(filmstripThumbnailMutex_);
                filmstripThumbnailQueue_.push_back(request);
            }
            StartFilmstripThumbnailWorker();
            filmstripThumbnailWake_.notify_one();
        }
        PruneFilmstripThumbnails();
    }
    void UpdateFilmstripThumbnailDemand(bool force = false) {
        if (!FilmstripEligible()) return;
        const auto [visibleFirst, visibleLast] = FilmstripVisibleRange();
        const size_t first = visibleFirst > 2 ? visibleFirst - 2 : 0;
        const size_t last = std::min(navigationFiles_.size(), visibleLast + 2);
        const size_t current = CurrentNavigationIndex();
        if (!force && filmstripDemandFirst_ == first && filmstripDemandLast_ == last && filmstripDemandCurrent_ == current) return;
        filmstripDemandFirst_ = first;
        filmstripDemandLast_ = last;
        filmstripDemandCurrent_ = current;
        QueueFilmstripThumbnails();
    }
    void PruneFilmstripThumbnails() {
        constexpr size_t budget = 64u * 1024u * 1024u;
        const auto bytes = [&] {
            size_t total = 0;
            for (const FilmstripThumbnailEntry& entry : filmstripThumbnails_) if (entry.pixels) total += entry.pixels->size();
            return total;
        };
        while (bytes() > budget && !filmstripThumbnails_.empty()) filmstripThumbnails_.erase(filmstripThumbnails_.begin());
    }
    void HandleFilmstripThumbnailResult(FilmstripThumbnailResult* result) {
        if (!result) return;
        filmstripThumbnailPending_.erase(std::remove_if(filmstripThumbnailPending_.begin(), filmstripThumbnailPending_.end(), [&](const FilmstripThumbnailRequest& request) {
            return request.itemGeneration == result->request.itemGeneration && PathsEqual(fs::path(request.path), fs::path(result->request.path));
        }), filmstripThumbnailPending_.end());
        const auto item = std::find_if(navigationFiles_.begin(), navigationFiles_.end(), [&](const fs::path& path) {
            return PathsEqual(path, fs::path(result->request.path));
        });
        const size_t index = item == navigationFiles_.end() ? navigationFiles_.size() : static_cast<size_t>(std::distance(navigationFiles_.begin(), item));
        const bool current = result->request.folderGeneration == navigationFolderGeneration_ && index < filmstripThumbnailGenerations_.size() &&
            filmstripThumbnailGenerations_[index] == result->request.itemGeneration;
        const bool succeeded = current && SUCCEEDED(result->result) && result->pixels && result->width && result->height;
        if (succeeded) {
            filmstripThumbnails_.erase(std::remove_if(filmstripThumbnails_.begin(), filmstripThumbnails_.end(), [&](const FilmstripThumbnailEntry& entry) {
                return PathsEqual(fs::path(entry.path), fs::path(result->request.path));
            }), filmstripThumbnails_.end());
            FilmstripThumbnailEntry entry{};
            entry.path = result->request.path;
            entry.itemGeneration = result->request.itemGeneration;
            entry.aspect = result->aspect;
            entry.width = result->width;
            entry.height = result->height;
            entry.stride = result->stride;
            entry.pixels = std::move(result->pixels);
            filmstripThumbnails_.push_back(std::move(entry));
            PruneFilmstripThumbnails();
            const bool aspectChanged = UpdateFilmstripKnownAspect(index, result->aspect);
            const bool layoutDeferred = aspectChanged && filmstripScrollAnimating_;
#ifdef _DEBUG
            if (IsVideoPath(result->request.path)) {
                wchar_t message[768]{};
                swprintf_s(message, L"[Viewtrious] VIDEO_SHELL_THUMB_PUBLISHED index=%zu size=%ux%u aspect=%.3f path=%ls\n",
                    index, result->width, result->height, result->aspect, result->request.path.c_str());
                OutputDebugStringW(message);
            }
            const int thumbnail = FindFilmstripThumbnail(result->request.path, result->request.itemGeneration);
            if (thumbnail >= 0) TraceFilmstripThumbnailPublication(index, filmstripThumbnails_[thumbnail], aspectChanged, layoutDeferred);
#endif
            if (layoutDeferred) filmstripLayoutRebuildPending_ = true;
            else if (aspectChanged) ApplyFilmstripAspectRelayout();
            InvalidateRect(window_, nullptr, FALSE);
        } else {
            if (current) filmstripThumbnailFailures_.push_back(result->request);
            QueueFilmstripThumbnails();
        }
        delete result;
    }
    ID2D1Bitmap* FilmstripThumbnailBitmap(size_t index) {
        if (!renderTarget_ || index >= navigationFiles_.size() || index >= filmstripThumbnailGenerations_.size()) return nullptr;
        const int thumbnail = FindFilmstripThumbnail(navigationFiles_[index].wstring(), filmstripThumbnailGenerations_[index]);
        if (thumbnail < 0) return nullptr;
        FilmstripThumbnailEntry& entry = filmstripThumbnails_[thumbnail];
        if (!entry.bitmap && entry.pixels) {
            const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), RenderTargetDpi(), RenderTargetDpi());
            renderTarget_->CreateBitmap(D2D1::SizeU(entry.width, entry.height), entry.pixels->data(), entry.stride, properties, &entry.bitmap);
        }
        return entry.bitmap.Get();
    }
    void EnsureFilmstripItemVisible(size_t index) {
        if (!FilmstripEligible() || filmstripItemOffsets_.size() < 2 || index >= navigationFiles_.size() ||
            index >= filmstripItemWidths_.size() || index + 1 >= filmstripItemOffsets_.size()) return;
        const RECT bounds = GetFilmstripBounds();
        const double left = filmstripItemOffsets_[index];
        const double right = left + FilmstripThumbnailWidth(index);
        const double visibleRight = filmstripScroll_ + static_cast<double>(bounds.right - bounds.left);
        if (left < filmstripScroll_) filmstripScroll_ = left;
        else if (right > visibleRight) filmstripScroll_ = right - static_cast<double>(bounds.right - bounds.left);
        filmstripScroll_ = std::clamp(filmstripScroll_, 0.0, static_cast<double>(FilmstripMaximumScroll()));
        StopFilmstripScrollAnimation();
    }
    RECT GetFilmstripThumbnailBounds(size_t index) const {
        const RECT strip = GetFilmstripBounds();
        const int left = strip.left + static_cast<int>(std::lround(static_cast<double>(filmstripItemOffsets_[index]) - filmstripScroll_));
        const int height = FilmstripThumbnailHeight();
        const int top = strip.top + (strip.bottom - strip.top - height) / 2;
        return { left, top, left + static_cast<int>(std::lround(FilmstripThumbnailWidth(index))), top + height };
    }
    int FilmstripItemAt(POINT point) const {
        if (!FilmstripContains(point)) return -1;
        const auto [first, last] = FilmstripVisibleRange();
        for (size_t index = first; index < last; ++index) {
            RECT hit = GetFilmstripThumbnailBounds(index);
            InflateRect(&hit, MulDiv(5, GetDpiForWindow(window_), 96), MulDiv(4, GetDpiForWindow(window_), 96));
            if (PtInRect(&hit, point)) return static_cast<int>(index);
        }
        return -1;
    }
    double FilmstripScrollRefreshPeriodQpc() const {
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return 0.0;
        MONITORINFOEXW monitor{};
        monitor.cbSize = sizeof(monitor);
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        const HMONITOR handle = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        const DWORD refreshHz = GetMonitorInfoW(handle, &monitor) &&
            EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
            mode.dmDisplayFrequency >= 30 && mode.dmDisplayFrequency <= 360 ? mode.dmDisplayFrequency : 120;
        return static_cast<double>(frequency.QuadPart) / static_cast<double>(refreshHz);
    }
    bool EnsureFilmstripScrollScheduler() {
        if (filmstripScrollTimer_) return true;
        filmstripScrollStopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        filmstripScrollTimer_ = CreateWaitableTimerExW(nullptr, nullptr, kHighResolutionWaitableTimerFlag, TIMER_ALL_ACCESS);
        if (!filmstripScrollStopEvent_ || !filmstripScrollTimer_) {
            if (filmstripScrollTimer_) CloseHandle(filmstripScrollTimer_);
            if (filmstripScrollStopEvent_) CloseHandle(filmstripScrollStopEvent_);
            filmstripScrollTimer_ = filmstripScrollStopEvent_ = nullptr;
            return false;
        }
        try {
            filmstripScrollSchedulerThread_ = std::thread([this] {
                HANDLE handles[] = { filmstripScrollStopEvent_, filmstripScrollTimer_ };
                for (;;) {
                    const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, INFINITE);
                    if (wait != WAIT_OBJECT_0 + 1) return;
                    const uint64_t generation = filmstripScrollGeneration_.load(std::memory_order_acquire);
                    uint64_t none = 0;
                    if (filmstripScrollWakePendingGeneration_.compare_exchange_strong(none, generation, std::memory_order_acq_rel))
                        PostMessageW(window_, kFilmstripScrollWakeMessage, static_cast<WPARAM>(generation), 0);
                }
            });
        } catch (const std::system_error&) {
            CloseHandle(filmstripScrollTimer_);
            CloseHandle(filmstripScrollStopEvent_);
            filmstripScrollTimer_ = filmstripScrollStopEvent_ = nullptr;
            return false;
        }
        return true;
    }
    bool ArmFilmstripScrollWake() {
        if (!filmstripScrollTimer_ || !filmstripScrollAnimating_) return false;
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return false;
        const double periodQpc = FilmstripScrollRefreshPeriodQpc();
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(std::llround(periodQpc * 10000000.0 / static_cast<double>(frequency.QuadPart))));
        return SetWaitableTimer(filmstripScrollTimer_, &due, 0, nullptr, nullptr, FALSE) != FALSE;
    }
    void StopFilmstripScrollAnimation() {
        filmstripScrollVelocity_ = 0.0;
        filmstripScrollAnimating_ = false;
        ++filmstripScrollGeneration_;
        filmstripScrollWakePendingGeneration_.store(0, std::memory_order_release);
        if (filmstripScrollTimer_) CancelWaitableTimer(filmstripScrollTimer_);
    }
    void StopFilmstripScrollScheduler() {
        StopFilmstripScrollAnimation();
        if (filmstripScrollStopEvent_) SetEvent(filmstripScrollStopEvent_);
        if (filmstripScrollSchedulerThread_.joinable()) filmstripScrollSchedulerThread_.join();
        if (filmstripScrollTimer_) CloseHandle(filmstripScrollTimer_);
        if (filmstripScrollStopEvent_) CloseHandle(filmstripScrollStopEvent_);
        filmstripScrollTimer_ = filmstripScrollStopEvent_ = nullptr;
    }
    void ResumeFilmstripHoverAtCursor() {
        POINT point{};
        if (!GetCursorPos(&point)) return;
        ScreenToClient(window_, &point);
        filmstripHoveredIndex_ = -1;
        SetFilmstripHover(point);
    }
    bool AdvanceFilmstripScroll(LONGLONG nowQpc) {
        if (!filmstripScrollAnimating_ || filmstripScrollQpcFrequency_ <= 0) return false;
        const double dt = std::clamp(static_cast<double>(nowQpc - filmstripScrollLastQpc_) / static_cast<double>(filmstripScrollQpcFrequency_), 0.0, 0.050);
        filmstripScrollLastQpc_ = nowQpc;
        if (dt <= 0.0) return std::abs(filmstripScrollVelocity_) > kFilmstripVelocityStopEpsilon;
#ifdef _DEBUG
        const double previousPosition = filmstripScroll_;
        const double previousVelocity = filmstripScrollVelocity_;
#endif
        filmstripScroll_ += filmstripScrollVelocity_ * dt;
        const double maximum = FilmstripMaximumScroll();
        bool hitBound = false;
        if (filmstripScroll_ <= 0.0) {
            filmstripScroll_ = 0.0;
            if (filmstripScrollVelocity_ < 0.0) { filmstripScrollVelocity_ = 0.0; hitBound = true; }
        } else if (filmstripScroll_ >= maximum) {
            filmstripScroll_ = maximum;
            if (filmstripScrollVelocity_ > 0.0) { filmstripScrollVelocity_ = 0.0; hitBound = true; }
        }
        if (!hitBound) filmstripScrollVelocity_ *= std::exp(-kFilmstripVelocityDampingPerSecond * dt);
#ifdef _DEBUG
        filmstripScrollTickCount_++;
        filmstripScrollTickTotalMs_ += dt * 1000.0;
        filmstripScrollTickMinimumMs_ = std::min(filmstripScrollTickMinimumMs_, dt * 1000.0);
        filmstripScrollTickMaximumMs_ = std::max(filmstripScrollTickMaximumMs_, dt * 1000.0);
        if (hitBound) OutputDebugStringW(L"Viewtrious filmstrip scroll: bound collision\n");
        wchar_t message[320]{};
        swprintf_s(message, L"Viewtrious filmstrip scroll: qpc=%lld dt=%.3fms position=%.3f->%.3f delta=%.3f velocity=%.3f->%.3f bound=%d reversed=%d\n",
            nowQpc, dt * 1000.0, previousPosition, filmstripScroll_, filmstripScroll_ - previousPosition,
            previousVelocity, filmstripScrollVelocity_, hitBound ? 1 : 0,
            previousVelocity * filmstripScrollVelocity_ < 0.0 ? 1 : 0);
        OutputDebugStringW(message);
#endif
        if (std::abs(filmstripScrollVelocity_) <= kFilmstripVelocityStopEpsilon) {
#ifdef _DEBUG
            filmstripPostStopPosition_ = filmstripScroll_;
            filmstripPostStopPaintCount_ = 0;
            wchar_t stopMessage[256]{};
            swprintf_s(stopMessage, L"Viewtrious filmstrip scroll: stop position=%.3f velocity=%.3f->0.000 animating=1->0 timer=1\n",
                filmstripScroll_, filmstripScrollVelocity_);
            OutputDebugStringW(stopMessage);
#endif
            filmstripScrollVelocity_ = 0.0;
#ifdef _DEBUG
            wchar_t message[256]{};
            const double average = filmstripScrollTickCount_ ? filmstripScrollTickTotalMs_ / filmstripScrollTickCount_ : 0.0;
            swprintf_s(message, L"Viewtrious filmstrip scroll: damping stop ticks=%u avg=%.2fms min=%.2fms max=%.2fms\n", filmstripScrollTickCount_, average, filmstripScrollTickMinimumMs_, filmstripScrollTickMaximumMs_);
            OutputDebugStringW(message);
#endif
            return false;
        }
        return true;
    }
    void ScrollFilmstrip(int rawWheelDelta) {
        if (!FilmstripVisible() || rawWheelDelta == 0) return;
        HideFilmstripHoverPreviewImmediately();
        SetFilmstripHover({ -1, -1 });
        if (!filmstripScrollAnimating_) ApplyDeferredFilmstripLayout();
#ifdef _DEBUG
        filmstripPostStopPosition_.reset();
#endif
        LARGE_INTEGER now{}, frequency{};
        if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return;
        if (filmstripScrollAnimating_) AdvanceFilmstripScroll(now.QuadPart);
        const double units = -static_cast<double>(rawWheelDelta) / static_cast<double>(WHEEL_DELTA);
        const double scale = static_cast<double>(GetDpiForWindow(window_)) / 96.0;
#ifdef _DEBUG
        const double before = filmstripScrollVelocity_;
#endif
        filmstripScrollVelocity_ = std::clamp(filmstripScrollVelocity_ + units * kFilmstripWheelImpulseDipsPerSecond * scale,
            -kFilmstripMaximumVelocityDipsPerSecond * scale, kFilmstripMaximumVelocityDipsPerSecond * scale);
        if (!filmstripScrollAnimating_) {
            filmstripScrollQpcFrequency_ = frequency.QuadPart;
            filmstripScrollLastQpc_ = now.QuadPart;
            ++filmstripScrollGeneration_;
            filmstripScrollWakePendingGeneration_.store(0, std::memory_order_release);
            filmstripScrollAnimating_ = true;
#ifdef _DEBUG
            filmstripScrollTickCount_ = 0;
            filmstripScrollTickTotalMs_ = 0.0;
            filmstripScrollTickMinimumMs_ = std::numeric_limits<double>::infinity();
            filmstripScrollTickMaximumMs_ = 0.0;
#endif
        }
#ifdef _DEBUG
        wchar_t message[256]{};
        swprintf_s(message, L"Viewtrious filmstrip scroll: qpc=%lld raw=%d units=%.3f position=%.3f velocity=%.1f->%.1f impulse=%.1f\n",
            now.QuadPart, rawWheelDelta, units, filmstripScroll_, before, filmstripScrollVelocity_, filmstripScrollVelocity_ - before);
        OutputDebugStringW(message);
#endif
        if (!EnsureFilmstripScrollScheduler() || !ArmFilmstripScrollWake()) {
            StopFilmstripScrollAnimation();
            ApplyDeferredFilmstripLayout();
            ResumeFilmstripHoverAtCursor();
        }
        QueueFilmstripThumbnails();
        StartFilmstripHold();
        InvalidateRect(window_, nullptr, FALSE);
    }
    void FilmstripScrollWakeMessage(uint64_t generation) {
        uint64_t expected = generation;
        filmstripScrollWakePendingGeneration_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
        if (generation != filmstripScrollGeneration_.load(std::memory_order_acquire) || !filmstripScrollAnimating_) return;
        if (!FilmstripEligible()) {
            StopFilmstripScrollAnimation();
            ApplyDeferredFilmstripLayout();
            return;
        }
        LARGE_INTEGER now{};
        if (!QueryPerformanceCounter(&now) || !AdvanceFilmstripScroll(now.QuadPart)) {
            StopFilmstripScrollAnimation();
            ApplyDeferredFilmstripLayout();
            ResumeFilmstripHoverAtCursor();
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        QueueFilmstripThumbnails();
        InvalidateRect(window_, nullptr, FALSE);
        if (!ArmFilmstripScrollWake()) {
            StopFilmstripScrollAnimation();
            ApplyDeferredFilmstripLayout();
            ResumeFilmstripHoverAtCursor();
        }
    }
    RECT GetFilmstripRevealBounds() const {
        RECT client{};
        GetClientRect(window_, &client);
        const int top = std::max(fullscreen_ ? 0 : GetFrameMetrics(window_).titleBarHeight,
            static_cast<int>(client.bottom) - MulDiv(60, GetDpiForWindow(window_), 96));
        const RECT previous = GetCanvasNavigationZoneBounds(false);
        const RECT next = GetCanvasNavigationZoneBounds(true);
        return { previous.right, top, next.left, client.bottom };
    }
    bool FilmstripRevealContains(POINT point) const { const RECT bounds = GetFilmstripRevealBounds(); return FilmstripEligible() && PtInRect(&bounds, point); }
    bool BeginFilmstripInteraction(POINT point) {
        if (!FilmstripContains(point)) return false;
        filmstripDragCandidate_ = true;
        filmstripDragStart_ = point;
        filmstripDragStartScroll_ = filmstripScroll_;
        filmstripDragItem_ = FilmstripItemAt(point);
        StartFilmstripHold(UINT_MAX);
        return true;
    }
    bool ContinueFilmstripInteraction(POINT point) {
        if (!filmstripDragCandidate_) return false;
        const int dx = point.x - filmstripDragStart_.x;
        const int dy = point.y - filmstripDragStart_.y;
        if (!filmstripDragging_ && (std::abs(dx) >= GetSystemMetrics(SM_CXDRAG) || std::abs(dy) >= GetSystemMetrics(SM_CYDRAG))) {
            filmstripDragging_ = true;
            HideFilmstripHoverPreviewImmediately();
            SetFilmstripHover({ -1, -1 });
            StopFilmstripScrollAnimation();
            filmstripDragStart_ = point;
            filmstripDragStartScroll_ = filmstripScroll_;
        }
        if (!filmstripDragging_) return true;
        filmstripScroll_ = std::clamp(filmstripDragStartScroll_ - static_cast<double>(dx), 0.0, static_cast<double>(FilmstripMaximumScroll()));
        UpdateFilmstripThumbnailDemand();
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }
    bool EndFilmstripInteraction(POINT point) {
        const bool dragging = filmstripDragging_;
        const int click = filmstripDragItem_;
        filmstripDragCandidate_ = filmstripDragging_ = false;
        filmstripDragItem_ = -1;
        if (dragging) {
            UpdateFilmstripThumbnailDemand(true);
            SetFilmstripPointerState(point);
            filmstripHoveredIndex_ = -1;
            SetFilmstripHover(point);
            return true;
        }
        if (click >= 0 && FilmstripContains(point)) SelectFilmstripItem(click);
        return true;
    }
    void CancelFilmstripInteraction() {
        filmstripDragCandidate_ = filmstripDragging_ = false;
        filmstripDragItem_ = -1;
    }
    bool FilmstripInteractionActive() const { return filmstripDragCandidate_; }
    void StopFilmstripVisibilityTimer() { KillTimer(window_, kFilmstripVisibilityTimer); }
    void StartFilmstripHold(UINT holdDurationMs = 2000) {
        if (!FilmstripEligible()) return;
        filmstripOpacity_ = 1.0f;
        filmstripVisibilityState_ = FilmstripVisibilityState::Holding;
        filmstripHoldDurationMs_ = holdDurationMs;
        filmstripVisibilityStart_ = GetTickCount64();
        if (alwaysShowFilmstrip_) StopFilmstripVisibilityTimer();
        else SetTimer(window_, kFilmstripVisibilityTimer, animationsEnabled_ ? 16 : 50, nullptr);
        InvalidateRect(window_, nullptr, FALSE);
    }
    void StartFilmstripReveal() {
        if (!FilmstripEligible()) return;
        if (!animationsEnabled_) { StartFilmstripHold(); return; }
        filmstripRevealStartOpacity_ = filmstripOpacity_;
        filmstripVisibilityState_ = FilmstripVisibilityState::Revealing;
        filmstripVisibilityStart_ = GetTickCount64();
        SetTimer(window_, kFilmstripVisibilityTimer, 16, nullptr);
        InvalidateRect(window_, nullptr, FALSE);
    }
    void BeginFilmstripFadeSequence() {
        if (filmstripOpacity_ <= 0.001f || filmstripVisibilityState_ == FilmstripVisibilityState::Revealing || alwaysShowFilmstrip_) return;
        filmstripVisibilityState_ = FilmstripVisibilityState::Holding;
        filmstripHoldDurationMs_ = 2000;
        filmstripVisibilityStart_ = GetTickCount64();
        SetTimer(window_, kFilmstripVisibilityTimer, animationsEnabled_ ? 16 : 50, nullptr);
    }
    void SetFilmstripPointerState(POINT point) {
        const bool panel = FilmstripContains(point);
        const bool reveal = !dragging_ && FilmstripRevealContains(point);
        const bool hint = false;
        const bool wasHeld = filmstripPanelHovered_ || filmstripRevealHovered_ || filmstripHintHovered_;
        filmstripPanelHovered_ = panel;
        filmstripRevealHovered_ = reveal;
        filmstripHintHovered_ = hint;
        if ((panel || reveal || hint) && !wasHeld) StartFilmstripReveal();
        else if (wasHeld) BeginFilmstripFadeSequence();
    }
    void UpdateFilmstripVisibility() {
        if (!FilmstripEligible()) { CancelFilmstripVideoHoverFade(); HideFilmstripHoverPreviewImmediately(); filmstripOpacity_ = 0.0f; filmstripVisibilityState_ = FilmstripVisibilityState::Hidden; StopFilmstripVisibilityTimer(); return; }
        if (alwaysShowFilmstrip_) { filmstripOpacity_ = 1.0f; filmstripVisibilityState_ = FilmstripVisibilityState::Holding; StopFilmstripVisibilityTimer(); return; }
        const bool held = filmstripPanelHovered_ || filmstripRevealHovered_ || filmstripHintHovered_;
        const ULONGLONG elapsed = GetTickCount64() - filmstripVisibilityStart_;
        float opacity = filmstripOpacity_;
        if (filmstripVisibilityState_ == FilmstripVisibilityState::Revealing) {
            opacity = animationsEnabled_ ? filmstripRevealStartOpacity_ + (1.0f - filmstripRevealStartOpacity_) * std::min(1.0f, static_cast<float>(elapsed) / 500.0f) : 1.0f;
            if (!animationsEnabled_ || elapsed >= 500) { filmstripVisibilityState_ = FilmstripVisibilityState::Holding; filmstripVisibilityStart_ = GetTickCount64(); if (held) StopFilmstripVisibilityTimer(); }
        } else if (held) {
            StopFilmstripVisibilityTimer();
            return;
        } else if (filmstripVisibilityState_ == FilmstripVisibilityState::Holding && elapsed >= filmstripHoldDurationMs_) {
            if (!animationsEnabled_) opacity = 0.0f;
            else { filmstripVisibilityState_ = FilmstripVisibilityState::Fading; filmstripVisibilityStart_ = GetTickCount64(); }
        } else if (filmstripVisibilityState_ == FilmstripVisibilityState::Fading) {
            opacity = animationsEnabled_ ? std::max(0.0f, 1.0f - static_cast<float>(elapsed) / 1000.0f) : 0.0f;
        }
        if (std::abs(opacity - filmstripOpacity_) > 0.001f) { filmstripOpacity_ = opacity; InvalidateRect(window_, nullptr, FALSE); }
        if (opacity <= 0.001f) { filmstripVisibilityState_ = FilmstripVisibilityState::Hidden; StopFilmstripVisibilityTimer(); }
    }
    void RevealClickedFilmstripItem() {
        if (filmstripClickedRevealTarget_) {
            const size_t target = *filmstripClickedRevealTarget_;
            filmstripClickedRevealTarget_.reset();
            EnsureFilmstripItemVisible(target);
            StartFilmstripHold(1000);
        }
    }
    void SelectFilmstripItem(int index) {
        if (index < 0 || index >= static_cast<int>(navigationFiles_.size())) return;
#ifdef _DEBUG
        filmstripPostStopPosition_.reset();
#endif
        StopFilmstripScrollAnimation();
        filmstripClickedRevealTarget_ = static_cast<size_t>(index);
        const std::wstring path = navigationFiles_[index].wstring();
        if (!PathsEqual(fs::path(path), fs::path(currentPath_))) {
            if (FAILED(LoadContent(path, false))) filmstripClickedRevealTarget_.reset();
            else if (!imageDecodePending_) RevealClickedFilmstripItem();
        }
        else RevealClickedFilmstripItem();
    }
    void HideFilmstripHoverPreviewImmediately() {
        KillTimer(window_, kFilmstripHoverPreviewFadeTimer);
        filmstripHoverPreviewFadeActive_ = false;
        filmstripHoverPreviewFadeOut_ = false;
        filmstripHoverPreviewOpacity_ = 0.0f;
        filmstripVideoHoverLoading_ = false;
        filmstripPreviewIndex_ = -1;
        filmstripPreviewGeometryValid_ = false;
    }
    void SetFilmstripHover(POINT point) {
        const int index = FilmstripItemAt(point);
        if (filmstripHoveredIndex_ == index) return;
#ifdef _DEBUG
        wchar_t message[512]{};
        swprintf_s(message, L"[Viewtrious] FILMSTRIP_HOVER_CANDIDATE_%ls point=%ld,%ld index=%d dragging=%d wheel=%d visible=%d\n",
            index >= 0 ? L"SET" : L"CLEAR", point.x, point.y, index, filmstripDragging_ ? 1 : 0, filmstripScrollAnimating_ ? 1 : 0, FilmstripVisible() ? 1 : 0);
        OutputDebugStringW(message);
#endif
        ++filmstripHoverPreviewGeneration_;
        videoHoverPreviewGeneration_.store(filmstripHoverPreviewGeneration_, std::memory_order_release);
        CancelFilmstripVideoHoverFade();
        KillTimer(window_, kFilmstripHoverPreviewTimer);
        KillTimer(window_, kFilmstripHoverPreviewDwellTimer);
        CancelQueuedFilmstripHoverPreviews();
        filmstripHoveredIndex_ = index;
        if (filmstripPreviewIndex_ >= 0) StartFilmstripHoverPreviewFadeOut();
        if (FilmstripHoverPreviewEligible(index) && !filmstripDragging_ && !filmstripScrollAnimating_) {
            SetTimer(window_, kFilmstripHoverPreviewDwellTimer, 100, nullptr);
            const UINT_PTR timer = SetTimer(window_, kFilmstripHoverPreviewTimer, filmstripHoverPreviewDelayMs_, nullptr);
            (void)timer;
#ifdef _DEBUG
            wchar_t timerMessage[256]{};
            swprintf_s(timerMessage, L"[Viewtrious] FILMSTRIP_HOVER_SETTIMER hwnd=%p requested=%zu returned=%zu delay=%u error=%lu\n",
                window_, static_cast<size_t>(kFilmstripHoverPreviewTimer), static_cast<size_t>(timer), filmstripHoverPreviewDelayMs_, timer ? ERROR_SUCCESS : GetLastError());
            OutputDebugStringW(timerMessage);
#endif
        }
        InvalidateRect(window_, nullptr, FALSE);
    }
    bool FilmstripHoverPreviewEligible(int index) const {
        return index >= 0 && index < static_cast<int>(navigationFiles_.size()) && !currentPath_.empty() &&
            !PathsEqual(navigationFiles_[static_cast<size_t>(index)], fs::path(currentPath_));
    }
    void SuppressFilmstripHoverPreviewForCurrentMedia() {
        if (FilmstripHoverPreviewEligible(filmstripHoveredIndex_) && (filmstripPreviewIndex_ < 0 || FilmstripHoverPreviewEligible(filmstripPreviewIndex_))) return;
        ++filmstripHoverPreviewGeneration_;
        videoHoverPreviewGeneration_.store(filmstripHoverPreviewGeneration_, std::memory_order_release);
        CancelFilmstripVideoHoverFade();
        KillTimer(window_, kFilmstripHoverPreviewTimer);
        KillTimer(window_, kFilmstripHoverPreviewDwellTimer);
        CancelQueuedFilmstripHoverPreviews();
        filmstripPreviewIndex_ = -1;
        filmstripPreviewGeometryValid_ = false;
        filmstripVideoHoverLoading_ = false;
        filmstripHoverPreviewOpacity_ = 0.0f;
        filmstripHoverPreviewFadeActive_ = false;
        KillTimer(window_, kFilmstripHoverPreviewFadeTimer);
        InvalidateRect(window_, nullptr, FALSE);
    }
    float FilmstripHoverPreviewAspect(size_t index) const {
        if (index >= navigationFiles_.size() || index >= filmstripThumbnailGenerations_.size()) return 1.0f;
        if (filmstripVideoHoverPreview_ && filmstripVideoHoverPreview_->itemGeneration == filmstripThumbnailGenerations_[index] &&
            PathsEqual(fs::path(filmstripVideoHoverPreview_->path), navigationFiles_[index]) && filmstripVideoHoverPreview_->aspect > 0.0f)
            return filmstripVideoHoverPreview_->aspect;
        const int cached = FindFilmstripHoverPreview(navigationFiles_[index].wstring(), filmstripThumbnailGenerations_[index]);
        if (cached >= 0 && filmstripHoverPreviews_[cached].aspect > 0.0f) return filmstripHoverPreviews_[cached].aspect;
        if (index < filmstripKnownAspects_.size() && filmstripKnownAspects_[index] > 0.0f) return filmstripKnownAspects_[index];
        return static_cast<float>(FilmstripThumbnailWidth(index)) / std::max(1, FilmstripThumbnailHeight());
    }
    void SetFilmstripHoverPreviewGeometry(size_t index) {
        const RECT strip = GetFilmstripBounds();
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        const float inset = static_cast<float>(FilmstripPadding());
        const float aspect = std::max(0.01f, FilmstripHoverPreviewAspect(index));
        float height = std::min(320.0f * scale, std::max(1.0f, static_cast<float>(strip.top) - 24.0f * scale - inset));
        float width = height * aspect;
        const float sideClearance = std::max(8.0f * scale, 24.0f * scale);
        const float maximumWidth = std::max(1.0f, static_cast<float>(strip.right - strip.left) - sideClearance * 2.0f - inset * 2.0f);
        if (width > maximumWidth) { width = maximumWidth; height = width / aspect; }
        const RECT hovered = GetFilmstripThumbnailBounds(index);
        const float shellWidth = width + inset * 2.0f;
        const float shellLeft = std::clamp((hovered.left + hovered.right - shellWidth) * 0.5f,
            static_cast<float>(strip.left) + sideClearance, std::max(static_cast<float>(strip.left) + sideClearance,
                static_cast<float>(strip.right) - sideClearance - shellWidth));
        const float bottom = static_cast<float>(strip.top) - inset;
        filmstripPreviewGeometry_ = D2D1::RectF(shellLeft + inset, bottom - height, shellLeft + inset + width, bottom);
        filmstripPreviewGeometryValid_ = true;
    }
    void ShowFilmstripHoverPreview() {
        KillTimer(window_, kFilmstripHoverPreviewTimer);
#ifdef _DEBUG
        wchar_t message[512]{};
        swprintf_s(message, L"[Viewtrious] FILMSTRIP_HOVER_TIMER_FIRE candidate=%d dragging=%d wheel=%d\n",
            filmstripHoveredIndex_, filmstripDragging_ ? 1 : 0, filmstripScrollAnimating_ ? 1 : 0);
        OutputDebugStringW(message);
#endif
        if (FilmstripHoverPreviewEligible(filmstripHoveredIndex_) && !filmstripDragging_ && !filmstripScrollAnimating_) {
            filmstripPreviewIndex_ = filmstripHoveredIndex_;
            SetFilmstripHoverPreviewGeometry(static_cast<size_t>(filmstripPreviewIndex_));
            filmstripVideoHoverLoading_ = IsVideoPath(navigationFiles_[static_cast<size_t>(filmstripPreviewIndex_)].wstring());
            filmstripHoverPreviewFadeActive_ = true;
            filmstripHoverPreviewFadeOut_ = false;
            filmstripHoverPreviewFadeStartOpacity_ = 0.0f;
            filmstripHoverPreviewFadeStartedAtMs_ = GetTickCount64();
            filmstripHoverPreviewOpacity_ = 0.0f;
            SetTimer(window_, kFilmstripHoverPreviewFadeTimer, 16, nullptr);
#ifdef _DEBUG
            OutputDebugStringW(L"[Viewtrious] FILMSTRIP_HOVER_PREVIEW_ACTIVATE invalidate=1\n");
#endif
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
    void StartFilmstripHoverPreviewFadeOut() {
        if (filmstripPreviewIndex_ < 0) return;
        filmstripHoverPreviewFadeActive_ = true;
        filmstripHoverPreviewFadeOut_ = true;
        filmstripHoverPreviewFadeStartOpacity_ = filmstripHoverPreviewOpacity_;
        filmstripHoverPreviewFadeStartedAtMs_ = GetTickCount64();
        filmstripVideoHoverLoading_ = false;
        SetTimer(window_, kFilmstripHoverPreviewFadeTimer, 16, nullptr);
    }
    void UpdateFilmstripHoverPreviewFade() {
        if (!filmstripHoverPreviewFadeActive_) { KillTimer(window_, kFilmstripHoverPreviewFadeTimer); return; }
        const float progress = std::min(1.0f, static_cast<float>(GetTickCount64() - filmstripHoverPreviewFadeStartedAtMs_) / kFilmstripHoverPreviewFadeDurationMs);
        const float eased = SmoothTransitionProgress(progress);
        filmstripHoverPreviewOpacity_ = filmstripHoverPreviewFadeOut_ ? filmstripHoverPreviewFadeStartOpacity_ * (1.0f - eased) : eased;
        if (progress >= 1.0f) {
            filmstripHoverPreviewFadeActive_ = false;
            KillTimer(window_, kFilmstripHoverPreviewFadeTimer);
            if (filmstripHoverPreviewFadeOut_) { filmstripPreviewIndex_ = -1; filmstripPreviewGeometryValid_ = false; }
        }
        InvalidateRect(window_, nullptr, FALSE);
    }
    D2D1_RECT_F FitFilmstripHoverPreviewBitmap(const D2D1_RECT_F& bounds, const D2D1_SIZE_F& size) const {
        const float aspect = size.width / std::max(1.0f, size.height);
        const float boundsAspect = (bounds.right - bounds.left) / std::max(1.0f, bounds.bottom - bounds.top);
        if (aspect > boundsAspect) {
            const float height = (bounds.right - bounds.left) / aspect;
            const float top = (bounds.top + bounds.bottom - height) * 0.5f;
            return D2D1::RectF(bounds.left, top, bounds.right, top + height);
        }
        const float width = (bounds.bottom - bounds.top) * aspect;
        const float left = (bounds.left + bounds.right - width) * 0.5f;
        return D2D1::RectF(left, bounds.top, left + width, bounds.bottom);
    }
    bool DrawFilmstripHoverPreviewShell(const D2D1_RECT_F& panel, ID2D1Brush* panelBorder, ID2D1Brush* shellSurface,
        ID2D1Brush* shellBorder, float scale) {
        if (!d2dFactory_ || !filmstripPreviewGeometryValid_) return false;
        const float inset = static_cast<float>(FilmstripPadding());
        const float shellLeft = filmstripPreviewGeometry_.left - inset;
        const float shellRight = filmstripPreviewGeometry_.right + inset;
        const float shellTop = filmstripPreviewGeometry_.top - inset;
        const float panelRadius = 12.0f * scale;
        const float shellRadius = std::min({ panelRadius, (shellRight - shellLeft) * 0.5f,
            (static_cast<float>(GetFilmstripBounds().top) - shellTop) * 0.5f });
        const float joinRadius = std::min(panelRadius, (shellRight - shellLeft) * 0.5f);
        const float curve = 0.55228475f;
        const float panelTop = panel.top;
        const auto point = [](float x, float y) { return D2D1::Point2F(x, y); };
        const auto bezier = [](ID2D1GeometrySink* sink, D2D1_POINT_2F control1, D2D1_POINT_2F control2, D2D1_POINT_2F end) {
            sink->AddBezier(D2D1::BezierSegment(control1, control2, end));
        };
        const auto addShellOutline = [&](ID2D1GeometrySink* sink, D2D1_FIGURE_END figureEnd) {
            sink->BeginFigure(point(shellLeft - joinRadius, panelTop), D2D1_FIGURE_BEGIN_HOLLOW);
            bezier(sink, point(shellLeft - joinRadius + curve * joinRadius, panelTop),
                point(shellLeft, panelTop - joinRadius + curve * joinRadius), point(shellLeft, panelTop - joinRadius));
            sink->AddLine(point(shellLeft, shellTop + shellRadius));
            bezier(sink, point(shellLeft, shellTop + shellRadius - curve * shellRadius),
                point(shellLeft + shellRadius - curve * shellRadius, shellTop), point(shellLeft + shellRadius, shellTop));
            sink->AddLine(point(shellRight - shellRadius, shellTop));
            bezier(sink, point(shellRight - shellRadius + curve * shellRadius, shellTop),
                point(shellRight, shellTop + shellRadius - curve * shellRadius), point(shellRight, shellTop + shellRadius));
            sink->AddLine(point(shellRight, panelTop - joinRadius));
            bezier(sink, point(shellRight, panelTop - joinRadius + curve * joinRadius),
                point(shellRight + joinRadius - curve * joinRadius, panelTop), point(shellRight + joinRadius, panelTop));
            sink->EndFigure(figureEnd);
        };

        ComPtr<ID2D1PathGeometry> shellFill, shellOutline, panelOutline;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(d2dFactory_->CreatePathGeometry(&shellFill)) || FAILED(shellFill->Open(&sink))) return false;
        sink->BeginFigure(point(shellLeft - joinRadius, panelTop), D2D1_FIGURE_BEGIN_FILLED);
        bezier(sink, point(shellLeft - joinRadius + curve * joinRadius, panelTop),
            point(shellLeft, panelTop - joinRadius + curve * joinRadius), point(shellLeft, panelTop - joinRadius));
        sink->AddLine(point(shellLeft, shellTop + shellRadius));
        bezier(sink, point(shellLeft, shellTop + shellRadius - curve * shellRadius),
            point(shellLeft + shellRadius - curve * shellRadius, shellTop), point(shellLeft + shellRadius, shellTop));
        sink->AddLine(point(shellRight - shellRadius, shellTop));
        bezier(sink, point(shellRight - shellRadius + curve * shellRadius, shellTop),
            point(shellRight, shellTop + shellRadius - curve * shellRadius), point(shellRight, shellTop + shellRadius));
        sink->AddLine(point(shellRight, panelTop - joinRadius));
        bezier(sink, point(shellRight, panelTop - joinRadius + curve * joinRadius),
            point(shellRight + joinRadius - curve * joinRadius, panelTop), point(shellRight + joinRadius, panelTop));
        sink->AddLine(point(shellLeft - joinRadius, panelTop));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close()) || FAILED(d2dFactory_->CreatePathGeometry(&shellOutline)) || FAILED(shellOutline->Open(&sink))) return false;
        addShellOutline(sink.Get(), D2D1_FIGURE_END_OPEN);
        if (FAILED(sink->Close()) || FAILED(d2dFactory_->CreatePathGeometry(&panelOutline)) || FAILED(panelOutline->Open(&sink))) return false;
        sink->BeginFigure(point(panel.left + panelRadius, panel.top), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(point(shellLeft - joinRadius, panel.top));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->BeginFigure(point(shellRight + joinRadius, panel.top), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(point(panel.right - panelRadius, panel.top));
        bezier(sink, point(panel.right - panelRadius + curve * panelRadius, panel.top),
            point(panel.right, panel.top + panelRadius - curve * panelRadius), point(panel.right, panel.top + panelRadius));
        sink->AddLine(point(panel.right, panel.bottom - panelRadius));
        bezier(sink, point(panel.right, panel.bottom - panelRadius + curve * panelRadius),
            point(panel.right - panelRadius + curve * panelRadius, panel.bottom), point(panel.right - panelRadius, panel.bottom));
        sink->AddLine(point(panel.left + panelRadius, panel.bottom));
        bezier(sink, point(panel.left + panelRadius - curve * panelRadius, panel.bottom),
            point(panel.left, panel.bottom - panelRadius + curve * panelRadius), point(panel.left, panel.bottom - panelRadius));
        sink->AddLine(point(panel.left, panel.top + panelRadius));
        bezier(sink, point(panel.left, panel.top + panelRadius - curve * panelRadius),
            point(panel.left + panelRadius - curve * panelRadius, panel.top), point(panel.left + panelRadius, panel.top));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        if (FAILED(sink->Close())) return false;
        renderTarget_->FillGeometry(shellFill.Get(), shellSurface);
        renderTarget_->DrawGeometry(panelOutline.Get(), panelBorder, scale);
        renderTarget_->DrawGeometry(shellOutline.Get(), shellBorder, scale);
        return true;
    }
#ifdef _DEBUG
    void TraceFilmstripPostStopPaint(const RECT& strip, size_t first, size_t last) {
        if (!filmstripPostStopPosition_ || filmstripScrollAnimating_) return;
        const double difference = filmstripScroll_ - *filmstripPostStopPosition_;
        if (std::abs(difference) > 0.01) {
            wchar_t mutation[256]{};
            swprintf_s(mutation, L"Viewtrious FILMSTRIP_POST_STOP_MUTATION old=%.3f new=%.3f delta=%.3f animating=0\n",
                *filmstripPostStopPosition_, filmstripScroll_, difference);
            OutputDebugStringW(mutation);
            filmstripPostStopPosition_ = filmstripScroll_;
        }
        if (filmstripPostStopPaintCount_ >= 3) return;
        const size_t anchor = first < last ? first : 0;
        const RECT item = anchor < navigationFiles_.size() ? GetFilmstripThumbnailBounds(anchor) : RECT{};
        wchar_t message[320]{};
        swprintf_s(message, L"Viewtrious filmstrip scroll: idlePaint=%u position=%.3f clip=[%ld,%ld] anchor=%zu slot=%.3f drawX=%ld\n",
            ++filmstripPostStopPaintCount_, filmstripScroll_, strip.left, strip.right, anchor,
            anchor < filmstripItemOffsets_.size() ? filmstripItemOffsets_[anchor] : 0.0f, item.left);
        OutputDebugStringW(message);
    }
#endif
    void DrawFilmstrip() {
        if (!FilmstripVisible()) {
            if (FilmstripEligible() && !alwaysShowFilmstrip_ && filmstripOpacity_ <= 0.001f) {
                RECT client{}; GetClientRect(window_, &client);
                const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
                const float frameWidth = 12.0f * scale, frameHeight = 8.0f * scale, gap = 3.0f * scale;
                const float total = frameWidth * 5.0f + gap * 4.0f;
                const float left = (client.right - total) * 0.5f;
                const float top = static_cast<float>(client.bottom) - 18.0f * scale;
                ComPtr<ID2D1SolidColorBrush> glyph;
                if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, filmstripRevealHovered_ ? 0.20f : 0.15f), &glyph))) {
                    for (int i = 0; i < 5; ++i) renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left + i * (frameWidth + gap), top, left + i * (frameWidth + gap) + frameWidth, top + frameHeight), 1.5f * scale, 1.5f * scale), glyph.Get(), scale);
                }
            }
            return;
        }
        const RECT strip = GetFilmstripBounds();
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        const float opacity = filmstripOpacity_;
        const bool drawPreviewShell = FilmstripHoverPreviewEligible(filmstripPreviewIndex_) && !filmstripDragging_ && !filmstripScrollAnimating_ &&
            filmstripHoverPreviewOpacity_ > 0.001f;
        if (drawPreviewShell && !filmstripPreviewGeometryValid_) SetFilmstripHoverPreviewGeometry(static_cast<size_t>(filmstripPreviewIndex_));
        ComPtr<ID2D1SolidColorBrush> surface, border, previewSurface, previewBorder, selectedBacking, selectedGlow, selectedOutline, hover, placeholder, placeholderText;
        if (FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(15.f / 255, 17.f / 255, 21.f / 255, 0.78f * opacity), &surface)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(91.f / 255, 102.f / 255, 120.f / 255, 0.70f * opacity), &border)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(15.f / 255, 17.f / 255, 21.f / 255, 0.78f * opacity * filmstripHoverPreviewOpacity_), &previewSurface)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(91.f / 255, 102.f / 255, 120.f / 255, 0.70f * opacity * filmstripHoverPreviewOpacity_), &previewBorder)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f / 255, 90.f / 255, 160.f / 255, 0.22f * opacity), &selectedBacking)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f / 255, 120.f / 255, 212.f / 255, 0.25f * opacity), &selectedGlow)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f / 255, 150.f / 255, 255.f / 255, opacity), &selectedOutline)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.16f * opacity), &hover)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(24.f / 255, 26.f / 255, 30.f / 255, opacity), &placeholder)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.42f * opacity), &placeholderText))) return;
        const D2D1_RECT_F panel = D2D1::RectF(static_cast<float>(strip.left), static_cast<float>(strip.top), static_cast<float>(strip.right), static_cast<float>(strip.bottom));
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(panel, 12.0f * scale, 12.0f * scale), surface.Get());
        if (!drawPreviewShell || !DrawFilmstripHoverPreviewShell(panel, border.Get(), previewSurface.Get(), previewBorder.Get(), scale))
            renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(panel, 12.0f * scale, 12.0f * scale), border.Get(), scale);
        renderTarget_->PushAxisAlignedClip(panel, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const size_t current = CurrentNavigationIndex();
        const auto [first, last] = FilmstripVisibleRange();
#ifdef _DEBUG
        TraceFilmstripPostStopPaint(strip, first, last);
#endif
        for (size_t index = first; index < last; ++index) {
            const RECT bounds = GetFilmstripThumbnailBounds(index);
            const D2D1_RECT_F box = D2D1::RectF(static_cast<float>(bounds.left), static_cast<float>(bounds.top), static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));
            const D2D1_RECT_F selection = D2D1::RectF(box.left - 4.0f * scale, box.top - 4.0f * scale, box.right + 4.0f * scale, box.bottom + 4.0f * scale);
            if (index == current) {
                renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(selection, 8.0f * scale, 8.0f * scale), selectedBacking.Get());
                renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(selection, 8.0f * scale, 8.0f * scale), selectedGlow.Get(), 4.0f * scale);
            } else if (index == static_cast<size_t>(filmstripHoveredIndex_)) {
                renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(box, 6.0f * scale, 6.0f * scale), hover.Get(), scale);
            }
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(box, 6.0f * scale, 6.0f * scale), placeholder.Get());
            if (ID2D1Bitmap* thumbnail = FilmstripThumbnailBitmap(index)) {
                renderTarget_->DrawBitmap(thumbnail, box, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else if (IsVideoPath(navigationFiles_[index].wstring())) {
                DrawOverlayText(L"video", box.left, box.top, box.right - box.left, box.bottom - box.top, 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, placeholderText.Get(), true, false, true);
            } else {
                DrawOverlayText(L"image", box.left, box.top, box.right - box.left, box.bottom - box.top, 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, placeholderText.Get(), true, false, true);
            }
            if (IsVideoPath(navigationFiles_[index].wstring())) {
                const float diameter = std::min(20.0f * scale, std::max(12.0f * scale, (box.bottom - box.top) * 0.32f));
                const float left = box.right - diameter - 5.0f * scale;
                const float top = box.bottom - diameter - 5.0f * scale;
                const UINT iconSize = static_cast<UINT>(std::max(1.0f, std::round(diameter)));
                if (EnsureFilmstripVideoIcon(iconSize))
                    renderTarget_->DrawBitmap(filmstripVideoIcon_.Get(), D2D1::RectF(left, top, left + diameter, top + diameter), opacity,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            if (index == current) renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(box, 6.0f * scale, 6.0f * scale), selectedOutline.Get(), 2.0f * scale);
        }
        renderTarget_->PopAxisAlignedClip();
        const RECT delaySlider = GetFilmstripHoverDelaySliderBounds();
        const float delayProgress = (filmstripHoverPreviewDelayMs_ - 100.0f) / 900.0f;
        const D2D1_RECT_F slider = D2D1::RectF(static_cast<float>(delaySlider.left), static_cast<float>(delaySlider.top), static_cast<float>(delaySlider.right), static_cast<float>(delaySlider.bottom));
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(slider, 6.0f * scale, 6.0f * scale), surface.Get());
        const float trackLeft = slider.left + 112.0f * scale, trackRight = slider.right - 12.0f * scale, trackY = (slider.top + slider.bottom) * 0.5f;
        renderTarget_->DrawLine(D2D1::Point2F(trackLeft, trackY), D2D1::Point2F(trackRight, trackY), border.Get(), 2.0f * scale);
        const float thumbX = trackLeft + (trackRight - trackLeft) * delayProgress;
        renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, trackY), 4.5f * scale, 4.5f * scale), selectedOutline.Get());
        const std::wstring delayLabel = L"TEST hover " + std::to_wstring(filmstripHoverPreviewDelayMs_) + L" ms";
        DrawOverlayText(delayLabel.c_str(), slider.left + 8.0f * scale, slider.top, 100.0f * scale, slider.bottom - slider.top, 9.5f, DWRITE_FONT_WEIGHT_NORMAL, placeholderText.Get(), false, false, true);
        if (FilmstripHoverPreviewEligible(filmstripPreviewIndex_) && !filmstripDragging_ && !filmstripScrollAnimating_) {
#ifdef _DEBUG
            OutputDebugStringW(L"[Viewtrious] FILMSTRIP_HOVER_PREVIEW_PAINT_ENTER clip=popped\n");
#endif
            const size_t previewIndex = static_cast<size_t>(filmstripPreviewIndex_);
            ID2D1Bitmap* videoPreviewBitmap = FilmstripVideoHoverPreviewBitmap(previewIndex);
            ID2D1Bitmap* previewBitmap = videoPreviewBitmap ? videoPreviewBitmap : FilmstripHoverPreviewBitmap(previewIndex);
#ifdef _DEBUG
            const bool highQuality = previewBitmap != nullptr;
#endif
            if (!previewBitmap) previewBitmap = FilmstripThumbnailBitmap(previewIndex);
            if (previewBitmap) {
                if (!filmstripPreviewGeometryValid_) SetFilmstripHoverPreviewGeometry(previewIndex);
                const D2D1_SIZE_F size = previewBitmap->GetSize();
                const D2D1_RECT_F preview = FitFilmstripHoverPreviewBitmap(filmstripPreviewGeometry_, size);
#ifdef _DEBUG
                wchar_t message[512]{};
                swprintf_s(message, L"[Viewtrious] FILMSTRIP_HOVER_PREVIEW_DRAW bitmap=%.0fx%.0f rect=%.1f,%.1f,%.1f,%.1f\n", size.width, size.height, preview.left, preview.top, preview.right, preview.bottom);
                OutputDebugStringW(message);
#endif
                const float previewOpacity = filmstripHoverPreviewOpacity_;
                if (filmstripVideoHoverFadeActive_ && videoPreviewBitmap) {
                    const float fadeProgress = FilmstripVideoHoverFadeProgress();
                    if (ID2D1Bitmap* staticThumbnail = FilmstripThumbnailBitmap(previewIndex))
                        renderTarget_->DrawBitmap(staticThumbnail, preview, fadeProgress * previewOpacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    renderTarget_->DrawBitmap(videoPreviewBitmap, preview, (1.0f - fadeProgress) * previewOpacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                } else {
                    renderTarget_->DrawBitmap(previewBitmap, preview, previewOpacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                }
                if (filmstripVideoHoverLoading_ && !filmstripVideoHoverFadeActive_)
                    DrawOverlayText(L"video loading...", preview.left, preview.top, preview.right - preview.left, preview.bottom - preview.top,
                        22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, placeholderText.Get(), true, false, true);
#ifdef _DEBUG
                if (highQuality) OutputDebugStringW(L"[Viewtrious] FILMSTRIP_HD_PREVIEW_DRAW\n");
#endif
            } else {
#ifdef _DEBUG
                OutputDebugStringW(L"[Viewtrious] FILMSTRIP_HOVER_CACHE_LOOKUP resident=0\n");
#endif
            }
        }
    }

    void RefreshNavigationFromFileSystem() {
        if (source_ && !TutorialActive()) BuildNavigation(true);
    }

    void Navigate(int direction, bool immediatePaint = true) {
        ClearStillDissolve();
        const std::optional<std::wstring> path = NavigationTargetPath(direction);
        if (!path) return;
        LoadContent(*path, false);
        if (immediatePaint) UpdateWindow(window_);
    }

    std::optional<std::wstring> NavigationTargetPath(int direction) {
        if (currentPath_.empty() || ModelActive()) return std::nullopt;
        BuildNavigation(true);
        if (navigationFiles_.size() < 2) return std::nullopt;

        const fs::path current(currentPath_);
        auto currentIt = std::find_if(navigationFiles_.begin(), navigationFiles_.end(),
            [&current](const fs::path& path) { return PathsEqual(path, current); });
        const ptrdiff_t count = static_cast<ptrdiff_t>(navigationFiles_.size());
        const bool currentMissing = currentIt == navigationFiles_.end();
        const ptrdiff_t start = currentMissing ? (direction > 0 ? -1 : 0) : std::distance(navigationFiles_.begin(), currentIt);
        ptrdiff_t index = (start + direction) % count;
        if (index < 0) index += count;
        return navigationFiles_[index].wstring();
    }

    float VideoFitScale() const {
        DWORD width = 0, height = 0;
        if (!VideoActive() || !videoPlayer_.GetNativeVideoSize(width, height)) return 1.0f;
        const RECT canvas = ModelCanvasBounds();
        return std::min(static_cast<float>(std::max(1L, canvas.right - canvas.left)) / width,
            static_cast<float>(std::max(1L, canvas.bottom - canvas.top)) / height);
    }
    float VideoCurrentScale() const { return videoFitToWindow_ ? VideoFitScale() : videoZoom_; }
    void ClampVideoPan() {
        DWORD nativeWidth = 0, nativeHeight = 0;
        if (!VideoActive() || !videoPlayer_.GetNativeVideoSize(nativeWidth, nativeHeight)) return;
        const RECT canvas = ModelCanvasBounds();
        const float canvasWidth = static_cast<float>(std::max(1L, canvas.right - canvas.left));
        const float canvasHeight = static_cast<float>(std::max(1L, canvas.bottom - canvas.top));
        const float scale = VideoCurrentScale();
        const float width = nativeWidth * scale, height = nativeHeight * scale;
        const float inset = 100.0f * static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        const float visibleX = std::min(width, inset), visibleY = std::min(height, inset);
        const float centeredX = (canvasWidth - width) * 0.5f, centeredY = (canvasHeight - height) * 0.5f;
        videoPan_.x = std::clamp(videoPan_.x, visibleX - centeredX - width, canvasWidth - visibleX - centeredX);
        videoPan_.y = std::clamp(videoPan_.y, visibleY - centeredY - height, canvasHeight - visibleY - centeredY);
    }
    void SetVideoScaleAt(POINT cursor, float requestedScale) {
        DWORD nativeWidth = 0, nativeHeight = 0;
        if (!VideoActive() || !videoPlayer_.GetNativeVideoSize(nativeWidth, nativeHeight)) return;
        const float oldScale = VideoCurrentScale(), fitScale = VideoFitScale();
        const float newScale = std::clamp(requestedScale, fitScale, std::max(kMaximumZoom, fitScale));
        if (newScale <= fitScale + 0.0001f) {
            if (videoFitToWindow_ && std::abs(videoPan_.x) < 0.0001f && std::abs(videoPan_.y) < 0.0001f) return;
            videoFitToWindow_ = true;
            videoZoom_ = fitScale;
            videoPan_ = D2D1::Point2F();
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (std::abs(newScale - oldScale) < 0.0001f) return;
        const RECT canvas = ModelCanvasBounds();
        const float canvasWidth = static_cast<float>(std::max(1L, canvas.right - canvas.left));
        const float canvasHeight = static_cast<float>(std::max(1L, canvas.bottom - canvas.top));
        const float oldLeft = canvas.left + (canvasWidth - nativeWidth * oldScale) * 0.5f + videoPan_.x;
        const float oldTop = canvas.top + (canvasHeight - nativeHeight * oldScale) * 0.5f + videoPan_.y;
        const float ratio = newScale / oldScale;
        videoPan_.x = static_cast<float>(cursor.x) - (static_cast<float>(cursor.x) - oldLeft) * ratio + nativeWidth * newScale * 0.5f - (canvas.left + canvasWidth * 0.5f);
        videoPan_.y = static_cast<float>(cursor.y) - (static_cast<float>(cursor.y) - oldTop) * ratio + nativeHeight * newScale * 0.5f - (canvas.top + canvasHeight * 0.5f);
        videoFitToWindow_ = false;
        videoZoom_ = newScale;
        ClampVideoPan();
        InvalidateRect(window_, nullptr, FALSE);
    }
    bool VideoContains(POINT point) const {
        DWORD nativeWidth = 0, nativeHeight = 0;
        if (!VideoActive() || !videoPlayer_.GetNativeVideoSize(nativeWidth, nativeHeight)) return false;
        const RECT canvas = ModelCanvasBounds();
        const float scale = VideoCurrentScale(), width = nativeWidth * scale, height = nativeHeight * scale;
        const float left = canvas.left + (canvas.right - canvas.left - width) * 0.5f + videoPan_.x;
        const float top = canvas.top + (canvas.bottom - canvas.top - height) * 0.5f + videoPan_.y;
        return point.x >= left && point.x < left + width && point.y >= top && point.y < top + height;
    }
    void SetScaleAt(POINT cursor, float requestedScale, bool recenterAtMinimum = true) {
        if (!source_) return;
        const float oldScale = CurrentScale();
        const float baseScale = BaseScale();
        const float minimumScale = MinimumScale();
        const float newScale = std::clamp(requestedScale, minimumScale, std::max(kMaximumZoom, baseScale));
        if (!std::isfinite(newScale) || newScale <= 0.0001f) return;
        if (newScale <= minimumScale + 0.0001f) {
            if (recenterAtMinimum) {
                CenterAtMinimumScale();
                return;
            }
            if (std::abs(newScale - oldScale) < 0.0001f) return;
            fitToWindow_ = false;
            zoom_ = newScale;
            if (lanczosSelected_) {
                InvalidateLanczosVariant(true);
                QueueLanczosRefinement();
            }
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (std::abs(newScale - oldScale) < 0.0001f) return;

        const D2D1_SIZE_F target = ImageCanvasSize();
        const D2D1_RECT_F canvas = ImageCanvasBounds();
        const D2D1_POINT_2F oldTopLeft = ImageTopLeft(oldScale, target);
        const float ratio = newScale / oldScale;
        pan_.x = static_cast<float>(cursor.x) - (static_cast<float>(cursor.x) - oldTopLeft.x) * ratio +
            imageWidth_ * newScale / 2.0f - (canvas.left + target.width / 2.0f);
        pan_.y = static_cast<float>(cursor.y) - (static_cast<float>(cursor.y) - oldTopLeft.y) * ratio +
            imageHeight_ * newScale / 2.0f - (canvas.top + target.height / 2.0f);
        fitToWindow_ = false;
        zoom_ = newScale;
        ClampPan();
        if (lanczosSelected_) {
            InvalidateLanczosVariant(true);
            QueueLanczosRefinement();
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void ZoomAt(POINT cursor, float factor) { if (VideoActive()) SetVideoScaleAt(cursor, VideoCurrentScale() * factor); else SetScaleAt(cursor, CurrentScale() * factor); }
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
        if (fitToWindow_ || PhysicalPixelScale() < 1.0f - 0.0001f) ZoomToActualPixels(cursor);
        else FitToWindow();
    }

    void ZoomCentered(float factor) {
        const D2D1_SIZE_F canvas = ImageCanvasSize();
        const D2D1_RECT_F bounds = ImageCanvasBounds();
        ZoomAt({ static_cast<LONG>(bounds.left + canvas.width / 2.0f), static_cast<LONG>(bounds.top + canvas.height / 2.0f) }, factor);
    }

    void FitToWindow() {
        if (VideoActive()) { videoFitToWindow_ = true; videoPan_ = D2D1::Point2F(); InvalidateRect(window_, nullptr, FALSE); return; }
        if (!source_) return;
        const float oldScale = CurrentScale();
        const D2D1_POINT_2F oldPan = pan_;
        fitToWindow_ = true;
        pan_ = D2D1::Point2F();
        if (lanczosSelected_ && (std::abs(CurrentScale() - oldScale) >= 0.0001f ||
                std::abs(oldPan.x) >= 0.0001f || std::abs(oldPan.y) >= 0.0001f)) {
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
        if (swipeToNavigateWhenFit_) {
            if (!CanPan()) return;
        } else if (!source_ && !VideoActive()) return;
        dragging_ = true;
        lastDragPoint_ = point;
        SetCapture(window_);
    }

    bool BeginSwipeNavigation(POINT point) {
        if (!swipeToNavigateWhenFit_ || ModelActive() || CanPan() || currentPath_.empty() ||
            CanvasNavigationZoneAt(point) != ButtonKind::None || VideoControlsContains(point) ||
            !(VideoActive() ? VideoContains(point) : ImageContains(point))) return false;
        swipeNavigationPending_ = true;
        swipeNavigationStart_ = point;
        SetCapture(window_);
        return true;
    }

    bool SwipeNavigationPending() const { return swipeNavigationPending_; }

    bool FinishSwipeNavigation(POINT point) {
        if (!swipeNavigationPending_) return false;
        swipeNavigationPending_ = false;
        const LONG deltaX = point.x - swipeNavigationStart_.x;
        const LONG deltaY = point.y - swipeNavigationStart_.y;
        const LONG horizontalDistance = std::abs(deltaX);
        const LONG verticalDistance = std::abs(deltaY);
        const LONG threshold = MulDiv(72, GetDpiForWindow(window_), 96);
        if (horizontalDistance >= threshold && horizontalDistance >= verticalDistance * 2) {
            const int direction = deltaX < 0 ? 1 : -1;
            if (BeginStillDissolveNavigation(direction)) SelectNavigationTarget(dissolveTargetPath_, direction);
            else Navigate(direction);
        }
        return true;
    }

    void CancelSwipeNavigation() { swipeNavigationPending_ = false; }

    bool BeginStillDissolveNavigation(int direction) {
        if (!source_ || gifPlaying_ || VideoActive() || ModelActive() || dissolveAwaitingTarget_ || dissolveActive_) return false;
        const std::optional<std::wstring> target = NavigationTargetPath(direction);
        if (!target || IsGifPath(*target) || IsVideoPath(*target) || IsModelPath(*target)) return false;

        EnsureBitmap();
        if (!bitmap_) return false;
        dissolveOldBitmap_ = bitmap_;
        if (!imageAdjustments_.IsNeutral() && EnsureImageAdjustedBitmap()) dissolveOldBitmap_ = imageAdjustedBitmap_;
        if (!dissolveOldBitmap_) return false;

        dissolveOldWidth_ = imageWidth_;
        dissolveOldHeight_ = imageHeight_;
        dissolveTargetPath_ = *target;
        if (!QueryPerformanceFrequency(&dissolveQpcFrequency_) || dissolveQpcFrequency_.QuadPart <= 0) { ClearStillDissolve(); return false; }
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        dissolveStartQpc_ = now.QuadPart;
        dissolveAwaitingTarget_ = true;
        SetTimer(window_, kStillDissolveTimer, 15, nullptr);
        return true;
    }

    void BeginStillDissolveIfReady(const std::wstring& path) {
        if (!dissolveAwaitingTarget_ || !PathsEqual(fs::path(path), fs::path(dissolveTargetPath_))) return;
        dissolveAwaitingTarget_ = false;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        dissolveStartQpc_ = now.QuadPart;
        dissolveActive_ = true;
        InvalidateRect(window_, nullptr, FALSE);
    }

    void ClearStillDissolve() {
        KillTimer(window_, kStillDissolveTimer);
        dissolveActive_ = false;
        dissolveAwaitingTarget_ = false;
        dissolveOldBitmap_.Reset();
        dissolveOldWidth_ = dissolveOldHeight_ = 0;
        dissolveTargetPath_.clear();
    }

    float StillDissolveProgress() const {
        if (!dissolveActive_ || dissolveQpcFrequency_.QuadPart <= 0) return 1.0f;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsedMs = static_cast<double>(now.QuadPart - dissolveStartQpc_) * 1000.0 / static_cast<double>(dissolveQpcFrequency_.QuadPart);
        return std::clamp(static_cast<float>(elapsedMs / kStillDissolveDurationMs), 0.0f, 1.0f);
    }

    double StillDissolveElapsedMs() const {
        if (dissolveQpcFrequency_.QuadPart <= 0) return 0.0;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart - dissolveStartQpc_) * 1000.0 / static_cast<double>(dissolveQpcFrequency_.QuadPart);
    }

    void UpdateStillDissolve() {
        if (dissolveAwaitingTarget_) {
            if (StillDissolveElapsedMs() >= kStillDissolvePreviewWaitMaxMs) ClearStillDissolve();
            return;
        }
        if (!dissolveActive_) return;
        if (StillDissolveProgress() >= 1.0f) ClearStillDissolve();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void PanTo(POINT point) {
        if (!dragging_) return;
        const LONG deltaX = point.x - lastDragPoint_.x;
        const LONG deltaY = point.y - lastDragPoint_.y;
        if (VideoActive()) {
            if (videoFitToWindow_ && (deltaX || deltaY)) {
                videoZoom_ = VideoCurrentScale();
                videoFitToWindow_ = false;
            }
            videoPan_.x += static_cast<float>(deltaX);
            videoPan_.y += static_cast<float>(deltaY);
            lastDragPoint_ = point;
            ClampVideoPan();
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (fitToWindow_ && (deltaX || deltaY)) {
            zoom_ = CurrentScale();
            fitToWindow_ = false;
        }
        pan_.x += static_cast<float>(deltaX);
        pan_.y += static_cast<float>(deltaY);
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
        SetScaleAt({ static_cast<LONG>(center.width / 2.0f), static_cast<LONG>(center.height / 2.0f) }, requestedScale, false);
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
        if (fullscreen_) placement = fullscreenPlacement_;
        else if (!GetWindowPlacement(window_, &placement)) return;
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
        FlushImageAdjustmentPersistence();
        adjustmentPersistence_.Shutdown();
        MSG adjustmentMessage{};
        while (PeekMessageW(&adjustmentMessage, window_, kImageAdjustmentPersistenceCompleteMessage, kImageAdjustmentPersistenceCompleteMessage, PM_REMOVE))
            delete reinterpret_cast<ImageAdjustmentPersistenceResult*>(adjustmentMessage.lParam);
        StopFilmstripScrollScheduler();
        StopFilmstripHoverPreviewWorker();
        StopFilmstripThumbnailWorker();
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
        ClearStillDissolve();
        ++aiRequestGeneration_;
        if (aiAnalysisThread_.joinable()) aiAnalysisThread_.join();
        MSG aiMessage{};
        while (PeekMessageW(&aiMessage, window_, kAiAnalysisCompleteMessage, kAiAnalysisCompleteMessage, PM_REMOVE))
            delete reinterpret_cast<AiAnalysisResult*>(aiMessage.lParam);
        aiAddon_.Shutdown();
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
    void FilmstripThumbnailCompleteMessage(FilmstripThumbnailResult* result) { HandleFilmstripThumbnailResult(result); }
    void FilmstripHoverPreviewCompleteMessage(FilmstripHoverPreviewResult* result) { HandleFilmstripHoverPreviewResult(result); }
    void ImageAdjustmentPersistenceCompleteMessage(ImageAdjustmentPersistenceResult* result) {
        std::unique_ptr<ImageAdjustmentPersistenceResult> owned(result);
        if (!result || shuttingDown_ || !result->hashResolved) return;
        if (result->mediaGeneration != imageAdjustmentMediaGeneration_ || !PathsEqual(fs::path(result->path), fs::path(currentPath_))) {
            const auto pending = pendingImageAdjustmentSaves_.find(result->mediaGeneration);
            if (pending != pendingImageAdjustmentSaves_.end()) {
                adjustmentPersistence_.Save(result->hash, pending->second);
                pendingImageAdjustmentSaves_.erase(pending);
            }
            OutputDebugStringW(L"[Viewtrious] ADJUST_DB_LOAD_STALE_GENERATION\n");
            return;
        }
        imageAdjustmentHash_ = result->hash;
        imageAdjustmentHashResolved_ = true;
        if (result->editGeneration != imageAdjustmentEditGeneration_) {
            pendingImageAdjustmentSaves_.erase(result->mediaGeneration);
            FlushImageAdjustmentPersistence();
            return;
        }
        if (result->hasAdjustments) {
            imageAdjustments_ = result->adjustments;
            ApplyImageAdjustments();
            OutputDebugStringW(L"[Viewtrious] ADJUST_DB_LOAD_HIT\n");
        } else {
            OutputDebugStringW(L"[Viewtrious] ADJUST_DB_LOAD_NEUTRAL\n");
        }
    }
    void ImageAdjustmentPersistenceTimer() { FlushImageAdjustmentPersistence(); }
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
        currentPath_ = path; SuppressFilmstripHoverPreviewForCurrentMedia(); displayedPath_.clear(); source_.Reset(); bitmap_.Reset(); displayedPixels_.reset(); imageWidth_ = imageHeight_ = 0;
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
        DeactivateModel(); DeactivateVideo(); StopGifPlayback(); StopDirectoryWatcher(); InvalidateLanczosVariant(false);
        videoPreferredPlaybackRatePercent_ = 100;
        videoEffectivePlaybackRate_ = 1.0;
        ++decodeRequestGeneration_; ++modelLoadGeneration_; pendingFullDecode_.reset(); imageDecodePending_ = false;
        source_.Reset(); bitmap_.Reset(); displayedPixels_.reset(); imageWidth_ = imageHeight_ = 0;
        videoFitToWindow_ = true; videoZoom_ = 1.0f; videoPan_ = D2D1::Point2F();
        currentPath_ = path; SuppressFilmstripHoverPreviewForCurrentMedia(); displayedPath_.clear(); filenameText_ = fs::path(path).filename().wstring();
        currentFileIdentity_ = ReadFileIdentity(fs::path(path));
        fileSizeText_ = FormatFileSize(path); resolutionText_.clear(); error_.clear();
        navigationFiles_.clear(); navigationBuilt_ = false; navigationBuildQueued_ = false; contentKind_ = ContentKind::Video2D;
        ResetVideoControls();
        EnsureRenderTarget();
        std::wstring videoError;
        if (!graphicsHost_.Ready() || !videoPlayer_.Open(window_, graphicsHost_.Device(), path, videoError)) {
            contentKind_ = ContentKind::None;
            error_ = videoError.empty() ? L"Viewtrious could not open this video." : videoError;
        } else {
            videoPlayer_.SetDisplayAdjustments(videoAdjustments_);
            videoPlayer_.SetPreferredPlaybackRate(PlaybackRateFromPercent(videoPreferredPlaybackRatePercent_));
            videoEffectivePlaybackRate_ = videoPlayer_.EffectivePlaybackRate();
        }
        InvalidateRect(window_, nullptr, FALSE);
    }
    void DeactivateVideo() {
        if (fullscreen_ && !shuttingDown_) ToggleFullscreen();
        videoPausedSeekRefreshPending_ = false;
        StopVideoPlaybackScheduler();
        StopVideoControls();
        videoPlayer_.Shutdown();
        if (contentKind_ == ContentKind::Video2D) { resolutionText_.clear(); contentKind_ = ContentKind::None; }
    }
public:
    void VideoMediaEngineEvent(DWORD event) {
        if (!VideoActive()) return;
        const bool wasPlaying = videoPlayer_.Playing();
        std::wstring videoError;
        videoPlayer_.HandleMediaEvent(event, videoError);
        videoEffectivePlaybackRate_ = videoPlayer_.EffectivePlaybackRate();
        UpdateVideoTitleMetadata();
        if (!videoError.empty()) error_ = videoError;
        if (videoPlayer_.Failed()) { DeactivateVideo(); InvalidateRect(window_, nullptr, FALSE); return; }
        if (event == MF_MEDIA_ENGINE_EVENT_SEEKED) {
            if (videoPlayer_.Playing()) videoPlayer_.UpdateFrame(VideoPlayer::FrameAcquisitionReason::Seek);
        } else if (!videoPlayer_.HasValidFrame() &&
            (event == MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY || event == MF_MEDIA_ENGINE_EVENT_CANPLAY)) {
            videoPlayer_.UpdateFrame(VideoPlayer::FrameAcquisitionReason::InitialLoad);
        }
        if ((!wasPlaying && videoPlayer_.Playing()) ||
            (event == MF_MEDIA_ENGINE_EVENT_SEEKED && videoPlayer_.Playing())) {
            ScheduleVideoPlaybackTimer(true);
        } else if (!videoPlayer_.Playing()) {
            StopVideoPlaybackScheduler();
        }
        if (event == MF_MEDIA_ENGINE_EVENT_ENDED || event == MF_MEDIA_ENGINE_EVENT_CANPLAY || event == MF_MEDIA_ENGINE_EVENT_PLAYING) ShowVideoControls();
        InvalidateRect(window_, nullptr, FALSE);
    }
    void UpdateVideoTitleMetadata() {
        if (!VideoActive()) return;
        DWORD width = 0, height = 0;
        if (videoPlayer_.GetNativeVideoSize(width, height)) resolutionText_ = std::to_wstring(width) + L" x " + std::to_wstring(height);
        float framesPerSecond = 0.0f;
        if (videoPlayer_.TryGetFramesPerSecond(framesPerSecond)) {
            if (!resolutionText_.empty()) resolutionText_ += L"  \x2022  " + FormatFramesPerSecond(framesPerSecond);
        }
    }
    void VideoPlaybackWakeMessage(uint64_t generation) {
        uint64_t expected = generation;
        videoPlaybackWakePendingGeneration_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
        if (generation != videoPlaybackSchedulerGeneration_.load(std::memory_order_acquire) || !VideoActive() || !videoPlayer_.Playing()) return;

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        videoPlayer_.RecordFramePacingTimer(videoPlaybackWakeQpc_.load(std::memory_order_acquire),
            static_cast<LONGLONG>(std::llround(videoPlaybackDeadlineQpc_)));
        videoPlayer_.UpdateFrame(VideoPlayer::FrameAcquisitionReason::Scheduler);
        // Advance the stable QPC grid past missed slots instead of replaying stale wakeups.
        while (videoPlaybackDeadlineQpc_ <= static_cast<double>(now.QuadPart))
            videoPlaybackDeadlineQpc_ += videoPlaybackFramePeriodQpc_;
        if (!ArmVideoPlaybackTimer()) {
            FailVideoPlaybackScheduler();
            return;
        }
        InvalidateRect(window_, nullptr, FALSE);
    }
private:
    void ScheduleVideoPlaybackTimer(bool resetDeadline = false) {
        if (!VideoActive() || !videoPlayer_.Playing()) {
            StopVideoPlaybackScheduler();
            return;
        }
        if (!resetDeadline && videoPlaybackSchedulerRunning_) return;

        float framesPerSecond = 0.0f;
        double framePeriodSeconds = 1.0 / 60.0;
        if (!videoPlayer_.TryGetFramesPerSecond(framesPerSecond) || !std::isfinite(framesPerSecond) || framesPerSecond <= 0.0f) {
            framePeriodSeconds = 0.016;
        } else {
            framePeriodSeconds = 1.0 / static_cast<double>(framesPerSecond);
        }
        if (!EnsureVideoPlaybackScheduler()) {
            FailVideoPlaybackScheduler();
            return;
        }

        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const double rate = std::clamp(videoPlayer_.EffectivePlaybackRate(), 0.25, 2.0);
        videoPlaybackFramePeriodQpc_ = framePeriodSeconds * static_cast<double>(frequency.QuadPart) / rate;
        videoPlaybackDeadlineQpc_ = static_cast<double>(now.QuadPart) + videoPlaybackFramePeriodQpc_;
        ++videoPlaybackSchedulerGeneration_;
        videoPlaybackWakePendingGeneration_.store(0, std::memory_order_release);
        videoPlaybackSchedulerRunning_ = true;
        if (!ArmVideoPlaybackTimer()) FailVideoPlaybackScheduler();
    }
    bool EnsureVideoPlaybackScheduler() {
        if (videoPlaybackTimer_) return true;
        videoPlaybackStopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        videoPlaybackTimer_ = CreateWaitableTimerExW(nullptr, nullptr, kHighResolutionWaitableTimerFlag, TIMER_ALL_ACCESS);
        if (!videoPlaybackStopEvent_ || !videoPlaybackTimer_) {
            if (videoPlaybackTimer_) CloseHandle(videoPlaybackTimer_);
            if (videoPlaybackStopEvent_) CloseHandle(videoPlaybackStopEvent_);
            videoPlaybackTimer_ = videoPlaybackStopEvent_ = nullptr;
            return false;
        }
        try {
            videoPlaybackSchedulerThread_ = std::thread([this] {
                HANDLE handles[] = { videoPlaybackStopEvent_, videoPlaybackTimer_ };
                for (;;) {
                    const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, INFINITE);
                    if (wait != WAIT_OBJECT_0 + 1) return;
                    const uint64_t generation = videoPlaybackSchedulerGeneration_.load(std::memory_order_acquire);
                    uint64_t none = 0;
                    LARGE_INTEGER wake{};
                    QueryPerformanceCounter(&wake);
                    videoPlaybackWakeQpc_.store(wake.QuadPart, std::memory_order_release);
                    // The helper only wakes the UI; Media Foundation and rendering remain on it.
                    if (videoPlaybackWakePendingGeneration_.compare_exchange_strong(none, generation, std::memory_order_acq_rel))
                        PostMessageW(window_, kVideoPlaybackWakeMessage, static_cast<WPARAM>(generation), 0);
                }
            });
        } catch (const std::system_error&) {
            CloseHandle(videoPlaybackTimer_);
            CloseHandle(videoPlaybackStopEvent_);
            videoPlaybackTimer_ = videoPlaybackStopEvent_ = nullptr;
            return false;
        }
        return true;
    }
    bool ArmVideoPlaybackTimer() {
        if (!videoPlaybackTimer_ || !videoPlaybackSchedulerRunning_) return false;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double remainingQpc = std::max(1.0, videoPlaybackDeadlineQpc_ - static_cast<double>(now.QuadPart));
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(std::llround(remainingQpc * 10000000.0 / videoPlaybackQpcFrequency())));
        videoPlayer_.RecordFramePacingSchedule(videoPlaybackFramePeriodQpc_ * 1000.0 / videoPlaybackQpcFrequency(), static_cast<LONGLONG>(std::llround(videoPlaybackDeadlineQpc_)));
        return SetWaitableTimer(videoPlaybackTimer_, &due, 0, nullptr, nullptr, FALSE) != FALSE;
    }
    void FailVideoPlaybackScheduler() {
        error_ = L"Viewtrious could not start video playback.";
        DeactivateVideo();
        InvalidateRect(window_, nullptr, FALSE);
    }
    double videoPlaybackQpcFrequency() const {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return static_cast<double>(frequency.QuadPart);
    }
    void StopVideoPlaybackScheduler() {
        videoPlaybackSchedulerRunning_ = false;
        ++videoPlaybackSchedulerGeneration_;
        videoPlaybackWakePendingGeneration_.store(0, std::memory_order_release);
        if (videoPlaybackTimer_) CancelWaitableTimer(videoPlaybackTimer_);
        if (videoPlaybackStopEvent_) SetEvent(videoPlaybackStopEvent_);
        if (videoPlaybackSchedulerThread_.joinable()) videoPlaybackSchedulerThread_.join();
        if (videoPlaybackTimer_) CloseHandle(videoPlaybackTimer_);
        if (videoPlaybackStopEvent_) CloseHandle(videoPlaybackStopEvent_);
        videoPlaybackTimer_ = videoPlaybackStopEvent_ = nullptr;
    }
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

    bool Reconcile3DAutoProgId(const wchar_t* extension, const std::wstring& executable, bool& changed) {
        const std::wstring autoProgId = std::wstring(extension + 1) + L"_auto_file";
        const std::wstring autoProgIdPath = std::wstring(L"Software\\Classes\\") + autoProgId;
        const std::wstring openCommandPath = autoProgIdPath + L"\\shell\\open\\command";
        std::wstring openCommand;
        if (!ReadRegistryStringIfPresent(HKEY_CURRENT_USER, openCommandPath.c_str(), L"", openCommand) ||
            !CommandOpensExecutable(openCommand, executable)) return true;

        const std::wstring iconReference = executable + L",-105";
        const std::wstring defaultIconPath = autoProgIdPath + L"\\DefaultIcon";
        std::wstring existingDefaultIcon;
        std::wstring existingTypeOverlay;
        const bool updateDefaultIcon = !ReadRegistryStringIfPresent(HKEY_CURRENT_USER, defaultIconPath.c_str(), L"", existingDefaultIcon) || existingDefaultIcon != iconReference;
        const bool updateTypeOverlay = !ReadRegistryStringIfPresent(HKEY_CURRENT_USER, autoProgIdPath.c_str(), L"TypeOverlay", existingTypeOverlay) || existingTypeOverlay != iconReference;
        bool success = true;
        if (updateDefaultIcon) success &= WriteRegistryString(HKEY_CURRENT_USER, defaultIconPath.c_str(), L"", iconReference);
        if (updateTypeOverlay) success &= WriteRegistryString(HKEY_CURRENT_USER, autoProgIdPath.c_str(), L"TypeOverlay", iconReference);
        if (!updateDefaultIcon && !updateTypeOverlay) return true;
        success &= VerifyRegistryString(HKEY_CURRENT_USER, defaultIconPath.c_str(), L"", iconReference);
        success &= VerifyRegistryString(HKEY_CURRENT_USER, autoProgIdPath.c_str(), L"TypeOverlay", iconReference);
        changed |= success;
        return success;
    }

    bool RegisterStlThumbnailProvider(const std::wstring& executable, bool& changed) {
        constexpr wchar_t kThumbnailHandlerClsid[] = L"{E357FCCD-A995-4576-B01F-234630154E96}";
        constexpr wchar_t kProviderClsid[] = L"{D812B4F2-B141-4A0D-9A4F-574DDB2975B2}";
        constexpr wchar_t kObsoleteProviderClsid[] = L"{6D3CF8C3-96CD-4E2E-B553-4AB90F097D1A}";
        const fs::path providerPath = fs::path(executable).parent_path() / L"ViewtriousStlThumbnail.dll";
        const DWORD attributes = GetFileAttributesW(providerPath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return true;

        const std::wstring extensionPath = std::wstring(L"Software\\Classes\\.stl\\shellex\\") + kThumbnailHandlerClsid;
        std::wstring existingProvider;
        if (ReadRegistryStringIfPresent(HKEY_CURRENT_USER, extensionPath.c_str(), L"", existingProvider) &&
            existingProvider != kProviderClsid && existingProvider != kObsoleteProviderClsid) return true;

        const std::wstring providerClassPath = std::wstring(L"Software\\Classes\\CLSID\\") + kProviderClsid;
        const std::wstring serverPath = providerClassPath + L"\\InprocServer32";
        const std::wstring providerDll = providerPath.wstring();
        std::wstring existingDll;
        std::wstring existingThreadingModel;
        const bool updateHandler = existingProvider != kProviderClsid;
        const bool updateDll = !ReadRegistryStringIfPresent(HKEY_CURRENT_USER, serverPath.c_str(), L"", existingDll) || existingDll != providerDll;
        const bool updateThreadingModel = !ReadRegistryStringIfPresent(HKEY_CURRENT_USER, serverPath.c_str(), L"ThreadingModel", existingThreadingModel) || existingThreadingModel != L"Both";
        if (!updateHandler && !updateDll && !updateThreadingModel) return true;

        bool success = true;
        if (existingProvider == kObsoleteProviderClsid)
            success &= DeleteRegistryTreeIfPresent(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\CLSID\\") + kObsoleteProviderClsid).c_str());
        if (updateHandler) success &= WriteRegistryString(HKEY_CURRENT_USER, extensionPath.c_str(), L"", kProviderClsid);
        if (updateDll) success &= WriteRegistryString(HKEY_CURRENT_USER, serverPath.c_str(), L"", providerDll);
        if (updateThreadingModel) success &= WriteRegistryString(HKEY_CURRENT_USER, serverPath.c_str(), L"ThreadingModel", L"Both");
        if (updateHandler) success &= VerifyRegistryString(HKEY_CURRENT_USER, extensionPath.c_str(), L"", kProviderClsid);
        if (updateDll) success &= VerifyRegistryString(HKEY_CURRENT_USER, serverPath.c_str(), L"", providerDll);
        if (updateThreadingModel) success &= VerifyRegistryString(HKEY_CURRENT_USER, serverPath.c_str(), L"ThreadingModel", L"Both");
        changed |= success;
        return success;
    }

    bool RegisterDefaultAppCapabilities() {
        wchar_t modulePath[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath))) {
            TraceRegistryFailure(L"resolve executable", L"(module path)", L"", GetLastError());
            return false;
        }
        const std::wstring executable(modulePath);
        const std::wstring command = L"\"" + executable + L"\" \"%1\"";
        const struct Association { const wchar_t* extension; const wchar_t* progId; const wchar_t* description; int iconResourceId; } associations[] = {
            { L".jpg", L"Viewtrious.jpg", L"JPG File", 101 },
            { L".jpeg", L"Viewtrious.jpeg", L"JPEG File", 101 },
            { L".png", L"Viewtrious.png", L"PNG File", 101 },
            { L".bmp", L"Viewtrious.bmp", L"BMP File", 101 },
            { L".gif", L"Viewtrious.gif", L"GIF File", 101 },
            { L".heic", L"Viewtrious.heic", L"HEIC File", 101 },
            { L".heif", L"Viewtrious.heif", L"HEIF File", 101 },
            { L".dng", L"Viewtrious.dng", L"DNG File", 101 },
            { L".mp4", L"Viewtrious.mp4", L"MP4 File", 104 },
            { L".mov", L"Viewtrious.mov", L"MOV File", 104 },
            { L".mkv", L"Viewtrious.mkv", L"MKV File", 104 },
            { L".stl", L"Viewtrious.stl", L"STL File", 105 },
            { L".3mf", L"Viewtrious.3mf", L"3MF File", 105 },
        };
        const auto registerAssociation = [&](const Association& association) {
            const std::wstring progIdPath = std::wstring(L"Software\\Classes\\") + association.progId;
            const std::wstring iconReference = executable + L",-" + std::to_wstring(association.iconResourceId);
            const std::wstring defaultIconPath = progIdPath + L"\\DefaultIcon";
            const std::wstring malformedTypeOverlayPath = progIdPath + L"\\TypeOverlay";
            const std::wstring openCommandPath = progIdPath + L"\\shell\\open\\command";
            const std::wstring capabilitiesPath = std::wstring(kCapabilitiesPath) + L"\\FileAssociations";
            const std::wstring openWithProgIdsPath = std::wstring(L"Software\\Classes\\") + association.extension + L"\\OpenWithProgids";
            bool success = true;
            success &= DeleteRegistryTreeIfPresent(HKEY_CURRENT_USER, malformedTypeOverlayPath.c_str());
            success &= WriteRegistryString(HKEY_CURRENT_USER, progIdPath.c_str(), L"", association.description);
            success &= WriteRegistryString(HKEY_CURRENT_USER, defaultIconPath.c_str(), L"", iconReference);
            success &= WriteRegistryString(HKEY_CURRENT_USER, progIdPath.c_str(), L"TypeOverlay", iconReference);
            success &= WriteRegistryString(HKEY_CURRENT_USER, openCommandPath.c_str(), L"", command);
            success &= WriteRegistryString(HKEY_CURRENT_USER, capabilitiesPath.c_str(), association.extension, association.progId);
            success &= WriteRegistryString(HKEY_CURRENT_USER, openWithProgIdsPath.c_str(), association.progId, L"");
            success &= VerifyRegistryString(HKEY_CURRENT_USER, progIdPath.c_str(), L"", association.description);
            success &= VerifyRegistryString(HKEY_CURRENT_USER, defaultIconPath.c_str(), L"", iconReference);
            success &= VerifyRegistryString(HKEY_CURRENT_USER, progIdPath.c_str(), L"TypeOverlay", iconReference);
            success &= VerifyRegistryString(HKEY_CURRENT_USER, openCommandPath.c_str(), L"", command);
            success &= VerifyRegistryString(HKEY_CURRENT_USER, capabilitiesPath.c_str(), association.extension, association.progId);
            success &= VerifyRegistryString(HKEY_CURRENT_USER, openWithProgIdsPath.c_str(), association.progId, L"");
            success &= VerifyRegistryKeyAbsent(HKEY_CURRENT_USER, malformedTypeOverlayPath.c_str());
            return success;
        };
        bool success = true;
        if (StepAddonPresent()) {
            const Association stepAssociations[] = { { L".step", L"Viewtrious.step", L"STEP File", 105 }, { L".stp", L"Viewtrious.stp", L"STP File", 105 } };
            for (const Association& association : stepAssociations) success &= registerAssociation(association);
        }
        for (const Association& association : associations) success &= registerAssociation(association);
        success &= WriteRegistryString(HKEY_CURRENT_USER, kCapabilitiesPath, L"ApplicationName", kRegisteredApplicationName);
        success &= WriteRegistryString(HKEY_CURRENT_USER, kCapabilitiesPath, L"ApplicationDescription", L"Viewtrious image viewer");
        success &= WriteRegistryString(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", kRegisteredApplicationName, kCapabilitiesPath);
        success &= VerifyRegistryString(HKEY_CURRENT_USER, kCapabilitiesPath, L"ApplicationName", kRegisteredApplicationName);
        success &= VerifyRegistryString(HKEY_CURRENT_USER, kCapabilitiesPath, L"ApplicationDescription", L"Viewtrious image viewer");
        success &= VerifyRegistryString(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", kRegisteredApplicationName, kCapabilitiesPath);
        bool thumbnailProviderChanged = false;
        success &= RegisterStlThumbnailProvider(executable, thumbnailProviderChanged);
        bool autoProgIdChanged = false;
        success &= Reconcile3DAutoProgId(L".stl", executable, autoProgIdChanged);
        success &= Reconcile3DAutoProgId(L".3mf", executable, autoProgIdChanged);
        if (StepAddonPresent()) {
            success &= Reconcile3DAutoProgId(L".step", executable, autoProgIdChanged);
            success &= Reconcile3DAutoProgId(L".stp", executable, autoProgIdChanged);
        }
        if (success) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
        return success;
    }

    void OpenRegisteredDefaultApps(bool verifyRegistration = true) {
        if (verifyRegistration && !RegisterDefaultAppCapabilities()) { ShowOverlay(OverlayKind::RegistrationError); return; }
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
        ShowVideoControls();
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

    void StartCopyFeedback(const wchar_t* text = L"copied to clipboard", bool wallpaper = false) {
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
        if (FAILED(copy)) { GlobalFree(memory); ShowActionError(L"Viewtrious could not copy this media to the clipboard."); return; }
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
        if (!OpenClipboard(window_)) { GlobalFree(memory); if (fileDrop) GlobalFree(fileDrop); ShowActionError(L"the clipboard is currently unavailable."); return; }
        EmptyClipboard();
        if (!SetClipboardData(CF_DIBV5, memory)) { CloseClipboard(); GlobalFree(memory); if (fileDrop) GlobalFree(fileDrop); ShowActionError(L"Viewtrious could not publish the media to the clipboard."); return; }
        if (fileDrop && !SetClipboardData(CF_HDROP, fileDrop)) GlobalFree(fileDrop);
        CloseClipboard();
        StartCopyFeedback();
    }

private:

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
        if (!ShellExecuteExW(&execute)) ShowOverlay(OverlayKind::PrintError);
    }

    static UINT RotatedOrientation(UINT orientation, bool clockwise) {
        static constexpr UINT clockwiseMap[] = { 0, 6, 7, 8, 5, 2, 3, 4, 1 };
        static constexpr UINT counterClockwiseMap[] = { 0, 8, 5, 6, 7, 4, 1, 2, 3 };
        if (orientation < 1 || orientation > 8) orientation = 1;
        return clockwise ? clockwiseMap[orientation] : counterClockwiseMap[orientation];
    }

    UINT ReadPhotoOrientation(IWICBitmapFrameDecode* frame, const std::wstring* diagnosticPath = nullptr) const {
#ifndef _DEBUG
        (void)diagnosticPath;
#endif
        ComPtr<IWICMetadataQueryReader> metadata;
        UINT orientation = 1;
#ifdef _DEBUG
        const auto trace = [&](const wchar_t* event, ULONGLONG started, HRESULT result) {
            if (diagnosticPath) TraceFilmstripThumbnailStage(event, *diagnosticPath, started, result);
        };
        const ULONGLONG readerStarted = GetTickCount64();
#endif
        const HRESULT readerResult = frame->GetMetadataQueryReader(&metadata);
#ifdef _DEBUG
        trace(L"ORIENTATION_READER_END", readerStarted, readerResult);
#endif
        if (SUCCEEDED(readerResult)) {
            for (const wchar_t* query : { L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}", L"/{ushort=274}" }) {
                PROPVARIANT value{};
                PropVariantInit(&value);
#ifdef _DEBUG
                const ULONGLONG queryStarted = GetTickCount64();
#endif
                const HRESULT result = metadata->GetMetadataByName(query, &value);
#ifdef _DEBUG
                trace(query, queryStarted, result);
#endif
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
        imageAdjustmentSourceDirty_ = true;
        imageAdjustedBitmap_.Reset();
        return S_OK;
    }

    void BeginShellRotationRefresh(bool clockwise) {
        ++decodeRequestGeneration_;
        pendingFullDecode_.reset();
        imageDecodePending_ = false;
        KillTimer(window_, kNavigationDecodeDebounceTimer);
        shellRotationPath_ = currentPath_;
        shellRotationClockwise_ = clockwise;
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
        RotateResidentFilmstripThumbnail(shellRotationClockwise_);
        imageDecodePending_ = true;
        pendingFullDecode_ = DecodeRequest{ currentPath_, decodeRequestGeneration_, navigationFolderGeneration_ };
        QueueLatestFullDecode();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void UpdateShellRotation() {
        if (!shellRotationPending_) { KillTimer(window_, kShellRotationCheckTimer); return; }
        WIN32_FILE_ATTRIBUTE_DATA state{};
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
        BeginShellRotationRefresh(clockwise);
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
        if (SUCCEEDED(reload)) RotateResidentFilmstripThumbnail(clockwise);
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
        filmstripThumbnailFolderGeneration_.store(navigationFolderGeneration_, std::memory_order_release);
        pendingFullDecode_.reset(); imageDecodePending_ = false;
        KillTimer(window_, kNavigationDecodeDebounceTimer);
        source_.Reset(); bitmap_.Reset(); imageWidth_ = imageHeight_ = 0;
        displayedPixels_.reset();
        currentPath_.clear(); displayedPath_.clear(); currentFileIdentity_ = {}; resolutionText_.clear(); fileSizeText_.clear(); filenameText_.clear();
        CancelQueuedFilmstripThumbnails();
        navigationFiles_.clear(); navigationBuilt_ = false; navigationBuildQueued_ = false;
        filmstripClickedRevealTarget_.reset();
        filmstripThumbnailGenerations_.clear(); filmstripThumbnails_.clear(); filmstripThumbnailPending_.clear(); filmstripThumbnailFailures_.clear();
        CancelQueuedFilmstripHoverPreviews();
        filmstripHoverPreviews_.clear();
        ++filmstripHoverPreviewGeneration_;
        videoHoverPreviewGeneration_.store(filmstripHoverPreviewGeneration_, std::memory_order_release);
        CancelFilmstripVideoHoverFade();
        filmstripHoveredIndex_ = -1;
        HideFilmstripHoverPreviewImmediately();
        filmstripItemWidths_.clear(); filmstripItemOffsets_.clear(); filmstripScroll_ = 0.0;
        filmstripDemandFirst_ = filmstripDemandLast_ = filmstripDemandCurrent_ = std::numeric_limits<size_t>::max();
        filmstripLayoutAspects_.clear(); filmstripKnownAspects_.clear(); filmstripAspectAuthoritative_.clear(); filmstripAspectRelayoutPending_.clear();
        filmstripLayoutRebuildPending_ = false;
        StopFilmstripScrollAnimation();
        filmstripOpacity_ = 0.0f; filmstripVisibilityState_ = FilmstripVisibilityState::Hidden; StopFilmstripVisibilityTimer();
        fitToWindow_ = true; zoom_ = 1.0f; pan_ = D2D1::Point2F();
        error_ = L"Drop an image here, or launch Viewtrious with an image path.";
        InvalidateRect(window_, nullptr, FALSE);
    }

    void ShowImageAfterDelete(std::vector<fs::path> filesBeforeDelete, const fs::path& deleted) {
        StopGifPlayback();
        auto current = std::find_if(filesBeforeDelete.begin(), filesBeforeDelete.end(),
            [&deleted](const fs::path& path) { return PathsEqual(path, deleted); });
        if (current == filesBeforeDelete.end()) { ClearDeletedImage(); return; }
        const size_t index = static_cast<size_t>(std::distance(filesBeforeDelete.begin(), current));
        const std::vector<fs::path> previousNavigationFiles = filesBeforeDelete;
        filesBeforeDelete.erase(current);
        CancelQueuedFilmstripThumbnails();
        navigationFiles_ = std::move(filesBeforeDelete);
        ++navigationFolderGeneration_;
        filmstripThumbnailFolderGeneration_.store(navigationFolderGeneration_, std::memory_order_release);
        filmstripThumbnailGenerations_.assign(navigationFiles_.size(), ++filmstripThumbnailGenerationSeed_);
        RemapFilmstripAspectMetadata(previousNavigationFiles);
        filmstripThumbnails_.clear();
        filmstripThumbnailFailures_.clear();
        navigationBuilt_ = true;
        for (size_t offset = 0; offset < navigationFiles_.size(); ++offset) {
            const size_t candidate = (index + offset) % navigationFiles_.size();
            if (IsGifPath(navigationFiles_[candidate].wstring())) {
                if (SUCCEEDED(LoadAnimatedGif(navigationFiles_[candidate].wstring(), false))) {
                    InvalidateRect(window_, nullptr, FALSE);
                    return;
                }
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
        BuildNavigation(true);
        if (currentPath_.empty()) return;
        const fs::path deleted(currentPath_);
        const std::vector<fs::path> filesBeforeDelete = navigationFiles_;
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
        if (!aborted) ShowImageAfterDelete(filesBeforeDelete, deleted);
    }

    RECT GetDropdownBounds() const {
        RECT client{};
        GetClientRect(window_, &client);
        const UINT dpi = GetDpiForWindow(window_);
        const FrameMetrics frame = GetFrameMetrics(window_);
        const LONG margin = MulDiv(4, dpi, 96);
        const LONG width = std::min<LONG>(MulDiv(236, dpi, 96), std::max<LONG>(1, client.right - margin * 2));
        const LONG rowHeight = MulDiv(38, dpi, 96);
        const LONG separatorGap = MulDiv(9, dpi, 96);
        const LONG panelPadding = MulDiv(4, dpi, 96);
        const LONG height = panelPadding * 2 + rowHeight * 8 + separatorGap * 3;
        const LONG left = std::clamp<LONG>(frame.hamburger.left + margin, margin,
            std::max<LONG>(margin, client.right - width - margin));
        const LONG top = std::min<LONG>(frame.hamburger.bottom + margin,
            std::max<LONG>(margin, client.bottom - margin - height));
        return { left, top, left + width, top + height };
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
            source_ = bitmap; bitmap_.Reset(); imageWidth_ = gifCanvasWidth_; imageHeight_ = gifCanvasHeight_; imageAdjustmentSourceDirty_ = true; imageAdjustedBitmap_.Reset();
        }
        displayedPixels_ = gifCanvas_;
        imageHasTransparency_ = PixelsHaveTransparency(*gifCanvas_);
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
        SuppressFilmstripHoverPreviewForCurrentMedia();
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
        if (SUCCEEDED(hr)) {
            decoded.width = width;
            decoded.height = height;
            decoded.stride = stride;
            decoded.hasTransparency = PixelsHaveTransparency(*pixels);
            decoded.pixels = std::move(pixels);
        }
        return hr;
    }

    bool SourceHasTransparency(const ComPtr<IWICBitmapSource>& source, UINT width, UINT height) const {
        if (!source || width == 0 || height == 0 || width > UINT_MAX / 4) return false;
        ComPtr<IWICBitmap> bitmap;
        if (FAILED(source.As(&bitmap))) return false;
        ComPtr<IWICBitmapLock> lock;
        if (FAILED(bitmap->Lock(nullptr, WICBitmapLockRead, &lock))) return false;
        UINT stride = 0;
        UINT bufferSize = 0;
        BYTE* pixels = nullptr;
        if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&bufferSize, &pixels)) ||
            !pixels || stride < width * 4 || bufferSize < static_cast<size_t>(stride) * height) return false;
        for (UINT y = 0; y < height; ++y) {
            const BYTE* row = pixels + static_cast<size_t>(y) * stride;
            for (UINT x = 0; x < width; ++x) {
                if (row[x * 4 + 3] != 255) return true;
            }
        }
        return false;
    }

    bool ApplyFilmstripThumbnailOrientation(PixelBuffer& pixels, UINT orientation) const {
        if (orientation < 1 || orientation > 8) orientation = 1;
        if (orientation == 1) return true;
        if (!pixels.pixels || !pixels.width || !pixels.height || pixels.stride < pixels.width * 4) return false;
        const bool swapsAxes = orientation >= 5 && orientation <= 8;
        const UINT destinationWidth = swapsAxes ? pixels.height : pixels.width;
        const UINT destinationHeight = swapsAxes ? pixels.width : pixels.height;
        if (destinationWidth > UINT_MAX / 4 || destinationHeight > UINT_MAX / (destinationWidth * 4)) return false;
        const UINT destinationStride = destinationWidth * 4;
        auto transformed = std::make_shared<std::vector<BYTE>>(static_cast<size_t>(destinationStride) * destinationHeight);
        for (UINT y = 0; y < destinationHeight; ++y) for (UINT x = 0; x < destinationWidth; ++x) {
            UINT sourceX = x, sourceY = y;
            switch (orientation) {
            case 2: sourceX = pixels.width - 1 - x; break;
            case 3: sourceX = pixels.width - 1 - x; sourceY = pixels.height - 1 - y; break;
            case 4: sourceY = pixels.height - 1 - y; break;
            case 5: sourceX = y; sourceY = x; break;
            case 6: sourceX = y; sourceY = pixels.height - 1 - x; break;
            case 7: sourceX = pixels.width - 1 - y; sourceY = pixels.height - 1 - x; break;
            case 8: sourceX = pixels.width - 1 - y; sourceY = x; break;
            }
            std::memcpy(transformed->data() + static_cast<size_t>(y) * destinationStride + x * 4,
                pixels.pixels->data() + static_cast<size_t>(sourceY) * pixels.stride + sourceX * 4, 4);
        }
        pixels.width = destinationWidth;
        pixels.height = destinationHeight;
        pixels.stride = destinationStride;
        pixels.pixels = std::move(transformed);
        return true;
    }

    void RotateResidentFilmstripThumbnail(bool clockwise) {
        if (filmstripThumbnailGenerations_.size() != navigationFiles_.size()) return;
        const fs::path current(currentPath_);
        const auto item = std::find_if(navigationFiles_.begin(), navigationFiles_.end(), [&current](const fs::path& path) {
            return PathsEqual(path, current);
        });
        if (item == navigationFiles_.end()) return;
        const size_t index = static_cast<size_t>(std::distance(navigationFiles_.begin(), item));
#ifdef _DEBUG
        const ULONGLONG started = GetTickCount64();
#endif
        const int thumbnail = FindFilmstripThumbnail(currentPath_, filmstripThumbnailGenerations_[index]);
        if (thumbnail < 0) {
#ifdef _DEBUG
            TraceFilmstripThumbnailStage(L"THUMB_RAM_LIVE_ROTATE_SKIPPED_NOT_RESIDENT", currentPath_, started, S_FALSE);
#endif
            return;
        }
        FilmstripThumbnailEntry& entry = filmstripThumbnails_[thumbnail];
#ifdef _DEBUG
        TraceFilmstripThumbnailStage(L"THUMB_RAM_LIVE_ROTATE_BEGIN", entry.path, started, S_OK);
#endif
        if (!ApplyFilmstripThumbnailOrientation(entry, clockwise ? 6u : 8u)) {
#ifdef _DEBUG
            TraceFilmstripThumbnailStage(L"THUMB_RAM_LIVE_ROTATE_END", entry.path, started, E_FAIL);
#endif
            return;
        }
        entry.aspect = static_cast<float>(entry.width) / static_cast<float>(entry.height);
        entry.bitmap.Reset();
        // This is a local cache/layout update. It must not create thumbnail worker demand.
        if (UpdateFilmstripKnownAspect(index, entry.aspect)) ApplyFilmstripAspectRelayout(false);
#ifdef _DEBUG
        TraceFilmstripThumbnailStage(L"THUMB_RAM_LIVE_ROTATE_END", entry.path, started, S_OK);
#endif
        InvalidateRect(window_, nullptr, FALSE);
    }

    HRESULT DecodeFilmstripThumbnailPixels(const std::wstring& path, UINT targetHeight, PixelBuffer& decoded, float& aspect) const {
        if (targetHeight == 0) return E_INVALIDARG;
        HRESULT hr = E_FAIL;
        UINT orientation = 1;
#ifdef _DEBUG
        const ULONGLONG decodeStarted = GetTickCount64();
        const ULONGLONG decoderStarted = GetTickCount64();
#endif
        // All WIC objects stay inside this scope. Once it returns, the result contains only
        // Viewtrious-owned PBGRA bytes and has no source-file ownership.
        {
            ComPtr<IWICImagingFactory> factory;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
#ifdef _DEBUG
            TraceFilmstripThumbnailStage(L"DECODER_CREATE_END", path, decoderStarted, hr);
            const ULONGLONG frameStarted = GetTickCount64();
#endif
            if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
#ifdef _DEBUG
            TraceFilmstripThumbnailStage(L"GET_FRAME_END", path, frameStarted, hr);
#endif
            if (FAILED(hr)) return hr;
#ifdef _DEBUG
            const ULONGLONG orientationStarted = GetTickCount64();
#endif
            orientation = ReadPhotoOrientation(frame.Get(), &path);
#ifdef _DEBUG
            TraceFilmstripThumbnailStage(L"ORIENTATION_END", path, orientationStarted, S_OK);
#endif
            const auto decodeSource = [&](IWICBitmapSource* source) -> HRESULT {
                if (!source) return E_FAIL;
                UINT sourceWidth = 0, sourceHeight = 0;
                HRESULT attempt = source->GetSize(&sourceWidth, &sourceHeight);
                ComPtr<IWICFormatConverter> converter;
#ifdef _DEBUG
                const ULONGLONG converterStarted = GetTickCount64();
#endif
                if (SUCCEEDED(attempt)) attempt = factory->CreateFormatConverter(&converter);
                if (SUCCEEDED(attempt)) attempt = converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
#ifdef _DEBUG
                TraceFilmstripThumbnailStage(L"CONVERTER_INIT_END", path, converterStarted, attempt);
#endif
                // Do not place the EXIF transform in this source-backed WIC chain: metadata-rotated
                // JPEGs can defer an expensive transform until CopyPixels. Apply it after release instead.
                ComPtr<IWICBitmapSource> transformed = converter;
                if (FAILED(attempt) || !sourceWidth || !sourceHeight) return FAILED(attempt) ? attempt : E_FAIL;
#ifdef _DEBUG
                const ULONGLONG cropStarted = GetTickCount64();
#endif
                const bool swapsAxes = orientation >= 5 && orientation <= 8;
                const UINT orientedWidth = swapsAxes ? sourceHeight : sourceWidth;
                const UINT orientedHeight = swapsAxes ? sourceWidth : sourceHeight;
                const float naturalAspect = static_cast<float>(orientedWidth) / static_cast<float>(orientedHeight);
                const float croppedAspect = std::clamp(naturalAspect, 2.0f / 3.0f, 16.0f / 9.0f);
                UINT orientedCropWidth = orientedWidth, orientedCropHeight = orientedHeight;
                if (naturalAspect > croppedAspect) orientedCropWidth = std::max(1u, static_cast<UINT>(std::lround(orientedHeight * croppedAspect)));
                else if (naturalAspect < croppedAspect) orientedCropHeight = std::max(1u, static_cast<UINT>(std::lround(orientedWidth / croppedAspect)));
                const UINT cropWidth = swapsAxes ? orientedCropHeight : orientedCropWidth;
                const UINT cropHeight = swapsAxes ? orientedCropWidth : orientedCropHeight;
                const WICRect crop{ static_cast<INT>((sourceWidth - cropWidth) / 2), static_cast<INT>((sourceHeight - cropHeight) / 2),
                    static_cast<INT>(cropWidth), static_cast<INT>(cropHeight) };
#ifdef _DEBUG
                TraceFilmstripThumbnailStage(L"CROP_CALCULATION_END", path, cropStarted, S_OK);
#endif
                ComPtr<IWICBitmapClipper> clipper;
#ifdef _DEBUG
                const ULONGLONG clipperStarted = GetTickCount64();
#endif
                if (SUCCEEDED(attempt)) attempt = factory->CreateBitmapClipper(&clipper);
                if (SUCCEEDED(attempt)) attempt = clipper->Initialize(transformed.Get(), &crop);
#ifdef _DEBUG
                TraceFilmstripThumbnailStage(L"CLIPPER_INIT_END", path, clipperStarted, attempt);
#endif
                const UINT orientedTargetWidth = std::max(1u, static_cast<UINT>(std::lround(targetHeight * croppedAspect)));
                const UINT sourceTargetWidth = swapsAxes ? targetHeight : orientedTargetWidth;
                const UINT sourceTargetHeight = swapsAxes ? orientedTargetWidth : targetHeight;
                ComPtr<IWICBitmapScaler> scaler;
#ifdef _DEBUG
                const ULONGLONG scalerStarted = GetTickCount64();
#endif
                if (SUCCEEDED(attempt)) attempt = factory->CreateBitmapScaler(&scaler);
                if (SUCCEEDED(attempt)) attempt = scaler->Initialize(clipper.Get(), sourceTargetWidth, sourceTargetHeight, WICBitmapInterpolationModeFant);
#ifdef _DEBUG
                TraceFilmstripThumbnailStage(L"SCALER_INIT_END", path, scalerStarted, attempt);
#endif
                ComPtr<IWICFormatConverter> finalConverter;
#ifdef _DEBUG
                const ULONGLONG finalConverterStarted = GetTickCount64();
#endif
                if (SUCCEEDED(attempt)) attempt = factory->CreateFormatConverter(&finalConverter);
                if (SUCCEEDED(attempt)) attempt = finalConverter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
#ifdef _DEBUG
                TraceFilmstripThumbnailStage(L"FINAL_CONVERTER_INIT_END", path, finalConverterStarted, attempt);
#endif
                if (FAILED(attempt) || sourceTargetWidth > UINT_MAX / 4 || sourceTargetHeight > UINT_MAX / (sourceTargetWidth * 4)) return FAILED(attempt) ? attempt : E_OUTOFMEMORY;
                const UINT stride = sourceTargetWidth * 4;
                const size_t bytes = static_cast<size_t>(stride) * sourceTargetHeight;
                auto pixels = std::make_shared<std::vector<BYTE>>(bytes);
#ifdef _DEBUG
                const ULONGLONG copyStarted = GetTickCount64();
#endif
                attempt = finalConverter->CopyPixels(nullptr, stride, static_cast<UINT>(bytes), pixels->data());
#ifdef _DEBUG
                TraceFilmstripThumbnailStage(L"COPYPIXELS_END", path, copyStarted, attempt);
#endif
                if (SUCCEEDED(attempt)) {
                    decoded.width = sourceTargetWidth;
                    decoded.height = sourceTargetHeight;
                    decoded.stride = stride;
                    decoded.pixels = std::move(pixels);
                }
                return attempt;
            };
#ifdef _DEBUG
            const ULONGLONG scaleStarted = GetTickCount64();
#endif
            hr = decodeSource(frame.Get());
#ifdef _DEBUG
            TraceFilmstripThumbnailStage(L"SCALE_END", path, scaleStarted, hr);
#endif
        }
#ifdef _DEBUG
        TraceFilmstripThumbnailStage(L"SOURCE_RELEASED", path, decodeStarted, hr);
#endif
        if (SUCCEEDED(hr)) {
#ifdef _DEBUG
            const ULONGLONG ramOrientationStarted = GetTickCount64();
            TraceFilmstripThumbnailStage(L"RAM_ORIENTATION_BEGIN", path, ramOrientationStarted, S_OK);
#endif
            if (!ApplyFilmstripThumbnailOrientation(decoded, orientation)) return E_FAIL;
#ifdef _DEBUG
            TraceFilmstripThumbnailStage(L"RAM_ORIENTATION_END", path, ramOrientationStarted, S_OK);
#endif
            aspect = static_cast<float>(decoded.width) / static_cast<float>(decoded.height);
        }
        return hr;
    }

    HRESULT DecodeFilmstripHoverPreviewPixels(const std::wstring& path, PixelBuffer& decoded, float& aspect) const {
        constexpr UINT kMaximumPreviewDimension = 1024;
        HRESULT hr = E_FAIL;
        UINT orientation = 1;
#ifdef _DEBUG
        const ULONGLONG started = GetTickCount64();
#endif
        // Keep every WIC object inside this scope. The published result is copied PBGRA RAM only.
        {
            ComPtr<IWICImagingFactory> factory;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
            if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
            if (FAILED(hr)) return hr;
            orientation = ReadPhotoOrientation(frame.Get(), &path);
            UINT sourceWidth = 0, sourceHeight = 0;
            hr = frame->GetSize(&sourceWidth, &sourceHeight);
            if (FAILED(hr) || !sourceWidth || !sourceHeight) return FAILED(hr) ? hr : E_FAIL;
#ifdef _DEBUG
            wchar_t dimensions[512]{};
            swprintf_s(dimensions, L"[Viewtrious] FILMSTRIP_HD_PREVIEW_SOURCE size=%ux%u path=%ls\n", sourceWidth, sourceHeight, path.c_str());
            OutputDebugStringW(dimensions);
#endif
            const bool swapsAxes = orientation >= 5 && orientation <= 8;
            const UINT orientedWidth = swapsAxes ? sourceHeight : sourceWidth;
            const UINT orientedHeight = swapsAxes ? sourceWidth : sourceHeight;
            const float scale = std::min(1.0f, static_cast<float>(kMaximumPreviewDimension) / std::max(orientedWidth, orientedHeight));
            const UINT targetOrientedWidth = std::max(1u, static_cast<UINT>(std::lround(orientedWidth * scale)));
            const UINT targetOrientedHeight = std::max(1u, static_cast<UINT>(std::lround(orientedHeight * scale)));
            const UINT targetWidth = swapsAxes ? targetOrientedHeight : targetOrientedWidth;
            const UINT targetHeight = swapsAxes ? targetOrientedWidth : targetOrientedHeight;
            ComPtr<IWICFormatConverter> converter;
            if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
            if (SUCCEEDED(hr)) hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
            ComPtr<IWICBitmapScaler> scaler;
            if (SUCCEEDED(hr)) hr = factory->CreateBitmapScaler(&scaler);
            if (SUCCEEDED(hr)) hr = scaler->Initialize(converter.Get(), targetWidth, targetHeight, WICBitmapInterpolationModeFant);
            if (FAILED(hr) || targetWidth > UINT_MAX / 4 || targetHeight > UINT_MAX / (targetWidth * 4)) return FAILED(hr) ? hr : E_OUTOFMEMORY;
            const UINT stride = targetWidth * 4;
            const size_t bytes = static_cast<size_t>(stride) * targetHeight;
            auto pixels = std::make_shared<std::vector<BYTE>>(bytes);
            hr = scaler->CopyPixels(nullptr, stride, static_cast<UINT>(bytes), pixels->data());
#ifdef _DEBUG
            TraceFilmstripThumbnailStage(L"FILMSTRIP_HD_PREVIEW_COPYPIXELS_END", path, started, hr);
#endif
            if (SUCCEEDED(hr)) {
                decoded.width = targetWidth;
                decoded.height = targetHeight;
                decoded.stride = stride;
                decoded.pixels = std::move(pixels);
            }
        }
#ifdef _DEBUG
        TraceFilmstripThumbnailStage(L"FILMSTRIP_HD_PREVIEW_SOURCE_RELEASED", path, started, hr);
#endif
        if (SUCCEEDED(hr)) {
            if (!ApplyFilmstripThumbnailOrientation(decoded, orientation)) return E_FAIL;
            aspect = static_cast<float>(decoded.width) / std::max(1u, decoded.height);
        }
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
        const D2D1_RECT_F canvasBounds = ImageCanvasBounds();
        const D2D1_POINT_2F imageTopLeft = ImageTopLeft(CurrentScale(), canvas);
        const UINT fullWidth = std::max(1u, static_cast<UINT>(std::lround(imageWidth_ * physicalScale)));
        const UINT fullHeight = std::max(1u, static_cast<UINT>(std::lround(imageHeight_ * physicalScale)));
        const int visibleLeft = std::clamp(static_cast<int>(std::floor((canvasBounds.left - imageTopLeft.x) * dpiScale)), 0, static_cast<int>(fullWidth));
        const int visibleTop = std::clamp(static_cast<int>(std::floor((canvasBounds.top - imageTopLeft.y) * dpiScale)), 0, static_cast<int>(fullHeight));
        const int visibleRight = std::clamp(static_cast<int>(std::ceil((canvasBounds.right - imageTopLeft.x) * dpiScale)), 0, static_cast<int>(fullWidth));
        const int visibleBottom = std::clamp(static_cast<int>(std::ceil((canvasBounds.bottom - imageTopLeft.y) * dpiScale)), 0, static_cast<int>(fullHeight));
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
        SuppressFilmstripHoverPreviewForCurrentMedia();
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
                    CommitImage(result->request.path, bitmap, result->width, result->height, false, result->hasTransparency);
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
        bool resetNavigation, std::optional<bool> knownTransparency = std::nullopt) {
        FlushImageAdjustmentPersistence();
        if (!committingGifFrame_) StopGifPlayback();
        InvalidateLanczosVariant(false);
        displayedPixels_.reset();
        source_ = source;
        imageHasTransparency_ = knownTransparency.value_or(SourceHasTransparency(source, width, height));
        bitmap_.Reset();
        imageAdjustmentSourceDirty_ = true;
        imageAdjustedBitmap_.Reset();
        imageAdjustments_ = {};
        imageAdjustmentHashResolved_ = false;
        ++imageAdjustmentMediaGeneration_;
        imageWidth_ = width;
        imageHeight_ = height;
        currentPath_ = path;
        SuppressFilmstripHoverPreviewForCurrentMedia();
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
            CancelQueuedFilmstripThumbnails();
            navigationFiles_.clear();
            navigationBuilt_ = false;
            navigationBuildQueued_ = false;
            filmstripItemWidths_.clear();
            filmstripItemOffsets_.clear();
            filmstripLayoutAspects_.clear();
            filmstripKnownAspects_.clear();
            filmstripAspectAuthoritative_.clear();
            filmstripAspectRelayoutPending_.clear();
            filmstripThumbnailGenerations_.clear();
            filmstripThumbnails_.clear();
            filmstripThumbnailPending_.clear();
            filmstripThumbnailFailures_.clear();
            filmstripClickedRevealTarget_.reset();
            filmstripScroll_ = 0.0;
            filmstripLayoutRebuildPending_ = false;
            StopFilmstripScrollAnimation();
        } else if (navigationBuilt_) {
            // A video sibling can build navigation while Image2D has no source. Rebuild after
            // the image commit so stale compact placeholder widths cannot reach the first paint.
            RebuildFilmstripLayout();
            RevealClickedFilmstripItem();
        }
        BeginStillDissolveIfReady(path);
        adjustmentPersistence_.Resolve(path, imageAdjustmentMediaGeneration_, imageAdjustmentEditGeneration_);
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

    bool EnsureImageAdjustmentSource(bool useLanczos = false) {
        if (!source_ || !graphicsHost_.Device() || !graphicsHost_.Context()) return false;
        const bool canUseLanczos = useLanczos && !gifPlaying_ && !spaceMouseMotionActive_ && lanczosSelected_ && LanczosVariantMatchesCurrent();
        const UINT sourceWidth = canUseLanczos ? lanczosWidth_ : imageWidth_;
        const UINT sourceHeight = canUseLanczos ? lanczosHeight_ : imageHeight_;
        if (!sourceWidth || !sourceHeight) return false;
        if (!imageAdjustmentProcessorReady_) {
            if (!imageAdjustmentProcessor_.Initialize(graphicsHost_.Device())) return false;
            imageAdjustmentProcessorReady_ = true;
        }
        if (!imageAdjustmentSourceTexture_ || imageAdjustmentSourceWidth_ != sourceWidth || imageAdjustmentSourceHeight_ != sourceHeight) {
            imageAdjustmentSourceTexture_.Reset();
            D3D11_TEXTURE2D_DESC description{};
            description.Width = sourceWidth; description.Height = sourceHeight; description.MipLevels = 1; description.ArraySize = 1;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT; description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(graphicsHost_.Device()->CreateTexture2D(&description, nullptr, &imageAdjustmentSourceTexture_))) return false;
            imageAdjustmentSourceWidth_ = sourceWidth; imageAdjustmentSourceHeight_ = sourceHeight;
            imageAdjustmentSourceDirty_ = true;
        }
        if (!imageAdjustmentSourceDirty_ && imageAdjustmentSourceWic_ == source_.Get() && imageAdjustmentUsesLanczos_ == canUseLanczos) return true;
        const UINT stride = sourceWidth * 4;
        const size_t bytes = static_cast<size_t>(stride) * sourceHeight;
        const BYTE* pixels = nullptr;
        if (canUseLanczos && lanczosPixels_ && lanczosPixels_->size() >= bytes) pixels = lanczosPixels_->data();
        else if (!canUseLanczos && displayedPixels_ && displayedPixels_->size() >= bytes) pixels = displayedPixels_->data();
        else {
            imageAdjustmentPixels_.resize(bytes);
            if (FAILED(source_->CopyPixels(nullptr, stride, static_cast<UINT>(bytes), imageAdjustmentPixels_.data()))) return false;
            pixels = imageAdjustmentPixels_.data();
        }
        graphicsHost_.Context()->UpdateSubresource(imageAdjustmentSourceTexture_.Get(), 0, nullptr, pixels, stride, 0);
        imageAdjustmentSourceWic_ = source_.Get();
        imageAdjustmentUsesLanczos_ = canUseLanczos;
        imageAdjustmentSourceDirty_ = false;
        return true;
    }

    bool EnsureImageAdjustedBitmap() {
        if (imageAdjustments_.IsNeutral()) return false;
        const bool useLanczos = !gifPlaying_ && !spaceMouseMotionActive_ && lanczosSelected_ && LanczosVariantMatchesCurrent();
        if (imageAdjustedBitmap_ && imageAdjustmentUsesLanczos_ == useLanczos) return true;
        imageAdjustedBitmap_.Reset();
        if (!EnsureImageAdjustmentSource(useLanczos) || !imageAdjustmentProcessor_.ProcessImage(imageAdjustmentSourceTexture_.Get(), imageAdjustmentSourceWidth_, imageAdjustmentSourceHeight_, imageAdjustments_)) return false;
        ComPtr<IDXGISurface> surface;
        if (FAILED(imageAdjustmentProcessor_.OutputTexture()->QueryInterface(IID_PPV_ARGS(&surface)))) return false;
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), RenderTargetDpi(), RenderTargetDpi());
        imageAdjustedBitmap_.Reset();
        return SUCCEEDED(renderTarget_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &imageAdjustedBitmap_));
    }

    D2D1_SIZE_F ClientSize() const {
        RECT client{};
        GetClientRect(window_, &client);
        return D2D1::SizeF(static_cast<float>(std::max(1L, client.right - client.left)),
            static_cast<float>(std::max(1L, client.bottom - client.top)));
    }

    D2D1_SIZE_F ImageCanvasSize() const {
        const D2D1_RECT_F bounds = ImageCanvasBounds();
        return D2D1::SizeF(bounds.right - bounds.left, bounds.bottom - bounds.top);
    }

    D2D1_RECT_F ImageCanvasBounds() const {
        const D2D1_SIZE_F client = ClientSize();
        const float top = fullscreen_ ? 0.0f : static_cast<float>(GetFrameMetrics(window_).titleBarHeight);
        return D2D1::RectF(0.0f, top, client.width, std::max(top + 1.0f, client.height));
    }

    float BaseScale() const {
        if (!source_) return 1.0f;
        const D2D1_SIZE_F target = ImageCanvasSize();
        const float fitScale = std::min(target.width / static_cast<float>(imageWidth_),
            target.height / static_cast<float>(imageHeight_));
        return fitScale;
    }

    float MinimumScale() const { return std::min(1.0f, BaseScale()); }

    float CurrentScale() const { return fitToWindow_ ? BaseScale() : zoom_; }

    void CenterAtMinimumScale() {
        if (!source_) return;
        const float oldScale = CurrentScale();
        const D2D1_POINT_2F oldPan = pan_;
        const float minimumScale = MinimumScale();
        fitToWindow_ = std::abs(minimumScale - BaseScale()) < 0.0001f;
        zoom_ = minimumScale;
        pan_ = D2D1::Point2F();
        if (lanczosSelected_ && (std::abs(minimumScale - oldScale) >= 0.0001f ||
                std::abs(oldPan.x) >= 0.0001f || std::abs(oldPan.y) >= 0.0001f)) {
            InvalidateLanczosVariant(true);
            QueueLanczosRefinement();
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

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
        const D2D1_RECT_F canvas = ImageCanvasBounds();
        return D2D1::Point2F(canvas.left + (target.width - imageWidth_ * scale) / 2.0f + pan_.x,
            canvas.top + (target.height - imageHeight_ * scale) / 2.0f + pan_.y);
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
        if (VideoActive()) return VideoCurrentScale() > VideoFitScale() + 0.0001f;
        return source_ && CurrentScale() > BaseScale() + 0.0001f;
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
        const D2D1_RECT_F canvas = ImageCanvasBounds();
        const D2D1_RECT_F visible = D2D1::RectF(std::max(bounds.left, canvas.left), std::max(bounds.top, canvas.top),
            std::min(bounds.right, canvas.right), std::min(bounds.bottom, canvas.bottom));
        if (visible.right <= visible.left || visible.bottom <= visible.top) return;
        renderTarget_->PushAxisAlignedClip(visible, D2D1_ANTIALIAS_MODE_ALIASED);
        renderTarget_->FillRectangle(visible, checkerboardBrush_.Get());
        renderTarget_->PopAxisAlignedClip();
    }
    void DrawImage() {
        const D2D1_SIZE_F target = ImageCanvasSize();
        const D2D1_RECT_F canvas = ImageCanvasBounds();
        const float scale = CurrentScale();
        const D2D1_POINT_2F topLeft = ImageTopLeft(scale, target);
        const D2D1_RECT_F destination = D2D1::RectF(topLeft.x, topLeft.y,
            topLeft.x + imageWidth_ * scale, topLeft.y + imageHeight_ * scale);
        renderTarget_->PushAxisAlignedClip(canvas, D2D1_ANTIALIAS_MODE_ALIASED);
        if (imageHasTransparency_) DrawCheckerboard(destination);
        if (imageAdjustments_.IsNeutral() && !gifPlaying_ && !spaceMouseMotionActive_ && lanczosSelected_ && LanczosVariantMatchesCurrent() && EnsureLanczosBitmap())
            renderTarget_->DrawBitmap(lanczosBitmap_.Get(), lanczosDestination_, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        else {
            ID2D1Bitmap* displayed = bitmap_.Get();
            D2D1_RECT_F adjustedDestination = destination;
            if (!imageAdjustments_.IsNeutral() && EnsureImageAdjustedBitmap()) { displayed = imageAdjustedBitmap_.Get(); if (imageAdjustmentUsesLanczos_) adjustedDestination = lanczosDestination_; }
            renderTarget_->DrawBitmap(displayed, adjustedDestination, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        renderTarget_->PopAxisAlignedClip();
    }

    void DrawStillDissolve() {
        if (!dissolveActive_ || !dissolveOldBitmap_ || !dissolveOldWidth_ || !dissolveOldHeight_) { DrawImage(); return; }
        const D2D1_SIZE_F target = ImageCanvasSize();
        const D2D1_RECT_F canvas = ImageCanvasBounds();
        const float oldScale = std::min(target.width / dissolveOldWidth_, target.height / dissolveOldHeight_);
        const float newScale = CurrentScale();
        const D2D1_POINT_2F oldTopLeft = D2D1::Point2F(canvas.left + (target.width - dissolveOldWidth_ * oldScale) * 0.5f,
            canvas.top + (target.height - dissolveOldHeight_ * oldScale) * 0.5f);
        const D2D1_POINT_2F newTopLeft = ImageTopLeft(newScale, target);
        const D2D1_RECT_F oldDestination = D2D1::RectF(oldTopLeft.x, oldTopLeft.y, oldTopLeft.x + dissolveOldWidth_ * oldScale, oldTopLeft.y + dissolveOldHeight_ * oldScale);
        const D2D1_RECT_F newDestination = D2D1::RectF(newTopLeft.x, newTopLeft.y, newTopLeft.x + imageWidth_ * newScale, newTopLeft.y + imageHeight_ * newScale);
        const float progress = StillDissolveProgress();
        const float eased = SmoothTransitionProgress(progress);

        renderTarget_->PushAxisAlignedClip(canvas, D2D1_ANTIALIAS_MODE_ALIASED);
        ID2D1Bitmap* displayed = bitmap_.Get();
        D2D1_RECT_F adjustedDestination = newDestination;
        if (!imageAdjustments_.IsNeutral() && EnsureImageAdjustedBitmap()) {
            displayed = imageAdjustedBitmap_.Get();
            if (imageAdjustmentUsesLanczos_) adjustedDestination = lanczosDestination_;
        }
        renderTarget_->DrawBitmap(dissolveOldBitmap_.Get(), oldDestination, 1.0f - eased, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        if (displayed) renderTarget_->DrawBitmap(displayed, adjustedDestination, eased, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        renderTarget_->PopAxisAlignedClip();
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
        if (!source_) return;
        const ImageZoomHudLayout hud = GetImageZoomHudLayout();
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        ComPtr<ID2D1SolidColorBrush> backing, text, hover;
        if (FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.50f), &backing)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.62f), &text)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f), &hover))) return;
        const auto rect = [](const RECT& value) { return D2D1::RectF(static_cast<float>(value.left), static_cast<float>(value.top), static_cast<float>(value.right), static_cast<float>(value.bottom)); };
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect(hud.combined), 6.0f * scale, 6.0f * scale), backing.Get());
        if (hoveredButton_ == ButtonKind::ImageAdjustments || imageAdjustmentsPanelOpen_)
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect(hud.adjustments), 5.0f * scale, 5.0f * scale), hover.Get());
        const float centerX = (hud.adjustments.left + hud.adjustments.right) * 0.5f;
        const float centerY = (hud.adjustments.top + hud.adjustments.bottom) * 0.5f;
        for (int index = -1; index <= 1; ++index) {
            const float x = centerX + index * 4.5f * scale;
            renderTarget_->DrawLine(D2D1::Point2F(x, centerY - 6.0f * scale), D2D1::Point2F(x, centerY + 6.0f * scale), text.Get(), 1.15f * scale);
            renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, centerY + (index == 0 ? 2.5f : -2.5f) * scale), 1.9f * scale, 1.9f * scale), text.Get());
        }
        if (hud.hasZoom && EnsureZoomHudFormat()) {
            wchar_t label[16]{};
            const float percent = PhysicalPixelScale() * 100.0f;
            if (percent < 10.0f) swprintf_s(label, L"%.1f%%", percent); else swprintf_s(label, L"%.0f%%", percent);
            ComPtr<IDWriteTextLayout> layout;
            const float width = static_cast<float>(hud.zoom.right - hud.zoom.left), height = static_cast<float>(hud.zoom.bottom - hud.zoom.top);
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(label, static_cast<UINT32>(wcslen(label)), zoomHudFormat_.Get(), width, height, &layout)))
                renderTarget_->DrawTextLayout(D2D1::Point2F(static_cast<float>(hud.zoom.left), static_cast<float>(hud.zoom.top)), layout.Get(), text.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (hoveredButton_ == ButtonKind::ImageAdjustments && !imageAdjustmentsPanelOpen_) {
            const float width = 78.0f * scale, height = 24.0f * scale;
            const bool top = zoomHudPosition_ == ZoomHudPosition::TopLeft || zoomHudPosition_ == ZoomHudPosition::TopRight;
            const float left = centerX - width * 0.5f;
            const float topEdge = top ? static_cast<float>(hud.combined.bottom) + 6.0f * scale : static_cast<float>(hud.combined.top) - height - 6.0f * scale;
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, topEdge, left + width, topEdge + height), 5.0f * scale, 5.0f * scale), backing.Get());
            DrawOverlayText(L"adjustments", left, topEdge, width, height, 10.5f, DWRITE_FONT_WEIGHT_NORMAL, text.Get(), true, false, true);
        }
        if (imageAdjustmentsPanelOpen_) DrawImageAdjustmentsPanel();
    }

    void DrawImageAdjustmentsPanel() {
        const ImageAdjustmentsPanelLayout panel = GetImageAdjustmentsPanelLayout();
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        const bool dark = UseDarkAppMode();
        ComPtr<ID2D1SolidColorBrush> surface, border, text, accent, track, hover;
        if (FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(dark ? 35.0f / 255.0f : 246.0f / 255.0f, dark ? 38.0f / 255.0f : 246.0f / 255.0f, dark ? 45.0f / 255.0f : 246.0f / 255.0f, 0.94f), &surface)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(dark ? 78.0f / 255.0f : 180.0f / 255.0f, dark ? 82.0f / 255.0f : 180.0f / 255.0f, dark ? 92.0f / 255.0f : 180.0f / 255.0f, 0.55f), &border)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(dark ? 242.0f / 255.0f : 35.0f / 255.0f, dark ? 242.0f / 255.0f : 35.0f / 255.0f, dark ? 242.0f / 255.0f : 35.0f / 255.0f, 1.0f), &text)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 120.0f / 255.0f, 212.0f / 255.0f, 1.0f), &accent)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(dark ? 100.0f / 255.0f : 170.0f / 255.0f, dark ? 104.0f / 255.0f : 170.0f / 255.0f, dark ? 114.0f / 255.0f : 170.0f / 255.0f, 0.75f), &track)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(dark ? 66.0f / 255.0f : 224.0f / 255.0f, dark ? 70.0f / 255.0f : 224.0f / 255.0f, dark ? 80.0f / 255.0f : 224.0f / 255.0f, 1.0f), &hover))) return;
        const auto rect = [](const RECT& value) { return D2D1::RectF(static_cast<float>(value.left), static_cast<float>(value.top), static_cast<float>(value.right), static_cast<float>(value.bottom)); };
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect(panel.panel), 10.0f * scale, 10.0f * scale), surface.Get());
        renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(rect(panel.panel), 10.0f * scale, 10.0f * scale), border.Get(), scale);
        const std::array<const wchar_t*, 6> labels{ L"exposure", L"brightness", L"contrast", L"shadows", L"highlights", L"saturation" };
        const std::array<float, 6> values{ imageAdjustments_.exposure, imageAdjustments_.brightness, imageAdjustments_.contrast, imageAdjustments_.shadows, imageAdjustments_.highlights, imageAdjustments_.saturation };
        for (size_t index = 0; index < panel.sliders.size(); ++index) {
            const RECT slider = panel.sliders[index];
            DrawOverlayText(labels[index], static_cast<float>(panel.panel.left + MulDiv(12, GetDpiForWindow(window_), 96)), static_cast<float>(slider.top), static_cast<float>(slider.left - panel.panel.left - MulDiv(18, GetDpiForWindow(window_), 96)), static_cast<float>(slider.bottom - slider.top), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, text.Get(), false, false, true);
            const float centerY = (slider.top + slider.bottom) * 0.5f;
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(static_cast<float>(slider.left), centerY - 2.0f * scale, static_cast<float>(slider.right), centerY + 2.0f * scale), 2.0f * scale, 2.0f * scale), track.Get());
            const float normalizedValue = index == 0 ? (values[index] + 2.0f) * 0.25f : (values[index] + 1.0f) * 0.5f;
            const float thumbX = slider.left + (slider.right - slider.left) * normalizedValue;
            renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, centerY), 5.0f * scale, 5.0f * scale), accent.Get());
            const std::wstring value = std::to_wstring(static_cast<int>(std::lround(values[index] * 100.0f)));
            DrawOverlayText(value.c_str(), static_cast<float>(slider.right + MulDiv(8, GetDpiForWindow(window_), 96)), static_cast<float>(slider.top), static_cast<float>(panel.panel.right - slider.right - MulDiv(8, GetDpiForWindow(window_), 96)), static_cast<float>(slider.bottom - slider.top), 11.0f, DWRITE_FONT_WEIGHT_NORMAL, text.Get(), true, false, true);
        }
        const auto drawButton = [&](const RECT& bounds, const wchar_t* label) { renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect(bounds), 5.0f * scale, 5.0f * scale), hover.Get()); DrawOverlayText(label, static_cast<float>(bounds.left), static_cast<float>(bounds.top), static_cast<float>(bounds.right - bounds.left), static_cast<float>(bounds.bottom - bounds.top), 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, text.Get(), true, false, true); };
        if (aiAddon_.Available()) drawButton(panel.autoButton, aiAnalysisRunning_ ? L"AI..." : L"AI Auto"); drawButton(panel.resetButton, L"reset");
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
        if (videoPlaybackSpeedPanelOpen_) {
            const VideoPlaybackSpeedPanelLayout panel = GetVideoPlaybackSpeedPanelLayout();
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect(panel.panel), 10.0f * scale, 10.0f * scale), surface.Get());
            renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(rect(panel.panel), 10.0f * scale, 10.0f * scale), border.Get(), 1.0f * scale);
            for (size_t index = 0; index < panel.rates.size(); ++index) {
                const double rate = PlaybackRateFromPercent(kVideoPlaybackRatePercents[index]);
                const bool selected = std::abs(rate - videoEffectivePlaybackRate_) < 0.001;
                const bool supported = videoPlayer_.PlaybackRateSupported(rate);
                if (selected) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect(panel.rates[index]), 5.0f * scale, 5.0f * scale), hover.Get());
                const std::wstring label = FormatPlaybackRate(rate);
                DrawOverlayText(label.c_str(), static_cast<float>(panel.rates[index].left), static_cast<float>(panel.rates[index].top), static_cast<float>(panel.rates[index].right - panel.rates[index].left), static_cast<float>(panel.rates[index].bottom - panel.rates[index].top), 12.0f, selected ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, supported ? text.Get() : border.Get(), true, false, true);
            }
        }
        if (videoAdjustmentsPanelOpen_) {
            const VideoAdjustmentsPanelLayout panel = GetVideoAdjustmentsPanelLayout();
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect(panel.panel), 10.0f * scale, 10.0f * scale), surface.Get());
            renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(rect(panel.panel), 10.0f * scale, 10.0f * scale), border.Get(), 1.0f * scale);
            const std::array<const wchar_t*, 4> labels{ L"brightness", L"contrast", L"shadows", L"highlights" };
            const std::array<float, 4> values{ videoAdjustments_.brightness, videoAdjustments_.contrast, videoAdjustments_.shadows, videoAdjustments_.highlights };
            for (size_t index = 0; index < panel.sliders.size(); ++index) {
                const RECT slider = panel.sliders[index];
                DrawOverlayText(labels[index], static_cast<float>(panel.panel.left + MulDiv(12, GetDpiForWindow(window_), 96)), static_cast<float>(slider.top), static_cast<float>(slider.left - panel.panel.left - MulDiv(18, GetDpiForWindow(window_), 96)), static_cast<float>(slider.bottom - slider.top), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, text.Get(), false, false, true);
                const float centerY = (slider.top + slider.bottom) * 0.5f;
                renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(static_cast<float>(slider.left), centerY - 2.0f * scale, static_cast<float>(slider.right), centerY + 2.0f * scale), 2.0f * scale, 2.0f * scale), track.Get());
                const float centerX = slider.left + (slider.right - slider.left) * 0.5f;
                renderTarget_->DrawLine(D2D1::Point2F(centerX, centerY - 5.0f * scale), D2D1::Point2F(centerX, centerY + 5.0f * scale), border.Get(), scale);
                const float thumbX = slider.left + (slider.right - slider.left) * (values[index] + 1.0f) * 0.5f;
                renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, centerY), 5.0f * scale, 5.0f * scale), accent.Get());
                const std::wstring value = std::to_wstring(static_cast<int>(std::lround(values[index] * 100.0f)));
                DrawOverlayText(value.c_str(), static_cast<float>(slider.right + MulDiv(8, GetDpiForWindow(window_), 96)), static_cast<float>(slider.top), static_cast<float>(panel.panel.right - slider.right - MulDiv(8, GetDpiForWindow(window_), 96)), static_cast<float>(slider.bottom - slider.top), 11.0f, DWRITE_FONT_WEIGHT_NORMAL, text.Get(), true, false, true);
            }
            const auto drawPanelButton = [&](const RECT& bounds, const wchar_t* label) {
                renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect(bounds), 5.0f * scale, 5.0f * scale), hover.Get());
                DrawOverlayText(label, static_cast<float>(bounds.left), static_cast<float>(bounds.top), static_cast<float>(bounds.right - bounds.left), static_cast<float>(bounds.bottom - bounds.top), 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, text.Get(), true, false, true);
            };
            if (aiAddon_.Available()) drawPanelButton(panel.autoButton, aiAnalysisRunning_ ? L"AI..." : L"AI Auto");
            drawPanelButton(panel.resetButton, L"reset");
        }
        const D2D1_RECT_F island = rect(layout.island);
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(island, 11.0f * scale, 11.0f * scale), surface.Get());
        renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(island, 11.0f * scale, 11.0f * scale), border.Get(), 1.0f * scale);
        if (videoControlsHovered_ == ButtonKind::VideoPlayPause) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect(layout.playPause), 5.0f * scale, 5.0f * scale), hover.Get());
        if (videoControlsHovered_ == ButtonKind::VideoStepBackward || videoStepHoldDirection_ < 0) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect(layout.stepBackward), 5.0f * scale, 5.0f * scale), hover.Get());
        if (videoControlsHovered_ == ButtonKind::VideoStepForward || videoStepHoldDirection_ > 0) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect(layout.stepForward), 5.0f * scale, 5.0f * scale), hover.Get());
        if (videoControlsHovered_ == ButtonKind::VideoMute) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect(layout.mute), 5.0f * scale, 5.0f * scale), hover.Get());
        if (videoControlsHovered_ == ButtonKind::VideoPlaybackSpeed || videoPlaybackSpeedPanelOpen_) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect(layout.playbackSpeed), 5.0f * scale, 5.0f * scale), hover.Get());
        if (videoControlsHovered_ == ButtonKind::VideoAdjustments || videoAdjustmentsPanelOpen_) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect(layout.adjustments), 5.0f * scale, 5.0f * scale), hover.Get());
        if (videoControlsHovered_ == ButtonKind::VideoFullscreen) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect(layout.fullscreen), 5.0f * scale, 5.0f * scale), hover.Get());

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

        DrawOverlayText(L"-1", static_cast<float>(layout.stepBackward.left), static_cast<float>(layout.stepBackward.top), static_cast<float>(layout.stepBackward.right - layout.stepBackward.left), static_cast<float>(layout.stepBackward.bottom - layout.stepBackward.top), 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, text.Get(), true, false, true);
        DrawOverlayText(L"+1", static_cast<float>(layout.stepForward.left), static_cast<float>(layout.stepForward.top), static_cast<float>(layout.stepForward.right - layout.stepForward.left), static_cast<float>(layout.stepForward.bottom - layout.stepForward.top), 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, text.Get(), true, false, true);
        const std::wstring playbackRateLabel = FormatPlaybackRate(videoEffectivePlaybackRate_);
        DrawOverlayText(playbackRateLabel.c_str(), static_cast<float>(layout.playbackSpeed.left), static_cast<float>(layout.playbackSpeed.top), static_cast<float>(layout.playbackSpeed.right - layout.playbackSpeed.left), static_cast<float>(layout.playbackSpeed.bottom - layout.playbackSpeed.top), 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, text.Get(), true, false, true);
        if (videoControlsHovered_ == ButtonKind::VideoPlaybackSpeed && !videoPlaybackSpeedPanelOpen_) {
            const float tooltipWidth = 92.0f * scale, tooltipHeight = 24.0f * scale;
            const float tooltipLeft = (layout.playbackSpeed.left + layout.playbackSpeed.right) * 0.5f - tooltipWidth * 0.5f;
            const float tooltipTop = static_cast<float>(layout.island.top) - tooltipHeight - 6.0f * scale;
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tooltipLeft, tooltipTop, tooltipLeft + tooltipWidth, tooltipTop + tooltipHeight), 5.0f * scale, 5.0f * scale), surface.Get());
            DrawOverlayText(L"playback speed", tooltipLeft, tooltipTop, tooltipWidth, tooltipHeight, 10.5f, DWRITE_FONT_WEIGHT_NORMAL, text.Get(), true, false, true);
        }

        const float adjustmentsCenterX = (layout.adjustments.left + layout.adjustments.right) * 0.5f;
        const float adjustmentsCenterY = (layout.adjustments.top + layout.adjustments.bottom) * 0.5f;
        for (int index = -1; index <= 1; ++index) {
            const float x = adjustmentsCenterX + index * 5.0f * scale;
            renderTarget_->DrawLine(D2D1::Point2F(x, adjustmentsCenterY - 7.0f * scale), D2D1::Point2F(x, adjustmentsCenterY + 7.0f * scale), text.Get(), 1.25f * scale);
            const float y = adjustmentsCenterY + (index == 0 ? 3.0f : -3.0f) * scale;
            renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 2.1f * scale, 2.1f * scale), text.Get());
        }
        if (videoControlsHovered_ == ButtonKind::VideoAdjustments && !videoAdjustmentsPanelOpen_) {
            const float tooltipWidth = 78.0f * scale, tooltipHeight = 24.0f * scale;
            const float tooltipLeft = adjustmentsCenterX - tooltipWidth * 0.5f;
            const float tooltipTop = static_cast<float>(layout.island.top) - tooltipHeight - 6.0f * scale;
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tooltipLeft, tooltipTop, tooltipLeft + tooltipWidth, tooltipTop + tooltipHeight), 5.0f * scale, 5.0f * scale), surface.Get());
            DrawOverlayText(L"adjustments", tooltipLeft, tooltipTop, tooltipWidth, tooltipHeight, 10.5f, DWRITE_FONT_WEIGHT_NORMAL, text.Get(), true, false, true);
        }

        const float fullscreenCenterX = (layout.fullscreen.left + layout.fullscreen.right) * 0.5f;
        const float fullscreenCenterY = (layout.fullscreen.top + layout.fullscreen.bottom) * 0.5f;
        const float fullscreenArm = 7.0f * scale, fullscreenInset = 3.0f * scale;
        const auto drawFullscreenCorner = [&](float x, float y, float horizontal, float vertical) {
            renderTarget_->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x + horizontal * fullscreenArm, y), text.Get(), 1.5f * scale);
            renderTarget_->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x, y + vertical * fullscreenArm), text.Get(), 1.5f * scale);
        };
        if (fullscreen_) {
            drawFullscreenCorner(fullscreenCenterX - fullscreenInset, fullscreenCenterY - fullscreenInset, -1.0f, -1.0f);
            drawFullscreenCorner(fullscreenCenterX + fullscreenInset, fullscreenCenterY - fullscreenInset, 1.0f, -1.0f);
            drawFullscreenCorner(fullscreenCenterX - fullscreenInset, fullscreenCenterY + fullscreenInset, -1.0f, 1.0f);
            drawFullscreenCorner(fullscreenCenterX + fullscreenInset, fullscreenCenterY + fullscreenInset, 1.0f, 1.0f);
        } else {
            drawFullscreenCorner(fullscreenCenterX - fullscreenArm, fullscreenCenterY - fullscreenArm, 1.0f, 1.0f);
            drawFullscreenCorner(fullscreenCenterX + fullscreenArm, fullscreenCenterY - fullscreenArm, -1.0f, 1.0f);
            drawFullscreenCorner(fullscreenCenterX - fullscreenArm, fullscreenCenterY + fullscreenArm, 1.0f, -1.0f);
            drawFullscreenCorner(fullscreenCenterX + fullscreenArm, fullscreenCenterY + fullscreenArm, -1.0f, -1.0f);
        }
        if (videoControlsHovered_ == ButtonKind::VideoFullscreen) {
            const float tooltipWidth = fullscreen_ ? 98.0f * scale : 72.0f * scale, tooltipHeight = 24.0f * scale;
            const float tooltipLeft = fullscreenCenterX - tooltipWidth * 0.5f;
            const float tooltipTop = static_cast<float>(layout.island.top) - tooltipHeight - 6.0f * scale;
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tooltipLeft, tooltipTop, tooltipLeft + tooltipWidth, tooltipTop + tooltipHeight), 5.0f * scale, 5.0f * scale), surface.Get());
            DrawOverlayText(fullscreen_ ? L"exit fullscreen" : L"fullscreen", tooltipLeft, tooltipTop, tooltipWidth, tooltipHeight, 10.5f, DWRITE_FONT_WEIGHT_NORMAL, text.Get(), true, false, true);
        }

        double current = 0.0, duration = 0.0;
        const bool hasTimes = videoPlayer_.GetPlaybackTimes(current, duration);
        if ((videoScrubbing_ || videoPausedSeekRefreshPending_) && hasTimes) current = videoScrubSeconds_;
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
        const int compactRow = MulDiv(18, dpi, 96);
        const int fixedHeight = MulDiv(114, dpi, 96);
        return fixedHeight + static_cast<int>(kShortcutEntryCount) * normalRow <= availableHeight ? normalRow :
            std::max(compactRow, (availableHeight - fixedHeight) / static_cast<int>(kShortcutEntryCount));
    }

    RECT GetOverlayBounds() const {
        RECT client{};
        GetClientRect(window_, &client);
        if (!HasOverlay()) return {};
        const UINT dpi = GetDpiForWindow(window_);
        const int rowHeight = GetShortcutRowHeight();
        const int desiredWidth = MulDiv(overlay_ == OverlayKind::KeyboardShortcuts ? 460 :
            overlay_ == OverlayKind::Settings ? 760 : overlay_ == OverlayKind::ResetConfirm ? 500 : overlay_ == OverlayKind::DeleteConfirm ? 540 :
            overlay_ == OverlayKind::Welcome ? 640 : overlay_ == OverlayKind::DefaultAppsHelper ? 560 : overlay_ == OverlayKind::Feedback ? 440 : overlay_ == OverlayKind::Help ? 700 : (overlay_ == OverlayKind::PrintError || overlay_ == OverlayKind::RegistrationError) ? 420 : 460, dpi, 96);
        int desiredHeight = overlay_ == OverlayKind::KeyboardShortcuts
            ? MulDiv(114, dpi, 96) + static_cast<int>(kShortcutEntryCount) * rowHeight
            : overlay_ == OverlayKind::Settings ? MulDiv(680, dpi, 96) : overlay_ == OverlayKind::ResetConfirm ? MulDiv(236, dpi, 96) : overlay_ == OverlayKind::DeleteConfirm ? MulDiv(268, dpi, 96) :
            overlay_ == OverlayKind::Welcome ? MulDiv(224, dpi, 96) : overlay_ == OverlayKind::DefaultAppsHelper ? MulDiv(418, dpi, 96) : overlay_ == OverlayKind::Feedback ? MulDiv(330, dpi, 96) : overlay_ == OverlayKind::Help ? MulDiv(680, dpi, 96) : overlay_ == OverlayKind::PrintError ? MulDiv(190, dpi, 96) : overlay_ == OverlayKind::RegistrationError ? MulDiv(220, dpi, 96) : MulDiv(220, dpi, 96);
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

    bool CreateBitmapFromResource(int resourceId, LPCWSTR resourceType, UINT targetWidth, UINT targetHeight, ComPtr<ID2D1Bitmap>& target) {
        const HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(resourceId), resourceType);
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
        if (!CreateBitmapFromResource(kTopBarLogoResourceId, RT_RCDATA, width, height, aboutLogo_)) return false;
        aboutLogoWidth_ = width;
        aboutLogoHeight_ = height;
        return true;
    }

    bool EnsureTopBarLogo(UINT width, UINT height) {
        if (topBarLogo_ && topBarLogoWidth_ == width && topBarLogoHeight_ == height) return true;
        topBarLogo_.Reset();
        topBarLogoWidth_ = 0;
        topBarLogoHeight_ = 0;
        if (!CreateBitmapFromResource(kTopBarLogoResourceId, RT_RCDATA, width, height, topBarLogo_)) return false;
        topBarLogoWidth_ = width;
        topBarLogoHeight_ = height;
        return true;
    }

    bool EnsureFilmstripVideoIcon(UINT size) {
        if (filmstripVideoIcon_ && filmstripVideoIconSize_ == size) return true;
        filmstripVideoIcon_.Reset();
        filmstripVideoIconSize_ = 0;
        const HRSRC groupResource = FindResourceW(nullptr, MAKEINTRESOURCEW(kFilmstripVideoIconGroupResourceId), RT_GROUP_ICON);
        if (!groupResource) return false;
        const DWORD groupSize = SizeofResource(nullptr, groupResource);
        const HGLOBAL loadedGroup = LoadResource(nullptr, groupResource);
        const BYTE* group = static_cast<const BYTE*>(loadedGroup ? LockResource(loadedGroup) : nullptr);
        if (!group || groupSize < 6) return false;
        const UINT count = static_cast<UINT>(group[4]) | static_cast<UINT>(group[5]) << 8;
        if (count == 0 || groupSize < 6 + count * 14) return false;
        UINT frameResourceId = 0;
        UINT bestDistance = UINT_MAX;
        for (UINT index = 0; index < count; ++index) {
            const BYTE* entry = group + 6 + index * 14;
            const UINT frameSize = entry[0] ? entry[0] : 256;
            const UINT distance = frameSize > size ? frameSize - size : size - frameSize;
            if (distance < bestDistance) {
                bestDistance = distance;
                frameResourceId = static_cast<UINT>(entry[12]) | static_cast<UINT>(entry[13]) << 8;
            }
        }
        if (!frameResourceId || !CreateBitmapFromResource(static_cast<int>(frameResourceId), RT_ICON, size, size, filmstripVideoIcon_)) return false;
        filmstripVideoIconSize_ = size;
        return true;
    }

    RECT GetEmptyOpenFileButtonBounds() const {
        RECT client{};
        GetClientRect(window_, &client);
        const UINT dpi = GetDpiForWindow(window_);
        const int width = MulDiv(132, dpi, 96);
        const int height = MulDiv(38, dpi, 96);
        const int canvasTop = fullscreen_ ? 0 : GetFrameMetrics(window_).titleBarHeight;
        const int top = canvasTop + std::max(0L, (client.bottom - canvasTop - height) / 2);
        const int left = (client.right - width) / 2;
        return { left, top, left + width, top + height };
    }

    void DrawEmptyState() {
        if (!EmptyStatePresentationActive() || HasOverlay()) return;
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
        const RECT buttonBounds = GetEmptyOpenFileButtonBounds();
        const float textTop = std::max(top, static_cast<float>(buttonBounds.top) - 38.0f * scale);
        DrawOverlayText(error_.empty() ? L"drag and drop an image here or open a file" : error_.c_str(), 24.0f * scale,
            textTop, target.width - 48.0f * scale, 30.0f * scale, 16.0f,
            DWRITE_FONT_WEIGHT_NORMAL, secondary.Get(), true, false, true);
        const D2D1_RECT_F buttonRect = D2D1::RectF(static_cast<float>(buttonBounds.left), static_cast<float>(buttonBounds.top),
            static_cast<float>(buttonBounds.right), static_cast<float>(buttonBounds.bottom));
        ID2D1Brush* buttonBrush = pressedButton_ == ButtonKind::EmptyOpenFile ? buttonPressed.Get() :
            hoveredButton_ == ButtonKind::EmptyOpenFile ? buttonHover.Get() : button.Get();
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(buttonRect, 5.0f * scale, 5.0f * scale), buttonBrush);
        DrawOverlayText(L"open file", buttonRect.left, buttonRect.top, buttonRect.right - buttonRect.left,
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
                const float logoTop = std::round(static_cast<float>(bounds.top) + 20.0f * dpiScale);
                renderTarget_->DrawBitmap(aboutLogo_.Get(), D2D1::RectF(logoLeft, logoTop, logoLeft + logoWidth, logoTop + logoHeight));
            }
            DrawOverlayText(L"make Viewtrious the default for common media formats?", left, static_cast<float>(bounds.top) + 94.0f * dpiScale,
                contentWidth, 26.0f * dpiScale, 19.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), false, false, true);
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
                DrawOverlayText(L"yes", primaryButton.left, primaryButton.top,
                    primaryButton.right - primaryButton.left, primaryButton.bottom - primaryButton.top, 16.0f,
                    DWRITE_FONT_WEIGHT_SEMI_BOLD, buttonText.Get(), true, false, true);
            }
            renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(secondaryButton, 5.0f * dpiScale, 5.0f * dpiScale), borderBrush.Get(), 1.0f);
            DrawOverlayText(L"not now", secondaryButton.left, secondaryButton.top,
                secondaryButton.right - secondaryButton.left, secondaryButton.bottom - secondaryButton.top, 16.0f,
                DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), true, false, true);
        } else if (overlay_ == OverlayKind::DefaultAppsHelper) {
            const UINT logoHeight = static_cast<UINT>(std::max(1.0f, std::round(24.0f * dpiScale)));
            const UINT logoWidth = static_cast<UINT>(std::max(1.0f, std::round(static_cast<float>(logoHeight) * 300.0f / 73.0f)));
            if (EnsureTopBarLogo(logoWidth, logoHeight)) {
                const float logoTop = std::round(static_cast<float>(bounds.top) + 24.0f * dpiScale);
                renderTarget_->DrawBitmap(topBarLogo_.Get(), D2D1::RectF(left, logoTop, left + logoWidth, logoTop + logoHeight),
                    1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
            DrawOverlayText(L"choose which file types should open with Viewtrious.", left,
                static_cast<float>(bounds.top) + 68.0f * dpiScale, contentWidth, 24.0f * dpiScale,
                16.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get());
            constexpr float formatPanelTop = 102.0f, formatPanelHeight = 200.0f, formatPanelPadding = 12.0f;
            constexpr float categoryLineHeight = 20.0f, formatLineHeight = 18.0f, labelToFormatsGap = 6.0f, familyGap = 12.0f, noteGap = 4.0f, closingTop = 318.0f;
            ComPtr<ID2D1SolidColorBrush> formatPanelBrush, formatNoteBrush;
            const D2D1_COLOR_F formatPanelColor = dark ? D2D1::ColorF(34.0f / 255.0f, 37.0f / 255.0f, 44.0f / 255.0f)
                : D2D1::ColorF(242.0f / 255.0f, 242.0f / 255.0f, 242.0f / 255.0f);
            const D2D1_COLOR_F formatNoteColor = dark ? D2D1::ColorF(155.0f / 255.0f, 158.0f / 255.0f, 166.0f / 255.0f)
                : D2D1::ColorF(112.0f / 255.0f, 112.0f / 255.0f, 112.0f / 255.0f);
            if (FAILED(renderTarget_->CreateSolidColorBrush(formatPanelColor, &formatPanelBrush)) ||
                FAILED(renderTarget_->CreateSolidColorBrush(formatNoteColor, &formatNoteBrush))) return;
            const D2D1_RECT_F formatPanel = D2D1::RectF(left, static_cast<float>(bounds.top) + formatPanelTop * dpiScale,
                left + contentWidth, static_cast<float>(bounds.top) + (formatPanelTop + formatPanelHeight) * dpiScale);
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(formatPanel, 6.0f * dpiScale, 6.0f * dpiScale), formatPanelBrush.Get());
            const float formatLeft = left + formatPanelPadding * dpiScale;
            const float formatWidth = contentWidth - formatPanelPadding * 2.0f * dpiScale;
            const auto drawFormatFamily = [&](const wchar_t* label, const wchar_t* formats, float top) {
                const float rowTop = static_cast<float>(bounds.top) + top * dpiScale;
                DrawOverlayText(label, formatLeft, rowTop, formatWidth, categoryLineHeight * dpiScale,
                    15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
                DrawOverlayText(formats, formatLeft, rowTop + (categoryLineHeight + labelToFormatsGap) * dpiScale, formatWidth, formatLineHeight * dpiScale,
                    15.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get());
            };
            const float imagesTop = formatPanelTop + formatPanelPadding;
            const float familyHeight = categoryLineHeight + labelToFormatsGap + formatLineHeight;
            const float videoTop = imagesTop + familyHeight + familyGap;
            const float modelsTop = videoTop + familyHeight + familyGap;
            drawFormatFamily(L"images", L"JPG, JPEG, PNG, BMP, GIF, HEIC, HEIF, DNG", imagesTop);
            drawFormatFamily(L"video", L"MP4, MOV, MKV", videoTop);
            drawFormatFamily(L"3D", StepAddonPresent() ? L"STL, 3MF, STEP, STP" : L"STL, 3MF", modelsTop);
            DrawOverlayText(L"STEP/STP require the optional add-on", formatLeft,
                static_cast<float>(bounds.top) + (modelsTop + familyHeight + noteGap) * dpiScale, formatWidth, 16.0f * dpiScale,
                13.0f, DWRITE_FONT_WEIGHT_NORMAL, formatNoteBrush.Get());
            DrawOverlayText(L"close Windows Settings when you are finished.", left,
                static_cast<float>(bounds.top) + closingTop * dpiScale, contentWidth, 24.0f * dpiScale,
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
                DrawOverlayText(L"choose defaults", open.left, open.top, open.right - open.left, open.bottom - open.top, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, buttonText.Get(), true, false, true);
            }
            renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(cancel, 5.0f * dpiScale, 5.0f * dpiScale), borderBrush.Get(), 1.0f);
            DrawOverlayText(L"cancel", cancel.left, cancel.top, cancel.right - cancel.left, cancel.bottom - cancel.top, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), true, false, true);
        } else if (overlay_ == OverlayKind::Help) {
            const RECT closeBounds = GetHelpCloseBounds();
            const D2D1_RECT_F close = D2D1::RectF(static_cast<float>(closeBounds.left), static_cast<float>(closeBounds.top), static_cast<float>(closeBounds.right), static_cast<float>(closeBounds.bottom));
            if (hoveredButton_ == ButtonKind::HelpClose || pressedButton_ == ButtonKind::HelpClose) {
                ComPtr<ID2D1SolidColorBrush> closeHover;
                if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(60.f / 255, 64.f / 255, 74.f / 255) : D2D1::ColorF(228.f / 255, 228.f / 255, 228.f / 255), &closeHover)))
                    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(close, 4.0f * dpiScale, 4.0f * dpiScale), closeHover.Get());
            }
            const float closeCenterX = (close.left + close.right) * 0.5f, closeCenterY = (close.top + close.bottom) * 0.5f, closeRadius = 5.0f * dpiScale;
            renderTarget_->DrawLine(D2D1::Point2F(closeCenterX - closeRadius, closeCenterY - closeRadius), D2D1::Point2F(closeCenterX + closeRadius, closeCenterY + closeRadius), primaryBrush.Get(), std::max(1.0f, dpiScale));
            renderTarget_->DrawLine(D2D1::Point2F(closeCenterX + closeRadius, closeCenterY - closeRadius), D2D1::Point2F(closeCenterX - closeRadius, closeCenterY + closeRadius), primaryBrush.Get(), std::max(1.0f, dpiScale));
            DrawOverlayText(L"help", left, static_cast<float>(bounds.top) + panelPadding, contentWidth - 44.0f * dpiScale, 28.0f * dpiScale, 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
            ComPtr<ID2D1SolidColorBrush> accent, rowHover, selectedText;
            if (FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f / 255, 120.f / 255, 212.f / 255), &accent)) ||
                FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(60.f / 255, 64.f / 255, 74.f / 255) : D2D1::ColorF(228.f / 255, 228.f / 255, 228.f / 255), &rowHover)) ||
                FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &selectedText))) return;
            const RECT railBounds = GetHelpRailBounds();
            const float dividerX = static_cast<float>(railBounds.right) + 12.0f * dpiScale;
            renderTarget_->DrawLine(D2D1::Point2F(dividerX, static_cast<float>(railBounds.top)), D2D1::Point2F(dividerX, static_cast<float>(railBounds.bottom)), borderBrush.Get());
            const int topicRowHeight = GetHelpTopicRowHeight();
            const float topicFontSize = topicRowHeight < MulDiv(25, GetDpiForWindow(window_), 96) ? 10.0f : 11.5f;
            for (int topic = 0; topic < static_cast<int>(kHelpTopics.size()); ++topic) {
                const RECT topicBounds = GetHelpTopicBounds(topic);
                const D2D1_RECT_F topicRect = D2D1::RectF(static_cast<float>(topicBounds.left), static_cast<float>(topicBounds.top), static_cast<float>(topicBounds.right), static_cast<float>(topicBounds.bottom));
                if (topic == helpTopic_) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(topicRect, 4.0f * dpiScale, 4.0f * dpiScale), accent.Get());
                else if (hoveredButton_ == ButtonKind::HelpTopic && helpTopicHover_ == topic) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(topicRect, 4.0f * dpiScale, 4.0f * dpiScale), rowHover.Get());
                DrawOverlayText(kHelpTopics[topic].title, topicRect.left + 9.0f * dpiScale, topicRect.top, topicRect.right - topicRect.left - 18.0f * dpiScale,
                    topicRect.bottom - topicRect.top, topicFontSize, DWRITE_FONT_WEIGHT_SEMI_BOLD, topic == helpTopic_ ? selectedText.Get() : primaryBrush.Get(), true);
            }
            const RECT contentBounds = GetHelpContentBounds();
            const D2D1_RECT_F viewport = D2D1::RectF(static_cast<float>(contentBounds.left), static_cast<float>(contentBounds.top), static_cast<float>(contentBounds.right), static_cast<float>(contentBounds.bottom));
            renderTarget_->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            renderTarget_->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -helpScroll_));
            const HelpTopic& topic = kHelpTopics[helpTopic_];
            const int helpWidth = static_cast<int>(std::max<LONG>(1, contentBounds.right - contentBounds.left));
            float y = viewport.top + helpScroll_;
            const float titleHeight = static_cast<float>(MeasureHelpTextHeight(topic.title, helpWidth, 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
            DrawOverlayText(topic.title, viewport.left, y, viewport.right - viewport.left, titleHeight, 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), false, false, false, true);
            y += titleHeight + 14.0f * dpiScale;
            if (topic.sectionCount == 0) {
                const float bodyHeight = static_cast<float>(MeasureHelpTextHeight(topic.body, helpWidth, 14.0f, DWRITE_FONT_WEIGHT_NORMAL));
                DrawOverlayText(topic.body, viewport.left, y, viewport.right - viewport.left, bodyHeight, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), false, false, false, true);
            } else {
                for (size_t index = 0; index < topic.sectionCount; ++index) {
                    const HelpSection& section = topic.sections[index];
                    const float headingHeight = static_cast<float>(MeasureHelpTextHeight(section.heading, helpWidth, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
                    DrawOverlayText(section.heading, viewport.left, y, viewport.right - viewport.left, headingHeight, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), false, false, false, true);
                    y += headingHeight + 4.0f * dpiScale;
                    const float bodyHeight = static_cast<float>(MeasureHelpTextHeight(section.body, helpWidth, 14.0f, DWRITE_FONT_WEIGHT_NORMAL));
                    DrawOverlayText(section.body, viewport.left, y, viewport.right - viewport.left, bodyHeight, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), false, false, false, true);
                    y += bodyHeight + 14.0f * dpiScale;
                }
            }
            renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
            renderTarget_->PopAxisAlignedClip();
        } else if (overlay_ == OverlayKind::KeyboardShortcuts) {
            DrawOverlayText(L"Keyboard Shortcuts", left, static_cast<float>(bounds.top) + panelPadding,
                contentWidth, 24.0f * dpiScale, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
            const float shortcutWidth = 176.0f * dpiScale;
            float y = static_cast<float>(bounds.top) + panelPadding + 38.0f * dpiScale;
            const float shortcutRowHeight = static_cast<float>(GetShortcutRowHeight());
            const auto drawRows = [&](const auto& entries) {
                for (const ShortcutEntry& line : entries) {
                    const float fontSize = shortcutRowHeight < 22.0f * dpiScale ? 10.5f : 12.5f;
                    DrawOverlayText(line.shortcut, left, y, shortcutWidth, 18.0f * dpiScale, fontSize, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
                    DrawOverlayText(line.description, left + shortcutWidth, y, contentWidth - shortcutWidth,
                        18.0f * dpiScale, fontSize, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get());
                    y += shortcutRowHeight;
                }
            };
            drawRows(kKeyboardShortcutEntries);
            y += 10.0f * dpiScale;
            DrawOverlayText(L"Mouse Navigation", left, y, contentWidth, 18.0f * dpiScale, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
            y += 22.0f * dpiScale;
            drawRows(kMouseNavigationEntries);
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
            drawNavigation(SettingsPage::Image2D, ButtonKind::SettingsImage2DPage, L"IMAGE SETTINGS");
            drawNavigation(SettingsPage::Video2D, ButtonKind::SettingsVideoPage, L"VIDEO SETTINGS");
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
            const RECT swipeBounds = GetSettingsOptionBounds(3);
            const float appearanceTop = static_cast<float>(swipeBounds.bottom - bounds.top + SettingsSectionGap()) / dpiScale;
            const RECT themeBounds = GetSettingsThemeBounds(ThemePreference::System);
            const float defaultTypesTop = static_cast<float>(themeBounds.bottom - bounds.top + SettingsSectionGap()) / dpiScale;
            const RECT defaultAppsLayoutBounds = GetSettingsDefaultAppsButtonBounds();
            const float resetTop = static_cast<float>(defaultAppsLayoutBounds.bottom - bounds.top + SettingsSectionGap()) / dpiScale;
            group(L"GENERAL", 76.0f);
            drawToggle(0, ButtonKind::SettingsRememberPlacement, L"remember application position and size", rememberWindowPlacement_);
            drawToggle(1, ButtonKind::SettingsIncludeHidden, L"include hidden images in folder", includeHiddenImages_);
            drawToggle(2, ButtonKind::SettingsConfirmDelete, L"confirm before deleting images", confirmBeforeDeleting_);
            drawToggle(3, ButtonKind::SettingsSwipeToNavigateWhenFit, L"swipe to navigate when fit", swipeToNavigateWhenFit_);
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
            } else if (settingsPage_ == SettingsPage::Video2D) {
            const RECT placeholder = GetSettingsVideoPlaceholderBounds();
            DrawOverlayText(L"video-specific settings will appear here as they are added.", static_cast<float>(placeholder.left), static_cast<float>(placeholder.top),
                static_cast<float>(placeholder.right - placeholder.left), static_cast<float>(placeholder.bottom - placeholder.top), 16.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), false, false, false, true);
            } else if (settingsPage_ == SettingsPage::Image2D) {
            group(L"2D VIEWER", 76.0f);
            drawToggle(4, ButtonKind::SettingsAnimations, L"animations and face effects", animationsEnabled_);
            drawToggle(5, ButtonKind::SettingsReverseWheelZoom, L"reverse mouse wheel zoom direction", reverseMouseWheelZoom_);
            drawToggle(6, ButtonKind::SettingsAlwaysShowFilmstrip, L"always show filmstrip", alwaysShowFilmstrip_);
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
            drawToggle(6, ButtonKind::SettingsSpaceMouse, L"enable 3Dconnexion SpaceMouse", spaceMouseRuntimeAvailable_ && spaceMouseEnabled_, spaceMouseRuntimeAvailable_);
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
        } else if (overlay_ == OverlayKind::PrintError || overlay_ == OverlayKind::RegistrationError) {
            const RECT dismissBounds = GetPrintErrorDismissButtonBounds();
            const bool registrationError = overlay_ == OverlayKind::RegistrationError;
            DrawOverlayText(registrationError ? L"unable to register Viewtrious file types" : L"unable to print this file", left, static_cast<float>(bounds.top) + panelPadding,
                contentWidth, registrationError ? 52.0f * dpiScale : 34.0f * dpiScale, 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), false, false, true);
            DrawOverlayText(registrationError ? L"Viewtrious could not prepare Windows file associations." : L"Windows could not start printing this file.", left, static_cast<float>(bounds.top) + panelPadding + (registrationError ? 62.0f : 46.0f) * dpiScale,
                contentWidth, 42.0f * dpiScale, 16.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), false, false, true, true);
            const D2D1_RECT_F dismiss = D2D1::RectF(static_cast<float>(dismissBounds.left), static_cast<float>(dismissBounds.top),
                static_cast<float>(dismissBounds.right), static_cast<float>(dismissBounds.bottom));
            ComPtr<ID2D1SolidColorBrush> hover, pressed;
            if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(60.f / 255, 64.f / 255, 74.f / 255) : D2D1::ColorF(228.f / 255, 228.f / 255, 228.f / 255), &hover)) &&
                SUCCEEDED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(75.f / 255, 80.f / 255, 92.f / 255) : D2D1::ColorF(210.f / 255, 210.f / 255, 210.f / 255), &pressed))) {
                if (pressedButton_ == ButtonKind::PrintErrorDismiss) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(dismiss, 5.0f * dpiScale, 5.0f * dpiScale), pressed.Get());
                else if (hoveredButton_ == ButtonKind::PrintErrorDismiss) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(dismiss, 5.0f * dpiScale, 5.0f * dpiScale), hover.Get());
            }
            renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(dismiss, 5.0f * dpiScale, 5.0f * dpiScale), borderBrush.Get(), 1.0f);
            DrawOverlayText(L"dismiss", dismiss.left, dismiss.top, dismiss.right - dismiss.left, dismiss.bottom - dismiss.top,
                16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), true, false, true);
        } else if (overlay_ == OverlayKind::Feedback) {
            DrawOverlayText(L"feedback", left, static_cast<float>(bounds.top) + panelPadding, contentWidth, 34.0f * dpiScale, 24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
            DrawOverlayText(L"help make Viewtrious better.", left, static_cast<float>(bounds.top) + panelPadding + 42.0f * dpiScale, contentWidth, 26.0f * dpiScale, 16.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get());
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
            drawAction(false, L"report a bug", L"something isn't working correctly.");
            drawAction(true, L"suggest a feature", L"have an idea for Viewtrious?");
            DrawOverlayText(L"opens GitHub in your web browser.", left, static_cast<float>(bounds.bottom) - panelPadding - 20.0f * dpiScale, contentWidth, 20.0f * dpiScale, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), false, true);
        } else {
            const UINT logoWidth = static_cast<UINT>(std::max(1.0f, std::round(std::min(216.0f * dpiScale, contentWidth))));
            const UINT logoHeight = static_cast<UINT>(std::max(1.0f, std::round(static_cast<float>(logoWidth) * 577.0f / 2375.0f)));
            const float aboutTextGap = 16.0f * dpiScale;
            const float aboutLineHeight = 20.0f * dpiScale;
            const float aboutLineGap = 5.0f * dpiScale;
            const float aboutGroupHeight = static_cast<float>(logoHeight) + aboutTextGap + aboutLineHeight + aboutLineGap + aboutLineHeight;
            const float logoLeft = std::round(static_cast<float>(bounds.left) + (static_cast<float>(bounds.right - bounds.left) - static_cast<float>(logoWidth)) * 0.5f);
            const float logoTop = std::round(static_cast<float>(bounds.top) + (static_cast<float>(bounds.bottom - bounds.top) - aboutGroupHeight) * 0.5f);
            float logoBottom = logoTop;
            if (EnsureAboutLogo(logoWidth, logoHeight)) {
                renderTarget_->DrawBitmap(aboutLogo_.Get(), D2D1::RectF(logoLeft, logoTop, logoLeft + logoWidth, logoTop + logoHeight));
                logoBottom = logoTop + logoHeight;
            }
            const float textTop = logoBottom + aboutTextGap;
            DrawOverlayText(L"version " VIEWTRIOUS_VERSION, logoLeft, textTop, static_cast<float>(logoWidth), aboutLineHeight,
                14.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), false, false, true);
            DrawOverlayText(L"extremely lightweight image viewer", logoLeft, textTop + aboutLineHeight + aboutLineGap, static_cast<float>(logoWidth),
                aboutLineHeight, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), false, false, true);
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
        const int labelRight = bounds.right - MulDiv(14, dpi, 96);
        const auto drawItem = [&](DropdownItem item, int top, const wchar_t* label, wchar_t glyph, bool nativeCloseIcon = false) {
            if (dropdownPressed_ == item) renderTarget_->FillRectangle(row(top), pressedBrush.Get());
            else if (dropdownHovered_ == item) renderTarget_->FillRectangle(row(top), hoverBrush.Get());
            if (nativeCloseIcon) {
                const float centerX = static_cast<float>(iconLeft) + static_cast<float>(iconWidth) * 0.5f;
                const float centerY = static_cast<float>(top) + static_cast<float>(rowHeight) * 0.5f;
                const float radius = std::min(static_cast<float>(iconWidth), static_cast<float>(rowHeight)) * 0.27f;
                const float stroke = std::max(1.0f, static_cast<float>(dpi) / 96.0f);
                renderTarget_->DrawLine(D2D1::Point2F(centerX - radius, centerY - radius), D2D1::Point2F(centerX + radius, centerY + radius), textBrush.Get(), stroke);
                renderTarget_->DrawLine(D2D1::Point2F(centerX + radius, centerY - radius), D2D1::Point2F(centerX - radius, centerY + radius), textBrush.Get(), stroke);
            } else {
                DrawMenuGlyph(glyph, static_cast<float>(iconLeft), static_cast<float>(top), static_cast<float>(iconWidth), static_cast<float>(rowHeight), textBrush.Get());
            }
            DrawOverlayText(label, static_cast<float>(labelLeft), static_cast<float>(top),
                static_cast<float>(std::max(0, labelRight - labelLeft)), static_cast<float>(rowHeight),
                13.0f, DWRITE_FONT_WEIGHT_NORMAL, textBrush.Get(), true);
        };
        const int separatorGap = MulDiv(9, dpi, 96);
        int top = firstTop;
        const auto separator = [&] {
            const float y = static_cast<float>(top + separatorGap / 2);
            renderTarget_->DrawLine(D2D1::Point2F(static_cast<float>(bounds.left + MulDiv(12, dpi, 96)), y), D2D1::Point2F(static_cast<float>(bounds.right - MulDiv(12, dpi, 96)), y), borderBrush.Get());
            top += separatorGap;
        };
        drawItem(DropdownItem::OpenFile, top, L"open file...", L'\uE8B7'); top += rowHeight;
        drawItem(DropdownItem::Settings, top, L"settings", L'\uE713'); top += rowHeight;
        separator();
        drawItem(DropdownItem::QuickTour, top, L"quick tutorial", L'\uE897'); top += rowHeight;
        drawItem(DropdownItem::KeyboardShortcuts, top, L"keyboard shortcuts", L'\uE765'); top += rowHeight;
        separator();
        drawItem(DropdownItem::Help, top, L"help", L'\uE897'); top += rowHeight;
        drawItem(DropdownItem::Feedback, top, L"feedback", L'\uE939'); top += rowHeight;
        drawItem(DropdownItem::About, top, L"about", L'\uE946'); top += rowHeight;
        separator();
        drawItem(DropdownItem::Close, top, L"close Viewtrious", 0, true);
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
        drawItem(ContextAction::Copy, L"copy media", L'\uE8C8'); drawItem(ContextAction::Print, L"Print", L'\uE749'); separator();
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
            DrawHandwrittenText(L"open images", annotationLeft, headingTop, annotationWidth, 32.0f * scale, 24.0f, pencil.Get(), true);
            DrawOverlayText(L"use open file or drag and drop", annotationLeft, headingTop + 31.0f * scale,
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
            DrawHandwrittenText(L"resize the window", noteLeft, noteTop, noteWidth, 34.0f * scale, 23.0f, pencil.Get());
            DrawOverlayText(L"drag the app corners to resize the app", noteLeft, noteTop + 33.0f * scale,
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
            DrawHandwrittenText(L"menu & settings", headingLeft, headingTop,
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
            DrawHandwrittenText(L"file details appear here", headingLeft, headingTop,
                headingWidth, 38.0f * scale, 22.0f, pencil.Get(), true);
            scribble(target);
        } else if (tutorialStep_ == TutorialStep::ContextMenu) {
            const RECT target = GetContextMenuBounds();
            const float annotationWidth = std::min(230.0f * scale, std::max(150.0f * scale, static_cast<float>(target.left) - 34.0f * scale));
            const float headingLeft = std::max(16.0f * scale, static_cast<float>(target.left) - annotationWidth - 72.0f * scale);
            const float headingTop = std::clamp(static_cast<float>(target.top) + 16.0f * scale, canvasTop + 12.0f * scale,
                static_cast<float>(client.bottom) - 100.0f * scale);
            DrawHandwrittenText(L"right click menu", headingLeft, headingTop, annotationWidth, 38.0f * scale, 22.0f, pencil.Get(), false, true);
            const float noteGap = 18.0f * scale;
            const float noteWidth = std::min(340.0f * scale, static_cast<float>(target.left) - noteGap - 16.0f * scale);
            const float noteLeft = std::max(16.0f * scale, static_cast<float>(target.left) - noteGap - noteWidth);
            DrawOverlayText(L"right-click an image to find these options.", noteLeft, headingTop + 37.0f * scale,
                noteWidth, 28.0f * scale, 16.0f, DWRITE_FONT_WEIGHT_NORMAL, pencil.Get(), true, true);
            scribble(target);
            arrow(D2D1::Point2F(headingLeft + annotationWidth * 0.62f, headingTop - 8.0f * scale),
                D2D1::Point2F(static_cast<float>(target.left) - 4.0f * scale, static_cast<float>(target.top) + 10.0f * scale), 3.1f);
        } else if (tutorialStep_ == TutorialStep::Shortcuts) {
            const RECT target = GetOverlayBounds();
            DrawHandwrittenText(L"keyboard shortcuts", static_cast<float>(target.left), static_cast<float>(target.top) - 38.0f * scale,
                static_cast<float>(target.right - target.left), 32.0f * scale, 21.0f, pencil.Get(), true);
            scribble(target);
            DrawHandwrittenText(L"thanks for downloading, enjoy!", 0, static_cast<float>(target.bottom) + 10.0f * scale,
                static_cast<float>(client.right), 34.0f * scale, 18.0f, pencil.Get(), true);
        }
        const RECT skipBounds = GetTutorialButtonBounds(false), nextBounds = GetTutorialButtonBounds(true);
        const auto asRect = [](const RECT& value) { return D2D1::RectF(static_cast<float>(value.left), static_cast<float>(value.top), static_cast<float>(value.right), static_cast<float>(value.bottom)); };
        const D2D1_RECT_F skip = asRect(skipBounds), next = asRect(nextBounds);
        if (hoveredButton_ == ButtonKind::TutorialSkip || pressedButton_ == ButtonKind::TutorialSkip) renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(skip, 5.0f * scale, 5.0f * scale), pressedButton_ == ButtonKind::TutorialSkip ? buttonPressed.Get() : buttonHover.Get());
        renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(skip, 5.0f * scale, 5.0f * scale), pencil.Get(), 1.0f * scale);
        ID2D1Brush* nextBrush = pressedButton_ == ButtonKind::TutorialNext ? buttonPressed.Get() : hoveredButton_ == ButtonKind::TutorialNext ? buttonHover.Get() : button.Get();
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(next, 5.0f * scale, 5.0f * scale), nextBrush);
        DrawOverlayText(L"skip", skip.left, skip.top, skip.right - skip.left, skip.bottom - skip.top, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, pencil.Get(), true, false, true);
        DrawOverlayText(tutorialStep_ == TutorialStep::Shortcuts ? L"finish" : L"next", next.left, next.top, next.right - next.left, next.bottom - next.top, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, buttonText.Get(), true, false, true);
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
        if (EmptyStatePresentationActive() && !(tutorialPresentation_ && tutorialStep_ == TutorialStep::ImageDetails)) {
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
        imageAdjustedBitmap_.Reset();
        imageAdjustmentSourceTexture_.Reset();
        imageAdjustmentSourceWic_ = nullptr;
        imageAdjustmentUsesLanczos_ = false;
        imageAdjustmentProcessor_.Reset();
        imageAdjustmentProcessorReady_ = false;
        aboutLogo_.Reset();
        aboutLogoWidth_ = 0;
        aboutLogoHeight_ = 0;
        topBarLogo_.Reset();
        topBarLogoWidth_ = 0;
        topBarLogoHeight_ = 0;
        filmstripVideoIcon_.Reset();
        filmstripVideoIconSize_ = 0;
        checkerboardBrush_.Reset();
        checkerboardBitmap_.Reset();
        checkerboardDpi_ = 0;
        for (FilmstripThumbnailEntry& entry : filmstripThumbnails_) entry.bitmap.Reset();
        for (FilmstripHoverPreviewEntry& entry : filmstripHoverPreviews_) entry.bitmap.Reset();
        if (filmstripVideoHoverPreview_) filmstripVideoHoverPreview_->bitmap.Reset();
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
    ComPtr<ID2D1Bitmap1> imageAdjustedBitmap_;
    ComPtr<ID3D11Texture2D> imageAdjustmentSourceTexture_;
    MediaAdjustmentProcessor imageAdjustmentProcessor_;
    IWICBitmapSource* imageAdjustmentSourceWic_ = nullptr;
    std::vector<BYTE> imageAdjustmentPixels_;
    UINT imageAdjustmentSourceWidth_ = 0;
    UINT imageAdjustmentSourceHeight_ = 0;
    bool imageHasTransparency_ = false;
    bool imageAdjustmentSourceDirty_ = true;
    bool imageAdjustmentUsesLanczos_ = false;
    bool imageAdjustmentProcessorReady_ = false;
    ComPtr<ID2D1Bitmap> aboutLogo_;
    UINT aboutLogoWidth_ = 0;
    UINT aboutLogoHeight_ = 0;
    ComPtr<ID2D1Bitmap> topBarLogo_;
    UINT topBarLogoWidth_ = 0;
    UINT topBarLogoHeight_ = 0;
    ComPtr<ID2D1Bitmap> filmstripVideoIcon_;
    UINT filmstripVideoIconSize_ = 0;
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
    uint64_t modelTriangleCount_ = 0;
    std::wstring fileSizeText_;
    std::wstring filenameText_;
    std::wstring error_;
    std::wstring rotationDiagnosticDetail_;
    std::wstring feedbackText_ = L"copied to clipboard";
    std::vector<fs::path> navigationFiles_;
    D2D1_POINT_2F pan_ = D2D1::Point2F();
    D2D1_POINT_2F videoPan_ = D2D1::Point2F();
    POINT lastDragPoint_{};
    POINT swipeNavigationStart_{};
    float zoom_ = 1.0f;
    float videoZoom_ = 1.0f;
    bool fitToWindow_ = true;
    bool videoFitToWindow_ = true;
    bool gifPlaying_ = false;
    bool gifPaused_ = false;
    bool gifPlaybackTimerActive_ = false;
    HANDLE videoPlaybackTimer_ = nullptr;
    HANDLE videoPlaybackStopEvent_ = nullptr;
    std::thread videoPlaybackSchedulerThread_;
    std::atomic<uint64_t> videoPlaybackSchedulerGeneration_{ 0 };
    std::atomic<uint64_t> videoPlaybackWakePendingGeneration_{ 0 };
    std::atomic<LONGLONG> videoPlaybackWakeQpc_{ 0 };
    bool videoPausedSeekRefreshPending_ = false;
    MediaAdjustments videoAdjustments_;
    ImageAdjustments imageAdjustments_;
    ImageAdjustmentPersistence adjustmentPersistence_;
    std::array<unsigned char, 32> imageAdjustmentHash_{};
    uint64_t imageAdjustmentMediaGeneration_ = 0;
    uint64_t imageAdjustmentEditGeneration_ = 0;
    bool imageAdjustmentHashResolved_ = false;
    std::unordered_map<uint64_t, ImageAdjustments> pendingImageAdjustmentSaves_;
    AiAddonLoader aiAddon_;
    std::thread aiAnalysisThread_;
    std::atomic<uint64_t> aiRequestGeneration_{ 0 };
    std::atomic<bool> aiAnalysisRunning_{ false };
    bool imageAdjustmentsPanelOpen_ = false;
    int imageAdjustmentsDragging_ = -1;
    bool videoAdjustmentsPanelOpen_ = false;
    int videoAdjustmentsDragging_ = -1;
    DWORD videoPreferredPlaybackRatePercent_ = 100;
    double videoEffectivePlaybackRate_ = 1.0;
    bool videoPlaybackSpeedPanelOpen_ = false;
    int videoStepHoldDirection_ = 0;
    bool videoStepHoldActive_ = false;
    double videoStepHoldAnchorSeconds_ = 0.0;
    double videoStepHoldDurationSeconds_ = 0.0;
    LONGLONG videoStepHoldStartQpc_ = 0;
    LONGLONG videoStepHoldQpcFrequency_ = 0;
    bool videoPlaybackSchedulerRunning_ = false;
    double videoPlaybackDeadlineQpc_ = 0.0;
    double videoPlaybackFramePeriodQpc_ = 0.0;
    bool gifVisible_ = true;
    bool gifHasLoopExtension_ = false;
    bool committingGifFrame_ = false;
    bool dragging_ = false;
    bool swipeNavigationPending_ = false;
    bool dissolveAwaitingTarget_ = false;
    bool dissolveActive_ = false;
    LONGLONG dissolveStartQpc_ = 0;
    LARGE_INTEGER dissolveQpcFrequency_{};
    UINT dissolveOldWidth_ = 0;
    UINT dissolveOldHeight_ = 0;
    std::wstring dissolveTargetPath_;
    ComPtr<ID2D1Bitmap> dissolveOldBitmap_;
    bool presented_ = false;
    bool navigationBuilt_ = false;
    bool navigationBuildQueued_ = false;
    std::vector<float> filmstripItemWidths_;
    std::vector<float> filmstripItemOffsets_;
    double filmstripScroll_ = 0.0;
    double filmstripScrollVelocity_ = 0.0;
    LONGLONG filmstripScrollLastQpc_ = 0;
    LONGLONG filmstripScrollQpcFrequency_ = 0;
    HANDLE filmstripScrollTimer_ = nullptr;
    HANDLE filmstripScrollStopEvent_ = nullptr;
    std::thread filmstripScrollSchedulerThread_;
    std::atomic<uint64_t> filmstripScrollGeneration_{ 0 };
    std::atomic<uint64_t> filmstripScrollWakePendingGeneration_{ 0 };
    bool filmstripScrollAnimating_ = false;
    bool filmstripLayoutRebuildPending_ = false;
    std::vector<float> filmstripLayoutAspects_;
    std::vector<float> filmstripKnownAspects_;
    std::vector<bool> filmstripAspectAuthoritative_;
    std::vector<bool> filmstripAspectRelayoutPending_;
#ifdef _DEBUG
    UINT filmstripScrollTickCount_ = 0;
    double filmstripScrollTickTotalMs_ = 0.0;
    double filmstripScrollTickMinimumMs_ = 0.0;
    double filmstripScrollTickMaximumMs_ = 0.0;
    std::optional<double> filmstripPostStopPosition_;
    UINT filmstripPostStopPaintCount_ = 0;
#endif
    float filmstripOpacity_ = 0.0f;
    float filmstripRevealStartOpacity_ = 0.0f;
    ULONGLONG filmstripVisibilityStart_ = 0;
    UINT filmstripHoldDurationMs_ = 2000;
    FilmstripVisibilityState filmstripVisibilityState_ = FilmstripVisibilityState::Hidden;
    int filmstripHoveredIndex_ = -1;
    int filmstripPreviewIndex_ = -1;
    UINT filmstripHoverPreviewDelayMs_ = 250;
    float filmstripHoverPreviewOpacity_ = 0.0f;
    float filmstripHoverPreviewFadeStartOpacity_ = 0.0f;
    ULONGLONG filmstripHoverPreviewFadeStartedAtMs_ = 0;
    bool filmstripHoverPreviewFadeActive_ = false;
    bool filmstripHoverPreviewFadeOut_ = false;
    bool filmstripVideoHoverLoading_ = false;
    bool filmstripHoverDelaySliderDragging_ = false;
    uint64_t filmstripHoverPreviewGeneration_ = 0;
    std::atomic<uint64_t> videoHoverPreviewGeneration_{ 0 };
    uint64_t filmstripHoverPreviewUseSeed_ = 0;
    D2D1_RECT_F filmstripPreviewGeometry_{};
    bool filmstripPreviewGeometryValid_ = false;
    std::vector<FilmstripHoverPreviewEntry> filmstripHoverPreviews_;
    std::optional<FilmstripHoverPreviewEntry> filmstripVideoHoverPreview_;
    LONGLONG filmstripVideoHoverTimestamp_ = 0;
    bool filmstripVideoHoverFadeActive_ = false;
    LONGLONG filmstripVideoHoverFadeStartQpc_ = 0;
    LONGLONG filmstripVideoHoverFadeQpcFrequency_ = 0;
    std::mutex filmstripHoverPreviewMutex_;
    std::condition_variable filmstripHoverPreviewWake_;
    std::deque<FilmstripHoverPreviewRequest> filmstripHoverPreviewQueue_;
    std::thread filmstripHoverPreviewWorker_;
    std::atomic<bool> filmstripHoverPreviewStopping_{ false };
    std::optional<size_t> filmstripClickedRevealTarget_;
    bool filmstripPanelHovered_ = false;
    bool filmstripRevealHovered_ = false;
    bool filmstripHintHovered_ = false;
    uint64_t filmstripThumbnailGenerationSeed_ = 0;
    std::vector<uint64_t> filmstripThumbnailGenerations_;
    std::vector<FilmstripThumbnailEntry> filmstripThumbnails_;
    std::vector<FilmstripThumbnailRequest> filmstripThumbnailPending_;
    std::vector<FilmstripThumbnailRequest> filmstripThumbnailFailures_;
    std::mutex filmstripThumbnailMutex_;
    std::condition_variable filmstripThumbnailWake_;
    std::deque<FilmstripThumbnailRequest> filmstripThumbnailQueue_;
    std::array<std::thread, kFilmstripThumbnailWorkerCount> filmstripThumbnailWorkers_;
    std::atomic<bool> filmstripThumbnailStopping_{ false };
    std::atomic<uint64_t> filmstripThumbnailFolderGeneration_{ 0 };
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
    bool swipeToNavigateWhenFit_ = false;
    bool deleteWarningSuppressOnConfirm_ = false;
    bool showZoomPercentage_ = true;
    ZoomHudPosition zoomHudPosition_ = ZoomHudPosition::BottomRight;
    bool animationsEnabled_ = true;
    bool reverseMouseWheelZoom_ = false;
    bool alwaysShowFilmstrip_ = false;
    bool filmstripDragCandidate_ = false;
    bool filmstripDragging_ = false;
    POINT filmstripDragStart_{};
    double filmstripDragStartScroll_ = 0.0;
    int filmstripDragItem_ = -1;
    size_t filmstripDemandFirst_ = std::numeric_limits<size_t>::max();
    size_t filmstripDemandLast_ = std::numeric_limits<size_t>::max();
    size_t filmstripDemandCurrent_ = std::numeric_limits<size_t>::max();
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
    float helpScroll_ = 0.0f;
    int helpTopic_ = 0;
    int helpTopicHover_ = -1;
    mutable int helpTopicHit_ = -1;
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
    ULONGLONG shellRotationStarted_ = 0;
    bool shellRotationClockwise_ = false;
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
    bool videoWasPlayingBeforeScrub_ = false;
    bool videoCursorHidden_ = false;
    float videoControlsOpacity_ = 0.0f;
    float videoControlsFadeStartOpacity_ = 1.0f;
    double videoScrubSeconds_ = 0.0;
    ULONGLONG videoControlsLastActivity_ = 0;
    ULONGLONG videoControlsFadeStart_ = 0;
    bool videoControlsFadeActive_ = false;
    ULONGLONG videoFullscreenToggleTick_ = 0;
    ButtonKind hoveredButton_ = ButtonKind::None;
    ButtonKind pressedButton_ = ButtonKind::None;
    bool resetInProgress_ = false;
    LONG_PTR fullscreenStyle_ = 0;
    LONG_PTR fullscreenExStyle_ = 0;
    RECT fullscreenRect_{};
    WINDOWPLACEMENT fullscreenPlacement_{ sizeof(WINDOWPLACEMENT) };
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
        if (viewer->HelpContentContains(point)) {
            const float wheelUnits = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
            viewer->ScrollHelp(-wheelUnits * MulDiv(54, GetDpiForWindow(window), 96));
            return 0;
        }
        if (viewer->FilmstripContains(point)) {
            viewer->ScrollFilmstrip(GET_WHEEL_DELTA_WPARAM(wParam));
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
        if (viewer->FilmstripContains(point)) return 0;
        if (viewer->ImageAdjustmentsPanelContains(point)) return 0;
        if (viewer->ConsumeVideoFullscreenButtonDoubleClick()) return 0;
        const ButtonKind videoControl = viewer->VideoControlAt(point);
        if (videoControl == ButtonKind::VideoStepBackward || videoControl == ButtonKind::VideoStepForward) {
            const int direction = videoControl == ButtonKind::VideoStepBackward ? -1 : 1;
            if (viewer->BeginVideoStepHold(direction)) SetCapture(window);
            return 0;
        }
        if (viewer->VideoActive() && viewer->VideoControlsContains(point)) return 0;
        const ButtonKind navigation = viewer->CanvasNavigationZoneAt(point);
        if (navigation != ButtonKind::None) {
            viewer->BeginCanvasNavigationClick(navigation, point);
            SetCapture(window);
            return 0;
        }
        if (viewer->VideoActive()) {
            if (viewer->VideoCanvasContains(point)) { viewer->ToggleVideoFullscreen(); return 0; }
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
        viewer->HideFilmstripHoverPreviewImmediately();
        viewer->SetFilmstripHover({ -1, -1 });
        if (viewer->BeginFilmstripHoverDelaySlider(point)) {
            SetCapture(window);
            return 0;
        }
        if (viewer->BeginFilmstripInteraction(point)) {
            SetCapture(window);
            return 0;
        }
        if (viewer->BeginVideoControlsInteraction(point)) {
            SetCapture(window);
            return 0;
        }
        if (viewer->BeginImageAdjustmentsInteraction(point)) {
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
            else if (!viewer->BeginSwipeNavigation(point)) viewer->BeginPan(point);
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
        if (!viewer->TutorialActive() && !viewer->HasOverlay() && !viewer->DropdownOpen() && !viewer->ContextMenuOpen()) {
            const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (viewer->VideoActive()) {
                if (!viewer->VideoControlsContains(point) && viewer->VideoCanvasContains(point)) { viewer->ToggleVideoFullscreen(); return 0; }
            }
            if (viewer->HasImage() && viewer->ImageContains(point)) {
                viewer->ToggleFitActualPixels(point);
                return 0;
            }
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
            viewer->SetCanvasNavigationHover(viewer->VideoControlsContains(point) ? ButtonKind::None : viewer->CanvasNavigationZoneAt(point));
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
            TrackMouseEvent(&track);
            if (viewer->CanvasNavigationPressed()) {
                viewer->ContinueCanvasNavigationClick(point);
                return 0;
            }
            if (viewer->SwipeNavigationPending()) return 0;
            if (!viewer->HamburgerPressed() && viewer->PressedButton() == ButtonKind::None) viewer->PanTo(point);
            return 0;
        }
        const FrameMetrics frame = GetFrameMetrics(window);
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (viewer->FilmstripHoverDelaySliderDragging()) viewer->UpdateFilmstripHoverDelaySlider(point);
        viewer->SetFilmstripPointerState(point);
        viewer->SetFilmstripHover(point);
        if (viewer->ContinueFilmstripInteraction(point)) return 0;
        viewer->SetButtonHover(viewer->ButtonAt(point));
        viewer->SetCanvasNavigationHover(viewer->CanvasNavigationZoneAt(point));
        viewer->SetHamburgerHover(!viewer->IsFullscreen() && PtInRect(&frame.hamburger, point));
        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
        TrackMouseEvent(&track);
        if (viewer->CanvasNavigationPressed()) {
            viewer->ContinueCanvasNavigationClick(point);
            return 0;
        }
        if (viewer->SwipeNavigationPending()) return 0;
        if (viewer->ContinueImageAdjustmentsInteraction(point)) return 0;
        if (!viewer->HamburgerPressed() && viewer->PressedButton() == ButtonKind::None) {
            if (viewer->ModelActive()) {
                if (wParam & (MK_LBUTTON | MK_MBUTTON)) viewer->ContinueModelDrag(point);
                else viewer->EndModelDrag();
            } else viewer->PanTo(point);
        }
        return 0;
    }
    case WM_MOUSELEAVE: viewer->UpdateTriangleCountTooltipHover({ -1, -1 }); viewer->SetHamburgerHover(false); viewer->SetButtonHover(ButtonKind::None); viewer->SetCanvasNavigationHover(ButtonKind::None); viewer->SetDropdownHover(DropdownItem::None); viewer->SetContextHover(ContextAction::None); viewer->SetFilmstripPointerState({ -1, -1 }); viewer->SetFilmstripHover({ -1, -1 }); viewer->VideoControlsMouseLeave(); return 0;
    case WM_LBUTTONUP: {
        if (viewer->FilmstripHoverDelaySliderDragging()) {
            viewer->EndFilmstripHoverDelaySlider();
            if (GetCapture() == window) ReleaseCapture();
            return 0;
        }
        if (viewer->FilmstripInteractionActive()) {
            viewer->EndFilmstripInteraction({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            if (GetCapture() == window) ReleaseCapture();
            return 0;
        }
        if (viewer->EndVideoControlsInteraction({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) })) {
            if (GetCapture() == window) ReleaseCapture();
            return 0;
        }
        if (viewer->EndImageAdjustmentsInteraction({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) })) {
            if (GetCapture() == window) ReleaseCapture();
            return 0;
        }
        if (viewer->CanvasNavigationPressed()) {
            const ButtonKind navigation = viewer->FinishCanvasNavigationClick({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            if (GetCapture() == window) ReleaseCapture();
            if (navigation != ButtonKind::None) viewer->InvokeButton(navigation);
            return 0;
        }
        if (viewer->FinishSwipeNavigation({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) })) {
            if (GetCapture() == window) ReleaseCapture();
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
        viewer->EndPan(); viewer->EndModelDrag(); viewer->EndFilmstripHoverDelaySlider(); viewer->CancelFilmstripInteraction(); viewer->CancelSwipeNavigation(); viewer->CancelCanvasNavigationClick(); viewer->CancelVideoControlsInteraction(); viewer->ClearCaptionButtonPressed(); viewer->ClearButtonPressed(); viewer->SetHamburgerPressed(false); viewer->ClearDropdownPressed(); viewer->ClearContextPressed(); return 0;
    case WM_RBUTTONUP: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        viewer->HideFilmstripHoverPreviewImmediately();
        viewer->SetFilmstripHover({ -1, -1 });
        if (!viewer->TutorialActive()) { if (viewer->ModelActive()) viewer->SelectModelFace(point); viewer->OpenContextMenu(point); }
        return 0;
    }
    case WM_TIMER:
#ifdef _DEBUG
        { wchar_t timerMessage[128]{}; swprintf_s(timerMessage, L"[Viewtrious] VIEWTRIOUS_WM_TIMER_RECEIVED id=%zu hwnd=%p\n", static_cast<size_t>(wParam), window); OutputDebugStringW(timerMessage); }
#endif
        if (wParam == kGifPlaybackTimer) { viewer->GifPlaybackTimerMessage(); return 0; }
        if (wParam == kCopyFeedbackTimer) { viewer->UpdateCopyFeedback(); return 0; }
        if (wParam == kCanvasNavigationFadeTimer) { viewer->UpdateCanvasNavigationFade(); return 0; }
        if (wParam == kDirectoryChangeDebounceTimer) { KillTimer(window, kDirectoryChangeDebounceTimer); viewer->RefreshNavigationFromFileSystem(); return 0; }
        if (wParam == kNavigationDecodeDebounceTimer) { viewer->NavigationDecodeTimer(); return 0; }
        if (wParam == kImageAdjustmentPersistenceTimer) { KillTimer(window, kImageAdjustmentPersistenceTimer); viewer->ImageAdjustmentPersistenceTimer(); return 0; }
        if (wParam == kShellRotationCheckTimer) { viewer->ShellRotationTimer(); return 0; }
        if (wParam == kHeifRotationMenuRefreshTimer) { viewer->HeifRotationMenuRefreshTimer(); return 0; }
        if (wParam == kLanczosSettleTimer) { KillTimer(window, kLanczosSettleTimer); viewer->LanczosRefinementTimer(); return 0; }
        if (wParam == kModelHomeAnimationTimer) { viewer->UpdateAnimatedModelHome(); return 0; }
        if (wParam == kModelLoadingAnimationTimer) { viewer->ModelLoadingAnimationTimerMessage(); return 0; }
        if (wParam == kTriangleCountTooltipTimer) { viewer->TriangleCountTooltipTimerMessage(); return 0; }
        if (wParam == kVideoControlsTimer) { viewer->UpdateVideoControlsFade(); return 0; }
        if (wParam == kVideoStepHoldTimer) { viewer->UpdateVideoStepHold(); return 0; }
        if (wParam == kStillDissolveTimer) { viewer->UpdateStillDissolve(); return 0; }
        if (wParam == kFilmstripVisibilityTimer) { viewer->UpdateFilmstripVisibility(); return 0; }
        if (wParam == kFilmstripHoverPreviewDwellTimer) { viewer->BeginFilmstripHoverPreviewDecode(); return 0; }
        if (wParam == kFilmstripHoverPreviewTimer) { viewer->ShowFilmstripHoverPreview(); return 0; }
        if (wParam == kFilmstripVideoHoverFadeTimer) { viewer->UpdateFilmstripVideoHoverFade(); return 0; }
        if (wParam == kFilmstripHoverPreviewFadeTimer) { viewer->UpdateFilmstripHoverPreviewFade(); return 0; }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {
            viewer->RefreshNavigationFromFileSystem();
            viewer->ResumePendingTour();
        } else {
            viewer->SetVideoPlaybackSpeedPanelOpen(false);
            viewer->CancelVideoControlsInteraction();
        }
        break;
    case WM_SHOWWINDOW:
        viewer->GifPlaybackVisibilityChanged(wParam != FALSE && !IsIconic(window));
        break;
    case WM_SETTINGCHANGE: ApplyTitleBarTheme(window); return 0;
    case kBuildNavigationMessage: viewer->BuildNavigation(); return 0;
    case kFilmstripThumbnailCompleteMessage: viewer->FilmstripThumbnailCompleteMessage(reinterpret_cast<FilmstripThumbnailResult*>(lParam)); return 0;
    case kFilmstripHoverPreviewCompleteMessage: viewer->FilmstripHoverPreviewCompleteMessage(reinterpret_cast<FilmstripHoverPreviewResult*>(lParam)); return 0;
    case kFilmstripScrollWakeMessage: viewer->FilmstripScrollWakeMessage(static_cast<uint64_t>(wParam)); return 0;
    case kDirectoryChangedMessage: viewer->QueueDirectoryRefreshFromWatcher(); return 0;
    case kFullDecodeCompleteMessage: viewer->FullDecodeCompleteMessage(reinterpret_cast<FullDecodeResult*>(lParam)); return 0;
    case kLanczosCompleteMessage: viewer->LanczosCompleteMessage(reinterpret_cast<LanczosResult*>(lParam)); return 0;
    case kDecodeWorkerFinishedMessage: viewer->DecodeWorkerFinishedMessage(reinterpret_cast<DecodeWorkerFinished*>(lParam)); return 0;
    case kModelLoadCompleteMessage: viewer->ModelLoadCompleteMessage(reinterpret_cast<ModelLoadResult*>(lParam)); return 0;
    case kVideoMediaEngineEventMessage: viewer->VideoMediaEngineEvent(static_cast<DWORD>(wParam)); return 0;
    case kVideoPlaybackWakeMessage: viewer->VideoPlaybackWakeMessage(static_cast<uint64_t>(wParam)); return 0;
    case kAiAnalysisCompleteMessage: viewer->AiAnalysisCompleteMessage(reinterpret_cast<AiAnalysisResult*>(lParam)); return 0;
    case kImageAdjustmentPersistenceCompleteMessage: viewer->ImageAdjustmentPersistenceCompleteMessage(reinterpret_cast<ImageAdjustmentPersistenceResult*>(lParam)); return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && viewer->VideoPlaybackSpeedPanelOpen()) { viewer->SetVideoPlaybackSpeedPanelOpen(false); return 0; }
        if (wParam == VK_ESCAPE && viewer->VideoAdjustmentsPanelOpen()) { viewer->SetVideoAdjustmentsPanelOpen(false); return 0; }
        if (wParam == VK_ESCAPE && viewer->ImageAdjustmentsPanelOpen()) { viewer->SetImageAdjustmentsPanelOpen(false); return 0; }
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
        if (viewer->DeleteConfirmationOpen() && wParam == VK_RETURN) {
            viewer->InvokeButton(ButtonKind::DeleteConfirm);
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
