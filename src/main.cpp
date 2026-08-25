#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

constexpr wchar_t kWindowClass[] = L"FeatherViewWindow";
constexpr wchar_t kWindowTitle[] = L"FeatherView";
constexpr UINT kBuildNavigationMessage = WM_APP + 1;
constexpr float kMaximumZoom = 16.0f;
constexpr float kZoomStep = 1.20f;
constexpr wchar_t kSettingsKey[] = L"Software\\FeatherView";
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
const D2D1_COLOR_F kViewerBackground = D2D1::ColorF(26.0f / 255.0f, 26.0f / 255.0f, 26.0f / 255.0f);

enum class OverlayKind { None, KeyboardShortcuts, About };
enum class DropdownItem { None, KeyboardShortcuts, About, Close };

struct ShortcutEntry { const wchar_t* shortcut; const wchar_t* description; };
constexpr ShortcutEntry kShortcutEntries[] = {
    { L"Left Arrow", L"Previous image" }, { L"Right Arrow", L"Next image" }, { L"Mouse Wheel", L"Zoom" },
    { L"+ / =", L"Zoom in" }, { L"-", L"Zoom out" }, { L"0", L"Reset zoom and center" },
    { L"Left mouse drag", L"Pan" }, { L"Double-click image", L"Toggle fullscreen" }, { L"F11", L"Toggle fullscreen" },
    { L"Esc", L"Exit fullscreen, or close FeatherView" },
};
constexpr size_t kShortcutEntryCount = sizeof(kShortcutEntries) / sizeof(kShortcutEntries[0]);

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
        swprintf_s(message, L"FeatherView startup: %s: %.2f ms\n", label, elapsedMs);
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
        extension == L".heic" || extension == L".heif" || extension == L".avif";
}

bool PathsEqual(const fs::path& left, const fs::path& right) {
    const std::wstring leftText = left.lexically_normal().wstring();
    const std::wstring rightText = right.lexically_normal().wstring();
    return CompareStringOrdinal(leftText.c_str(), static_cast<int>(leftText.size()),
        rightText.c_str(), static_cast<int>(rightText.size()), TRUE) == CSTR_EQUAL;
}

bool ReadSetting(const wchar_t* name, DWORD& value) {
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
}

struct SavedPlacement {
    RECT rect{};
    bool maximized = false;
};

bool LoadPlacement(SavedPlacement& placement) {
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

bool UseDarkAppMode() {
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

FrameMetrics GetFrameMetrics(HWND window) {
    const UINT dpi = GetDpiForWindow(window);
    const int titleBarHeight = MulDiv(40, dpi, 96);
    const int border = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    const int buttonWidth = MulDiv(46, dpi, 96);
    const int hamburgerWidth = MulDiv(46, dpi, 96);
    const int separatorWidth = MulDiv(1, dpi, 96);
    const int separatorHeight = MulDiv(20, dpi, 96);
    const int sectionGutter = MulDiv(14, dpi, 96);
    const int hamburgerSeparatorInset = MulDiv(4, dpi, 96);
    const int resolutionWidth = MulDiv(92, dpi, 96);
    const int fileSizeWidth = MulDiv(72, dpi, 96);
    RECT client{};
    GetClientRect(window, &client);
    const int buttonLeft = std::max(0L, client.right - buttonWidth * 3);
    const int separatorTop = std::max(0, (titleBarHeight - separatorHeight) / 2);
    const int hamburgerSeparatorLeft = hamburgerWidth - hamburgerSeparatorInset;
    const int resolutionLeft = hamburgerSeparatorLeft + separatorWidth + sectionGutter;
    const int resolutionSeparatorLeft = resolutionLeft + resolutionWidth + sectionGutter;
    const int fileSizeLeft = resolutionSeparatorLeft + separatorWidth + sectionGutter;
    const int fileSizeSeparatorLeft = fileSizeLeft + fileSizeWidth + sectionGutter;
    const int filenameLeft = fileSizeSeparatorLeft + separatorWidth + sectionGutter;
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
        if (!path.empty()) return LoadImage(path);
        error_ = L"Drop an image here, or launch FeatherView with an image path.";
        return S_OK;
    }

    HRESULT LoadImage(const std::wstring& path) {
        ComPtr<IWICBitmapSource> source;
        UINT width = 0;
        UINT height = 0;
        const HRESULT hr = DecodeImage(path, source, width, height);
        if (SUCCEEDED(hr)) {
            CommitImage(path, source, width, height, true);
        } else {
            source_.Reset();
            bitmap_.Reset();
            imageWidth_ = imageHeight_ = 0;
            currentPath_.clear();
            resolutionText_.clear();
            fileSizeText_.clear();
            filenameText_.clear();
            navigationFiles_.clear();
            navigationBuilt_ = false;
            error_ = L"Unable to open this image. It may be corrupt or use an unsupported codec.";
        }
        return hr;
    }

    void SetWindow(HWND window) { window_ = window; }
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
        hamburgerPressed_ = pressed;
        InvalidateRect(window_, nullptr, FALSE);
    }
    bool HamburgerPressed() const { return hamburgerPressed_; }
    bool DropdownOpen() const { return dropdownOpen_; }
    void ToggleDropdown() {
        if (fullscreen_) return;
        dropdownOpen_ = !dropdownOpen_;
        if (dropdownOpen_) DismissOverlay();
        dropdownHovered_ = DropdownItem::None;
        dropdownPressed_ = DropdownItem::None;
        InvalidateRect(window_, nullptr, FALSE);
    }
    void DismissDropdown() {
        if (!dropdownOpen_) return;
        dropdownOpen_ = false;
        dropdownHovered_ = DropdownItem::None;
        dropdownPressed_ = DropdownItem::None;
        InvalidateRect(window_, nullptr, FALSE);
    }
    DropdownItem DropdownItemAt(POINT point) const {
        if (!dropdownOpen_) return DropdownItem::None;
        const RECT bounds = GetDropdownBounds();
        if (!PtInRect(&bounds, point)) return DropdownItem::None;
        const int rowHeight = MulDiv(38, GetDpiForWindow(window_), 96);
        const int firstRowTop = bounds.top + MulDiv(4, GetDpiForWindow(window_), 96);
        const int firstRowBottom = firstRowTop + rowHeight;
        const int secondRowTop = firstRowBottom + MulDiv(9, GetDpiForWindow(window_), 96);
        if (point.y >= firstRowTop && point.y < firstRowBottom) return DropdownItem::KeyboardShortcuts;
        if (point.y < secondRowTop) return DropdownItem::None;
        if (point.y < secondRowTop + rowHeight) return DropdownItem::About;
        return DropdownItem::Close;
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
    void ClearDropdownPressed() { SetDropdownPressed(DropdownItem::None); }
    void InvokeDropdownItem(DropdownItem item) {
        DismissDropdown();
        if (item == DropdownItem::KeyboardShortcuts) ShowOverlay(OverlayKind::KeyboardShortcuts);
        else if (item == DropdownItem::About) ShowOverlay(OverlayKind::About);
        else if (item == DropdownItem::Close) SendMessageW(window_, WM_SYSCOMMAND, SC_CLOSE, 0);
    }
    bool HasOverlay() const { return overlay_ != OverlayKind::None; }
    void ShowOverlay(OverlayKind overlay) {
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
    void ToggleFullscreen() {
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
        EnsureRenderTarget();
        if (renderTarget_) {
            renderTarget_->BeginDraw();
            renderTarget_->Clear(kViewerBackground);            if (source_) {
                EnsureBitmap();
                if (bitmap_) DrawImage();
            }
            DrawTitleBar();
            DrawDropdown();
            DrawOverlay();
            const HRESULT hr = renderTarget_->EndDraw();
            if (SUCCEEDED(hr) && bitmap_) MarkFirstPresentation();
            if (hr == D2DERR_RECREATE_TARGET) DiscardRenderResources();
        }
        if (!source_ && !error_.empty()) DrawErrorText(paint.hdc);
        EndPaint(window_, &paint);
    }

    void Resize() {
        if (renderTarget_) {
            RECT client{};
            GetClientRect(window_, &client);
            renderTarget_->Resize(D2D1::SizeU(std::max(1L, client.right - client.left),
                std::max(1L, client.bottom - client.top)));
        }
        if (!fitToWindow_ && zoom_ < BaseScale()) FitToWindow();
        InvalidateRect(window_, nullptr, FALSE);
    }

    SIZE SuggestedClientSize() const {
        if (!source_) return { 900, 650 };
        constexpr double maxWidth = 1280.0;
        constexpr double maxHeight = 900.0;
        const double scale = std::min({ 1.0, maxWidth / imageWidth_, maxHeight / imageHeight_ });
        return { static_cast<LONG>(std::lround(imageWidth_ * scale)),
                 static_cast<LONG>(std::lround(imageHeight_ * scale)) };
    }

    void DropFile(HDROP drop) {
        const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
        if (length > 0) {
            std::wstring path(length + 1, L'\0');
            DragQueryFileW(drop, 0, path.data(), length + 1);
            path.resize(length);
            LoadImage(path);
            InvalidateRect(window_, nullptr, FALSE);
        }
        DragFinish(drop);
    }

    void BuildNavigation() {
        navigationBuildQueued_ = false;
        if (navigationBuilt_ || currentPath_.empty()) return;

        const fs::path current(currentPath_);
        std::error_code error;
        fs::directory_iterator iterator(current.parent_path(), error);
        for (; !error && iterator != fs::directory_iterator(); iterator.increment(error)) {
            std::error_code typeError;
            if (iterator->is_regular_file(typeError) && !typeError && IsSupportedExtension(iterator->path())) {
                navigationFiles_.push_back(iterator->path());
            }
        }
        std::sort(navigationFiles_.begin(), navigationFiles_.end(), [](const fs::path& left, const fs::path& right) {
            const int comparison = StrCmpLogicalW(left.filename().c_str(), right.filename().c_str());
            return comparison == 0 ? left.wstring() < right.wstring() : comparison < 0;
        });
        if (std::none_of(navigationFiles_.begin(), navigationFiles_.end(),
                [&current](const fs::path& path) { return PathsEqual(path, current); })) {
            navigationFiles_.push_back(current);
            std::sort(navigationFiles_.begin(), navigationFiles_.end(), [](const fs::path& left, const fs::path& right) {
                const int comparison = StrCmpLogicalW(left.filename().c_str(), right.filename().c_str());
                return comparison == 0 ? left.wstring() < right.wstring() : comparison < 0;
            });
        }
        navigationBuilt_ = true;
    }

    void Navigate(int direction) {
        if (!source_) return;
        BuildNavigation();
        if (navigationFiles_.size() < 2) return;

        const fs::path current(currentPath_);
        auto currentIt = std::find_if(navigationFiles_.begin(), navigationFiles_.end(),
            [&current](const fs::path& path) { return PathsEqual(path, current); });
        if (currentIt == navigationFiles_.end()) return;

        const ptrdiff_t count = static_cast<ptrdiff_t>(navigationFiles_.size());
        const ptrdiff_t start = std::distance(navigationFiles_.begin(), currentIt);
        for (ptrdiff_t attempt = 1; attempt < count; ++attempt) {
            ptrdiff_t index = (start + direction * attempt) % count;
            if (index < 0) index += count;
            ComPtr<IWICBitmapSource> source;
            UINT width = 0;
            UINT height = 0;
            const std::wstring path = navigationFiles_[index].wstring();
            if (SUCCEEDED(DecodeImage(path, source, width, height))) {
                CommitImage(path, source, width, height, false);
                InvalidateRect(window_, nullptr, FALSE);
                return;
            }
        }
    }

    void ZoomAt(POINT cursor, float factor) {
        if (!source_) return;
        const float oldScale = CurrentScale();
        const float baseScale = BaseScale();
        const float newScale = std::clamp(oldScale * factor, baseScale, kMaximumZoom);
        if (newScale <= baseScale + 0.0001f) {
            FitToWindow();
            return;
        }
        if (std::abs(newScale - oldScale) < 0.0001f) return;

        const D2D1_SIZE_F target = ClientSize();
        const D2D1_POINT_2F oldTopLeft = ImageTopLeft(oldScale, target);
        const float ratio = newScale / oldScale;
        pan_.x = static_cast<float>(cursor.x) - (static_cast<float>(cursor.x) - oldTopLeft.x) * ratio +
            imageWidth_ * newScale / 2.0f - target.width / 2.0f;
        pan_.y = static_cast<float>(cursor.y) - (static_cast<float>(cursor.y) - oldTopLeft.y) * ratio +
            imageHeight_ * newScale / 2.0f - target.height / 2.0f;
        fitToWindow_ = false;
        zoom_ = newScale;
        InvalidateRect(window_, nullptr, FALSE);
    }

    void ZoomCentered(float factor) {
        const D2D1_SIZE_F client = ClientSize();
        ZoomAt({ static_cast<LONG>(client.width / 2.0f), static_cast<LONG>(client.height / 2.0f) }, factor);
    }

    void FitToWindow() {
        if (!source_) return;
        fitToWindow_ = true;
        pan_ = D2D1::Point2F();
        InvalidateRect(window_, nullptr, FALSE);
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
        InvalidateRect(window_, nullptr, FALSE);
    }

    void EndPan() {
        if (!dragging_) return;
        dragging_ = false;
        if (GetCapture() == window_) ReleaseCapture();
    }

    void MarkFirstPresentation() {
        if (!presented_) {
            presented_ = true;
            timer_.Log(L"first successful image presentation");
            if (!currentPath_.empty() && !navigationBuildQueued_) {
                navigationBuildQueued_ = true;
                PostMessageW(window_, kBuildNavigationMessage, 0, 0);
            }
        }
    }

    void SaveWindowPlacement() const {
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

private:
    RECT GetDropdownBounds() const {
        RECT client{};
        GetClientRect(window_, &client);
        const UINT dpi = GetDpiForWindow(window_);
        const FrameMetrics frame = GetFrameMetrics(window_);
        const LONG margin = MulDiv(4, dpi, 96);
        const LONG width = std::min<LONG>(MulDiv(236, dpi, 96), std::max<LONG>(1, client.right - margin * 2));
        const LONG height = MulDiv(131, dpi, 96);
        const LONG left = std::clamp<LONG>(frame.hamburger.left + margin, margin,
            std::max<LONG>(margin, client.right - width - margin));
        const LONG top = frame.hamburger.bottom + margin;
        return { left, top, left + width, std::min<LONG>(client.bottom - margin, top + height) };
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
                    if (SUCCEEDED(hr)) source = converter;
                } else if (SUCCEEDED(hr)) {
                    hr = E_FAIL;
                }
            }
        }
        timer_.Log(L"decode complete");
        return hr;
    }

    void CommitImage(const std::wstring& path, const ComPtr<IWICBitmapSource>& source, UINT width, UINT height,
        bool resetNavigation) {
        source_ = source;
        bitmap_.Reset();
        imageWidth_ = width;
        imageHeight_ = height;
        currentPath_ = path;
        resolutionText_ = std::to_wstring(width) + L"\u00D7" + std::to_wstring(height);
        fileSizeText_ = FormatFileSize(path);
        filenameText_ = fs::path(path).filename().wstring();
        error_.clear();
        fitToWindow_ = true;
        zoom_ = 1.0f;
        pan_ = D2D1::Point2F();
        EndPan();
        if (resetNavigation) {
            navigationFiles_.clear();
            navigationBuilt_ = false;
            navigationBuildQueued_ = false;
        }
    }

    void EnsureRenderTarget() {
        if (renderTarget_) return;
        const D2D1_SIZE_F client = ClientSize();
        d2dFactory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(window_, D2D1::SizeU(
                static_cast<UINT32>(std::max(1.0f, client.width)),
                static_cast<UINT32>(std::max(1.0f, client.height)))), &renderTarget_);
        timer_.Log(L"rendering/window initialization complete");
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

    float BaseScale() const {
        if (!source_) return 1.0f;
        const D2D1_SIZE_F target = ClientSize();
        const float fitScale = std::min(target.width / static_cast<float>(imageWidth_),
            target.height / static_cast<float>(imageHeight_));
        return std::min(1.0f, fitScale);
    }

    float CurrentScale() const { return fitToWindow_ ? BaseScale() : std::max(zoom_, BaseScale()); }

    D2D1_POINT_2F ImageTopLeft(float scale, const D2D1_SIZE_F& target) const {
        return D2D1::Point2F((target.width - imageWidth_ * scale) / 2.0f + pan_.x,
            (target.height - imageHeight_ * scale) / 2.0f + pan_.y);
    }

    bool CanPan() const {
        return source_.Get() != nullptr;
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
        const D2D1_SIZE_F target = renderTarget_->GetSize();
        const D2D1_RECT_F visible = D2D1::RectF(std::max(bounds.left, 0.0f), std::max(bounds.top, 0.0f),
            std::min(bounds.right, target.width), std::min(bounds.bottom, target.height));
        if (visible.right <= visible.left || visible.bottom <= visible.top) return;
        renderTarget_->PushAxisAlignedClip(visible, D2D1_ANTIALIAS_MODE_ALIASED);
        renderTarget_->FillRectangle(visible, checkerboardBrush_.Get());
        renderTarget_->PopAxisAlignedClip();
    }
    void DrawImage() {
        const D2D1_SIZE_F target = renderTarget_->GetSize();
        const float scale = CurrentScale();
        const D2D1_POINT_2F topLeft = ImageTopLeft(scale, target);
        const D2D1_RECT_F destination = D2D1::RectF(topLeft.x, topLeft.y,
            topLeft.x + imageWidth_ * scale, topLeft.y + imageHeight_ * scale);
        DrawCheckerboard(destination);
        renderTarget_->DrawBitmap(bitmap_.Get(), destination, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
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

    RECT GetOverlayBounds() const {
        RECT client{};
        GetClientRect(window_, &client);
        if (!HasOverlay()) return {};
        const UINT dpi = GetDpiForWindow(window_);
        const int panelPadding = MulDiv(24, dpi, 96);
        const int titleHeight = MulDiv(24, dpi, 96);
        const int titleGap = MulDiv(14, dpi, 96);
        const int rowHeight = MulDiv(25, dpi, 96);
        const int desiredWidth = MulDiv(overlay_ == OverlayKind::KeyboardShortcuts ? 460 : 720, dpi, 96);
        const int aboutLogoWidth = std::min(MulDiv(520, dpi, 96), desiredWidth - panelPadding * 2);
        const int aboutLogoHeight = MulDiv(aboutLogoWidth, 941, 1672);
        const int desiredHeight = overlay_ == OverlayKind::KeyboardShortcuts
            ? panelPadding + titleHeight + titleGap + static_cast<int>(kShortcutEntryCount) * rowHeight + panelPadding
            : panelPadding + aboutLogoHeight + MulDiv(16, dpi, 96) + MulDiv(26, dpi, 96) + MulDiv(5, dpi, 96) +
                MulDiv(20, dpi, 96) + MulDiv(20, dpi, 96) + MulDiv(18, dpi, 96) + panelPadding;
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
        DWRITE_FONT_WEIGHT weight, ID2D1Brush* brush, bool verticallyCenter = false) {
        ComPtr<IDWriteTextFormat> format;
        const float dpiScale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        if (FAILED(dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size * dpiScale, L"", &format))) return;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (verticallyCenter) format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)), format.Get(), width, height, &layout))) return;
        renderTarget_->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    bool EnsureAboutLogo() {
        if (aboutLogo_) return true;
        wchar_t modulePath[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath))) return false;
        ComPtr<IWICBitmapSource> source;
        UINT width = 0, height = 0;
        if (FAILED(DecodeImage((fs::path(modulePath).parent_path() / L"FeatherViewLogo.png").wstring(), source, width, height))) return false;
        return SUCCEEDED(renderTarget_->CreateBitmapFromWicBitmap(source.Get(), nullptr, &aboutLogo_));
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
        if (overlay_ == OverlayKind::KeyboardShortcuts) {
            DrawOverlayText(L"Keyboard Shortcuts", left, static_cast<float>(bounds.top) + panelPadding,
                contentWidth, 24.0f * dpiScale, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
            const float shortcutWidth = 154.0f * dpiScale;
            float y = static_cast<float>(bounds.top) + panelPadding + 38.0f * dpiScale;
            for (const ShortcutEntry& line : kShortcutEntries) {
                DrawOverlayText(line.shortcut, left, y, shortcutWidth, 18.0f * dpiScale, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
                DrawOverlayText(line.description, left + shortcutWidth, y, contentWidth - shortcutWidth,
                    18.0f * dpiScale, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get());
                y += 25.0f * dpiScale;
            }
        } else {
            float logoBottom = static_cast<float>(bounds.top) + panelPadding;
            if (EnsureAboutLogo()) {
                const D2D1_SIZE_F logoSource = aboutLogo_->GetSize();
                const float logoWidth = std::min(520.0f * dpiScale, contentWidth);
                const float logoHeight = logoWidth * logoSource.height / logoSource.width;
                const float logoLeft = static_cast<float>(bounds.left) + (bounds.right - bounds.left - logoWidth) / 2.0f;
                const float logoTop = logoBottom;
                renderTarget_->DrawBitmap(aboutLogo_.Get(), D2D1::RectF(logoLeft, logoTop, logoLeft + logoWidth, logoTop + logoHeight));
                logoBottom = logoTop + logoHeight;
            }
            const float titleTop = logoBottom + 16.0f * dpiScale;
            DrawOverlayText(L"FeatherView", left, titleTop, contentWidth, 26.0f * dpiScale,
                18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
            DrawOverlayText(L"Version " FEATHERVIEW_VERSION, left, titleTop + 31.0f * dpiScale, contentWidth, 20.0f * dpiScale,
                12.5f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get());
            DrawOverlayText(L"Extremely lightweight image viewer", left, static_cast<float>(bounds.bottom) - panelPadding - 18.0f * dpiScale,
                contentWidth, 18.0f * dpiScale, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get());
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
        const int separatorGap = MulDiv(9, dpi, 96);
        const auto row = [&](int top) { return D2D1::RectF(static_cast<float>(bounds.left + 1), static_cast<float>(top),
            static_cast<float>(bounds.right - 1), static_cast<float>(top + rowHeight)); };
        const int shortcutsTop = bounds.top + 4;
        const int aboutTop = shortcutsTop + rowHeight + separatorGap;
        const int closeTop = aboutTop + rowHeight;
        const auto drawItem = [&](DropdownItem item, int top, const wchar_t* label) {
            if (dropdownPressed_ == item) renderTarget_->FillRectangle(row(top), pressedBrush.Get());
            else if (dropdownHovered_ == item) renderTarget_->FillRectangle(row(top), hoverBrush.Get());
            DrawOverlayText(label, static_cast<float>(bounds.left + MulDiv(14, dpi, 96)), static_cast<float>(top),
                static_cast<float>(bounds.right - bounds.left - MulDiv(28, dpi, 96)), static_cast<float>(rowHeight),
                13.0f, DWRITE_FONT_WEIGHT_NORMAL, textBrush.Get(), true);
        };
        drawItem(DropdownItem::KeyboardShortcuts, shortcutsTop, L"Keyboard Shortcuts");
        const float separatorY = static_cast<float>(shortcutsTop + rowHeight + separatorGap / 2);
        renderTarget_->DrawLine(D2D1::Point2F(static_cast<float>(bounds.left + MulDiv(12, dpi, 96)), separatorY),
            D2D1::Point2F(static_cast<float>(bounds.right - MulDiv(12, dpi, 96)), separatorY), borderBrush.Get(), 1.0f);
        drawItem(DropdownItem::About, aboutTop, L"About");
        drawItem(DropdownItem::Close, closeTop, L"Close");
        renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(menu, 7.0f, 7.0f), borderBrush.Get(), 1.0f);
    }

    void DrawTitleBar() {
        if (fullscreen_) return;

        const FrameMetrics frame = GetFrameMetrics(window_);
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

        DrawTitleText(resolutionText_, static_cast<float>(frame.resolutionLeft), static_cast<float>(frame.resolutionWidth), metadataBrush.Get(), false, true);
        DrawTitleText(fileSizeText_, static_cast<float>(frame.fileSizeLeft), static_cast<float>(frame.fileSizeWidth), metadataBrush.Get(), false, true);
        const float filenameWidth = static_cast<float>(std::max(0L,
            frame.titleBarContent.right - frame.filenameLeft - MulDiv(8, GetDpiForWindow(window_), 96)));
        DrawTitleText(filenameText_, static_cast<float>(frame.filenameLeft), filenameWidth, filenameBrush.Get(), true, false);

        const float dpiScale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        const float stroke = 1.0f;
        const auto pixelCenter = [](float value) { return std::floor(value) + 0.5f; };
        renderTarget_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        const D2D1_POINT_2F hamburgerCenter = D2D1::Point2F(frame.hamburgerSeparator.left / 2.0f,
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

    void DrawErrorText(HDC dc) const {
        RECT client{};
        GetClientRect(window_, &client);
        RECT text{ 0, 0, std::max(1L, client.right - client.left - 48), client.bottom - client.top };
        SetTextColor(dc, RGB(220, 220, 220));
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, error_.c_str(), -1, &text, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
        OffsetRect(&text, (client.right - client.left - (text.right - text.left)) / 2,
            (client.bottom - client.top - (text.bottom - text.top)) / 2);
        DrawTextW(dc, error_.c_str(), -1, &text, DT_CENTER | DT_WORDBREAK);
    }

    void DiscardRenderResources() {
        bitmap_.Reset();
        aboutLogo_.Reset();
        checkerboardBrush_.Reset();
        checkerboardBitmap_.Reset();
        checkerboardDpi_ = 0;
        renderTarget_.Reset();
    }

    const StartupTimer& timer_;
    HWND window_ = nullptr;
    ComPtr<IWICImagingFactory> wicFactory_;
    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IWICBitmapSource> source_;
    ComPtr<ID2D1HwndRenderTarget> renderTarget_;
    ComPtr<ID2D1Bitmap> bitmap_;
    ComPtr<ID2D1Bitmap> aboutLogo_;
    ComPtr<ID2D1Bitmap> checkerboardBitmap_;
    ComPtr<ID2D1BitmapBrush> checkerboardBrush_;
    UINT checkerboardDpi_ = 0;
    ComPtr<IDWriteTextFormat> titleTextFormat_;
    UINT titleTextDpi_ = 0;
    ComPtr<IDWriteTextFormat> captionIconFormat_;
    UINT captionIconDpi_ = 0;
    UINT imageWidth_ = 0;
    UINT imageHeight_ = 0;
    std::wstring currentPath_;
    std::wstring resolutionText_;
    std::wstring fileSizeText_;
    std::wstring filenameText_;
    std::wstring error_;
    std::vector<fs::path> navigationFiles_;
    D2D1_POINT_2F pan_ = D2D1::Point2F();
    POINT lastDragPoint_{};
    float zoom_ = 1.0f;
    bool fitToWindow_ = true;
    bool dragging_ = false;
    bool presented_ = false;
    bool navigationBuilt_ = false;
    bool navigationBuildQueued_ = false;
    bool fullscreen_ = false;
    CaptionButton hoveredCaptionButton_ = CaptionButton::None;
    CaptionButton pressedCaptionButton_ = CaptionButton::None;
    bool hamburgerHovered_ = false;
    bool hamburgerPressed_ = false;
    OverlayKind overlay_ = OverlayKind::None;
    bool dropdownOpen_ = false;
    DropdownItem dropdownHovered_ = DropdownItem::None;
    DropdownItem dropdownPressed_ = DropdownItem::None;
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
        viewer->SetCaptionButtonHover(CaptionButtonFromHitTest(wParam));
        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE | TME_NONCLIENT, window, 0 };
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_NCMOUSELEAVE: viewer->SetCaptionButtonHover(CaptionButton::None); return 0;
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
        if (viewer->HasOverlay()) return 0;
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(window, &point);
        viewer->ZoomAt(point, std::pow(kZoomStep, static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA));
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        if (viewer->HasOverlay() || viewer->DropdownOpen()) return 0;
        const FrameMetrics frame = GetFrameMetrics(window);
        if (!PtInRect(&frame.hamburger, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) })) viewer->ToggleFullscreen();
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
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
            if (!viewer->OverlayContains(point)) viewer->DismissOverlay();
            return 0;
        }
        const FrameMetrics frame = GetFrameMetrics(window);
        if (PtInRect(&frame.hamburger, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) })) {
            viewer->SetHamburgerPressed(true);
            SetCapture(window);
        } else {
            viewer->BeginPan({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (viewer->DropdownOpen()) {
            viewer->SetDropdownHover(viewer->DropdownItemAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }));
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        if (viewer->HasOverlay()) {
            viewer->SetHamburgerHover(false);
            return 0;
        }
        const FrameMetrics frame = GetFrameMetrics(window);
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        viewer->SetHamburgerHover(!viewer->IsFullscreen() && PtInRect(&frame.hamburger, point));
        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
        TrackMouseEvent(&track);
        if (!viewer->HamburgerPressed()) viewer->PanTo(point);
        return 0;
    }
    case WM_MOUSELEAVE: viewer->SetHamburgerHover(false); viewer->SetDropdownHover(DropdownItem::None); return 0;
    case WM_LBUTTONUP: {
        if (viewer->PressedDropdownItem() != DropdownItem::None) {
            const DropdownItem pressed = viewer->PressedDropdownItem();
            const DropdownItem released = viewer->DropdownItemAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            viewer->ClearDropdownPressed();
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
        if (pressed == CaptionButton::None) { viewer->EndPan(); return 0; }
        const FrameMetrics frame = GetFrameMetrics(window);
        const CaptionButton released = CaptionButtonAt(frame, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        viewer->ClearCaptionButtonPressed();
        if (GetCapture() == window) ReleaseCapture();
        if (pressed == released) SendMessageW(window, WM_SYSCOMMAND, SystemCommandForCaptionButton(window, released), 0);
        return 0;
    }
    case WM_CAPTURECHANGED:
        viewer->EndPan(); viewer->ClearCaptionButtonPressed(); viewer->SetHamburgerPressed(false); viewer->ClearDropdownPressed(); return 0;
    case WM_SETTINGCHANGE: ApplyTitleBarTheme(window); return 0;
    case kBuildNavigationMessage: viewer->BuildNavigation(); return 0;
    case WM_KEYDOWN:
        if (viewer->DropdownOpen()) {
            if (wParam == VK_ESCAPE) viewer->DismissDropdown();
            return 0;
        }
        if (viewer->HasOverlay()) {
            if (wParam == VK_ESCAPE) viewer->DismissOverlay();
            return 0;
        }
        if (wParam == VK_ESCAPE) { if (viewer->IsFullscreen()) viewer->ToggleFullscreen(); else DestroyWindow(window); return 0; }
        if (wParam == VK_F11) { viewer->ToggleFullscreen(); return 0; }
        if (wParam == VK_RIGHT) { viewer->Navigate(1); return 0; }
        if (wParam == VK_LEFT) { viewer->Navigate(-1); return 0; }
        if (wParam == VK_OEM_PLUS || wParam == VK_ADD || wParam == L'=') { viewer->ZoomCentered(kZoomStep); return 0; }
        if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) { viewer->ZoomCentered(1.0f / kZoomStep); return 0; }
        if (wParam == L'0' || wParam == VK_NUMPAD0) { viewer->FitToWindow(); return 0; }
        break;
    case WM_DESTROY: viewer->SaveWindowPlacement(); PostQuitMessage(0); return 0;
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
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.style = CS_DBLCLKS;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    windowClass.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(101));
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassExW(&windowClass);

    const SIZE client = viewer.SuggestedClientSize();
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
        const LONG width = bounds.right - bounds.left;
        const LONG height = bounds.bottom - bounds.top;
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
