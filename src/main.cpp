#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include <shobjidl_core.h>
#include <shlwapi.h>
#include <propkey.h>
#include <propsys.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <gdiplus.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>
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
constexpr UINT_PTR kCopyFeedbackTimer = 1;
constexpr int kContextMenuRowCount = 8;
constexpr int kContextMenuSeparatorCount = 3;
constexpr int kContextMenuPaddingDip = 8;
constexpr float kMaximumZoom = 16.0f;
constexpr float kZoomStep = 1.20f;
constexpr wchar_t kSettingsKey[] = L"Software\\Viewtrious";
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
const D2D1_COLOR_F kViewerBackground = D2D1::ColorF(26.0f / 255.0f, 26.0f / 255.0f, 26.0f / 255.0f);

enum class OverlayKind { None, KeyboardShortcuts, About, Settings };
enum class DropdownItem { None, OpenFile, Settings, KeyboardShortcuts, About, Close };
enum class ContextAction { None, RotateLeft, RotateRight, OpenWith, Copy, Print, SetBackground, SetLockScreen, Delete };

struct ShortcutEntry { const wchar_t* shortcut; const wchar_t* description; };
struct OpenWithHandler { std::wstring name; ComPtr<IAssocHandler> handler; };
constexpr ShortcutEntry kShortcutEntries[] = {
    { L"Ctrl+O", L"Open file" }, { L"Left Arrow", L"Previous image" }, { L"Right Arrow", L"Next image" }, { L"Mouse Wheel", L"Zoom" },
    { L"+ / =", L"Zoom in" }, { L"-", L"Zoom out" }, { L"0", L"Reset zoom and center" },
    { L"Left mouse drag", L"Pan" }, { L"Double-click image", L"Toggle fullscreen" }, { L"F11", L"Toggle fullscreen" },
    { L"Ctrl+C", L"Copy image" }, { L"Ctrl+P", L"Print" }, { L"Delete", L"Move image to Recycle Bin" },
    { L"Esc", L"Exit fullscreen, or close Viewtrious" },
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
        extension == L".heic" || extension == L".heif" || extension == L".avif";
}

std::wstring LowercaseExtension(const std::wstring& path) {
    std::wstring extension = fs::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return extension;
}

bool IsJpegPath(const std::wstring& path) {
    const std::wstring extension = LowercaseExtension(path);
    return extension == L".jpg" || extension == L".jpeg";
}

bool IsPngPath(const std::wstring& path) {
    return LowercaseExtension(path) == L".png";
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

void WriteSetting(const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
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
    const int filenameLeadIn = MulDiv(14, dpi, 96);
    const int resolutionWidth = MulDiv(92, dpi, 96);
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
        DWORD includeHidden = 1;
        ReadSetting(L"IncludeHiddenImages", includeHidden);
        includeHiddenImages_ = includeHidden != 0;
        if (!path.empty()) return LoadImage(path);
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
            currentPath_ = path;
            resolutionText_.clear();
            fileSizeText_ = FormatFileSize(path);
            filenameText_ = fs::path(path).filename().wstring();
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
        if (dropdownOpen_) { DismissOverlay(); DismissContextMenu(); }
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
        const int top = bounds.top + MulDiv(4, GetDpiForWindow(window_), 96);
        const int index = (point.y - top) / rowHeight;
        if (point.y < top || index < 0 || index > 4) return DropdownItem::None;
        return static_cast<DropdownItem>(index + 1);
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
        if (item == DropdownItem::OpenFile) OpenFile();
        else if (item == DropdownItem::Settings) ShowOverlay(OverlayKind::Settings);
        else if (item == DropdownItem::KeyboardShortcuts) ShowOverlay(OverlayKind::KeyboardShortcuts);
        else if (item == DropdownItem::About) ShowOverlay(OverlayKind::About);
        else if (item == DropdownItem::Close) SendMessageW(window_, WM_SYSCOMMAND, SC_CLOSE, 0);
    }
    void OpenFile() {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
        static const COMDLG_FILTERSPEC filters[] = {
            { L"Image files", L"*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.ico;*.webp;*.heic;*.heif;*.avif" },
            { L"All files", L"*.*" },
        };
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        dialog->SetFileTypeIndex(1);
        dialog->SetTitle(L"Open Image");
        if (FAILED(dialog->Show(window_))) return;
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            LoadImage(path);
            CoTaskMemFree(path);
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
    bool HasImage() const { return source_ != nullptr; }
    bool ContextMenuOpen() const { return contextMenuOpen_; }
    void OpenContextMenu(POINT point) {
        if (!HasImage()) return;
        DismissDropdown();
        DismissOverlay();
        contextMenuAnchor_ = point;
        contextMenuOpen_ = true;
        contextHovered_ = ContextAction::None;
        contextPressed_ = ContextAction::None;
        InvalidateRect(window_, nullptr, FALSE);
    }
    void DismissContextMenu() {
        if (!contextMenuOpen_) return;
        contextMenuOpen_ = false;
        openWithSubmenuOpen_ = false;
        contextHovered_ = ContextAction::None;
        contextPressed_ = ContextAction::None;
        InvalidateRect(window_, nullptr, FALSE);
    }
    ContextAction ContextActionAt(POINT point) const {
        if (!contextMenuOpen_) return ContextAction::None;
        const RECT bounds = GetContextMenuBounds();
        if (!PtInRect(&bounds, point)) return ContextAction::None;
        const int rowHeight = MulDiv(38, GetDpiForWindow(window_), 96);
        const int separatorGap = MulDiv(9, GetDpiForWindow(window_), 96);
        int top = bounds.top + MulDiv(kContextMenuPaddingDip, GetDpiForWindow(window_), 96);
        const auto hit = [&](ContextAction action) {
            const bool contains = point.y >= top && point.y < top + rowHeight;
            top += rowHeight;
            return contains ? action : ContextAction::None;
        };
        ContextAction action = hit(ContextAction::RotateLeft); if (action != ContextAction::None) return action;
        action = hit(ContextAction::RotateRight); if (action != ContextAction::None) return action;
        top += separatorGap;
        action = hit(ContextAction::OpenWith); if (action != ContextAction::None) return action;
        action = hit(ContextAction::Copy); if (action != ContextAction::None) return action;
        action = hit(ContextAction::Print); if (action != ContextAction::None) return action;
        top += separatorGap;
        action = hit(ContextAction::SetBackground); if (action != ContextAction::None) return action;
        action = hit(ContextAction::SetLockScreen); if (action != ContextAction::None) return action;
        top += separatorGap;
        return hit(ContextAction::Delete);
    }
    bool ContextActionEnabled(ContextAction action) const {
        if (action == ContextAction::OpenWith || action == ContextAction::Copy || action == ContextAction::Print || action == ContextAction::Delete)
            return true;
        return (action == ContextAction::RotateLeft || action == ContextAction::RotateRight) &&
            (IsJpegPath(currentPath_) || IsPngPath(currentPath_));
    }
    void SetContextHover(ContextAction action) {
        if (!ContextActionEnabled(action)) action = ContextAction::None;
        if (contextHovered_ == action) return;
        contextHovered_ = action;
        InvalidateRect(window_, nullptr, FALSE);
    }
    void SetContextPressed(ContextAction action) {
        contextPressed_ = action;
        InvalidateRect(window_, nullptr, FALSE);
    }
    ContextAction PressedContextAction() const { return contextPressed_; }
    void ClearContextPressed() { SetContextPressed(ContextAction::None); }
    void InvokeContextAction(ContextAction action) {
        if (action == ContextAction::OpenWith) { ToggleOpenWithSubmenu(); return; }
        DismissContextMenu();
        if (action == ContextAction::Copy) CopyImage();
        else if (action == ContextAction::Print) PrintImage();
        else if (action == ContextAction::RotateLeft) RotateImage(false);
        else if (action == ContextAction::RotateRight) RotateImage(true);
        else if (action == ContextAction::Delete) DeleteImage();
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
        const LONG top = parent.top + MulDiv(kContextMenuPaddingDip, dpi, 96) + row * 2 + gap;
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
    void ShowOverlay(OverlayKind overlay) {
        DismissDropdown();
        DismissContextMenu();
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
    bool SettingsCheckboxContains(POINT point) const {
        if (overlay_ != OverlayKind::Settings) return false;
        const RECT bounds = GetOverlayBounds();
        const UINT dpi = GetDpiForWindow(window_);
        const int padding = MulDiv(24, dpi, 96);
        const int rowTop = bounds.top + padding + MulDiv(42, dpi, 96);
        const RECT row{ bounds.left + padding, rowTop, bounds.right - padding, rowTop + MulDiv(32, dpi, 96) };
        return PtInRect(&row, point);
    }
    void ToggleIncludeHiddenImages() {
        includeHiddenImages_ = !includeHiddenImages_;
        WriteSetting(L"IncludeHiddenImages", includeHiddenImages_ ? 1 : 0);
        navigationFiles_.clear();
        navigationBuilt_ = false;
        navigationBuildQueued_ = false;
        InvalidateRect(window_, nullptr, FALSE);
    }
    bool EmptyOpenFileButtonContains(POINT point) const {
        if (HasImage() || HasOverlay() || dropdownOpen_ || contextMenuOpen_) return false;
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
        EnsureRenderTarget();
        if (renderTarget_) {
            renderTarget_->BeginDraw();
            renderTarget_->Clear(kViewerBackground);            if (source_) {
                EnsureBitmap();
                if (bitmap_) DrawImage();
            } else DrawEmptyState();
            DrawTitleBar();
            DrawDropdown();
            DrawContextMenu();
            DrawOpenWithSubmenu();
            DrawOverlay();
            DrawCopyFeedback();
            const HRESULT hr = renderTarget_->EndDraw();
            if (SUCCEEDED(hr) && bitmap_) MarkFirstPresentation();
            if (hr == D2DERR_RECREATE_TARGET) DiscardRenderResources();
        }
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
        if (!source_) return { 800, 600 };
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
            const DWORD attributes = GetFileAttributesW(iterator->path().c_str());
            const bool hidden = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
            if (iterator->is_regular_file(typeError) && !typeError && IsSupportedExtension(iterator->path()) &&
                (includeHiddenImages_ || !hidden)) {
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
        const LONG top = std::clamp<LONG>(parent.top + MulDiv(kContextMenuPaddingDip, dpi, 96) + row * 2 + gap, margin, std::max<LONG>(margin, client.bottom - height - margin));
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
        const LONG height = padding * 2 + rowHeight * kContextMenuRowCount + separatorGap * kContextMenuSeparatorCount;
        const LONG left = std::clamp<LONG>(contextMenuAnchor_.x, margin, std::max<LONG>(margin, client.right - width - margin));
        const LONG top = std::clamp<LONG>(contextMenuAnchor_.y, margin, std::max<LONG>(margin, client.bottom - height - margin));
        return { left, top, left + width, top + height };
    }

    void ShowActionError(const wchar_t* message) const { MessageBoxW(window_, message, kWindowTitle, MB_OK | MB_ICONWARNING); }

    void OpenWith() {
        if (currentPath_.empty()) return;
        OPENASINFO info{};
        info.pcszFile = currentPath_.c_str();
        info.oaifInFlags = OAIF_EXEC;
        if (FAILED(SHOpenWithDialog(window_, &info))) ShowActionError(L"Windows could not open the Open With chooser for this image.");
    }

    void StartCopyFeedback() {
        copyFeedbackStart_ = GetTickCount64();
        copyFeedbackActive_ = true;
        SetTimer(window_, kCopyFeedbackTimer, 16, nullptr);
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
        if (!OpenClipboard(window_)) { GlobalFree(memory); ShowActionError(L"The clipboard is currently unavailable."); return; }
        EmptyClipboard();
        if (!SetClipboardData(CF_DIBV5, memory)) { CloseClipboard(); GlobalFree(memory); ShowActionError(L"Viewtrious could not publish the image to the clipboard."); return; }
        CloseClipboard();
        StartCopyFeedback();
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
        PROPVARIANT value{};
        PropVariantInit(&value);
        UINT orientation = 1;
        if (SUCCEEDED(frame->GetMetadataQueryReader(&metadata)) &&
            SUCCEEDED(metadata->GetMetadataByName(L"/app1/ifd/{ushort=274}", &value))) {
            if (value.vt == VT_UI2) orientation = value.uiVal;
            else if (value.vt == VT_UI4) orientation = value.ulVal;
        }
        PropVariantClear(&value);
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

    HRESULT RotatePngWithWic(bool clockwise, const wchar_t*& failedStage, DWORD& failedWin32Error) {
        failedStage = nullptr;
        failedWin32Error = ERROR_SUCCESS;
        const auto stage = [&](const wchar_t* name, HRESULT result) {
            const DWORD win32Error = FAILED(result) ? GetLastError() : ERROR_SUCCESS;
            LogRotationStage(name, result, win32Error);
            if (FAILED(result)) { failedStage = name; failedWin32Error = win32Error; }
            return SUCCEEDED(result);
        };
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(currentPath_.c_str(), GetFileExInfoStandard, &attributes)) {
            const HRESULT openError = HRESULT_FROM_WIN32(GetLastError());
            stage(L"source PNG attributes: GetFileAttributesExW", openError);
            return openError;
        }

        ComPtr<IWICBitmapDecoder> decoder;
        HRESULT hr = wicFactory_->CreateDecoderFromFilename(currentPath_.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder);
        if (!stage(L"source PNG open/decode: IWICImagingFactory::CreateDecoderFromFilename", hr)) return hr;
        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, &frame);
        if (!stage(L"source PNG frame read: IWICBitmapDecoder::GetFrame", hr)) return hr;
        ComPtr<IWICFormatConverter> converter;
        hr = wicFactory_->CreateFormatConverter(&converter);
        if (!stage(L"source PNG format conversion: CreateFormatConverter", hr)) return hr;
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (!stage(L"source PNG format conversion: IWICFormatConverter::Initialize", hr)) return hr;
        ComPtr<IWICBitmapFlipRotator> rotator;
        hr = wicFactory_->CreateBitmapFlipRotator(&rotator);
        if (!stage(L"WIC transform/rotation: CreateBitmapFlipRotator", hr)) return hr;
        hr = rotator->Initialize(converter.Get(), clockwise ? WICBitmapTransformRotate90 : WICBitmapTransformRotate270);
        if (!stage(L"WIC transform/rotation: IWICBitmapFlipRotator::Initialize", hr)) return hr;

        struct TemporarySiblingFile {
            std::wstring path;
            ~TemporarySiblingFile() { if (!path.empty()) DeleteFileW(path.c_str()); }
            void Release() { path.clear(); }
        } temporary;
        wchar_t tempPath[MAX_PATH]{};
        const std::wstring directory = fs::path(currentPath_).parent_path().wstring();
        if (!GetTempFileNameW(directory.c_str(), L"FV", 0, tempPath)) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            stage(L"temp sibling path creation: GetTempFileNameW", hr);
            return hr;
        }
        temporary.path = tempPath;
        LogRotationStage(L"temp sibling path creation: GetTempFileNameW", S_OK);

        ComPtr<IWICStream> stream;
        hr = wicFactory_->CreateStream(&stream);
        if (!stage(L"temp output stream creation: CreateStream", hr)) return hr;
        hr = stream->InitializeFromFilename(temporary.path.c_str(), GENERIC_WRITE);
        if (!stage(L"temp output stream creation: IWICStream::InitializeFromFilename", hr)) return hr;
        ComPtr<IWICBitmapEncoder> encoder;
        hr = wicFactory_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (!stage(L"PNG encoder creation: CreateEncoder", hr)) return hr;
        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (!stage(L"PNG encoder initialization: IWICBitmapEncoder::Initialize", hr)) return hr;
        ComPtr<IWICBitmapFrameEncode> encodedFrame;
        IPropertyBag2* options = nullptr;
        hr = encoder->CreateNewFrame(&encodedFrame, &options);
        if (options) options->Release();
        if (!stage(L"PNG frame creation: IWICBitmapEncoder::CreateNewFrame", hr)) return hr;
        hr = encodedFrame->Initialize(nullptr);
        if (!stage(L"frame initialization: IWICBitmapFrameEncode::Initialize", hr)) return hr;
        UINT width = 0, height = 0;
        hr = rotator->GetSize(&width, &height);
        if (!stage(L"WIC transform output size: IWICBitmapSource::GetSize", hr)) return hr;
        hr = encodedFrame->SetSize(width, height);
        if (!stage(L"PNG frame size: IWICBitmapFrameEncode::SetSize", hr)) return hr;
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
        hr = encodedFrame->SetPixelFormat(&pixelFormat);
        if (!stage(L"PNG pixel format: IWICBitmapFrameEncode::SetPixelFormat", hr)) return hr;
        hr = encodedFrame->WriteSource(rotator.Get(), nullptr);
        if (!stage(L"pixel write: IWICBitmapFrameEncode::WriteSource", hr)) return hr;
        hr = encodedFrame->Commit();
        if (!stage(L"frame commit: IWICBitmapFrameEncode::Commit", hr)) return hr;
        hr = encoder->Commit();
        if (!stage(L"encoder commit: IWICBitmapEncoder::Commit", hr)) return hr;
        encodedFrame.Reset();
        encoder.Reset();
        stream.Reset();
        rotator.Reset();
        converter.Reset();
        frame.Reset();
        decoder.Reset();
        LogRotationStage(L"stream/file close: released WIC encoder, frame, stream, and source decoder graph", S_OK);

        ComPtr<IWICBitmapDecoder> validationDecoder;
        hr = wicFactory_->CreateDecoderFromFilename(temporary.path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &validationDecoder);
        if (!stage(L"validation of temporary PNG: CreateDecoderFromFilename", hr)) return hr;
        ComPtr<IWICBitmapFrameDecode> validationFrame;
        hr = validationDecoder->GetFrame(0, &validationFrame);
        if (!stage(L"validation of temporary PNG: IWICBitmapDecoder::GetFrame", hr)) return hr;
        validationFrame.Reset();
        validationDecoder.Reset();
        LogRotationStage(L"validation of temporary PNG", S_OK);

        const auto replacementProbe = [&](const std::wstring& path, const wchar_t* name) {
            HANDLE probe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (probe == INVALID_HANDLE_VALUE) {
                const HRESULT probeError = HRESULT_FROM_WIN32(GetLastError());
                stage(name, probeError);
                return probeError;
            }
            CloseHandle(probe);
            LogRotationStage(name, S_OK);
            return S_OK;
        };
        hr = replacementProbe(currentPath_, L"replacement boundary probe: original source path");
        if (FAILED(hr)) return hr;
        hr = replacementProbe(temporary.path, L"replacement boundary probe: temporary replacement path");
        if (FAILED(hr)) return hr;

        if (!ReplaceFileW(currentPath_.c_str(), temporary.path.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            stage(L"replacement of original: ReplaceFileW", hr);
            return hr;
        }
        temporary.Release();
        LogRotationStage(L"replacement of original: ReplaceFileW", S_OK);
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, currentPath_.c_str(), nullptr);
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
        if (!GetTempFileNameW(directory.c_str(), L"FV", 0, tempPath)) {
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
        const auto missing = std::find_if(sourcePropertyIds.begin(), sourcePropertyIds.end(), [&](PROPID id) {
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

    void RotateImage(bool clockwise) {
        if (currentPath_.empty()) return;
        rotationDiagnosticDetail_.clear();
        const wchar_t* failedStage = nullptr;
        DWORD failedWin32Error = ERROR_SUCCESS;
        const HRESULT hr = IsJpegPath(currentPath_) ? RotateJpeg(clockwise, failedStage, failedWin32Error) :
            IsPngPath(currentPath_) ? RotatePng(clockwise, failedStage, failedWin32Error) : E_NOTIMPL;
        if (FAILED(hr)) {
            if (IsJpegPath(currentPath_) || IsPngPath(currentPath_))
                ShowRotationFailure(failedStage ? failedStage : L"unknown rotation stage", hr, failedWin32Error);
            else ShowActionError(L"Viewtrious could not safely rotate this image. The original file was not replaced.");
            return;
        }
        const HRESULT reload = ReloadCurrentImage();
        if (IsJpegPath(currentPath_) || IsPngPath(currentPath_)) LogRotationStage(L"reload: DecodeImage", reload, FAILED(reload) ? GetLastError() : ERROR_SUCCESS);
        if (FAILED(reload) && (IsJpegPath(currentPath_) || IsPngPath(currentPath_))) ShowRotationFailure(L"reload: DecodeImage", reload, GetLastError());
    }

    void ClearDeletedImage() {
        source_.Reset(); bitmap_.Reset(); imageWidth_ = imageHeight_ = 0;
        currentPath_.clear(); resolutionText_.clear(); fileSizeText_.clear(); filenameText_.clear();
        navigationFiles_.clear(); navigationBuilt_ = false; navigationBuildQueued_ = false;
        fitToWindow_ = true; zoom_ = 1.0f; pan_ = D2D1::Point2F();
        error_ = L"Drop an image here, or launch Viewtrious with an image path.";
        InvalidateRect(window_, nullptr, FALSE);
    }

    void ShowImageAfterDelete() {
        BuildNavigation();
        const fs::path deleted(currentPath_);
        auto current = std::find_if(navigationFiles_.begin(), navigationFiles_.end(),
            [&deleted](const fs::path& path) { return PathsEqual(path, deleted); });
        if (current == navigationFiles_.end()) { ClearDeletedImage(); return; }
        const size_t index = static_cast<size_t>(std::distance(navigationFiles_.begin(), current));
        navigationFiles_.erase(current);
        for (size_t offset = 0; offset < navigationFiles_.size(); ++offset) {
            const size_t candidate = (index + offset) % navigationFiles_.size();
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
        const LONG height = MulDiv(198, dpi, 96);
        const LONG left = std::clamp<LONG>(frame.hamburger.left + margin, margin,
            std::max<LONG>(margin, client.right - width - margin));
        const LONG top = frame.hamburger.bottom + margin;
        return { left, top, left + width, std::min<LONG>(client.bottom - margin, top + height) };
    }

    HRESULT ReloadCurrentImage() {
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
        const int desiredWidth = MulDiv(overlay_ == OverlayKind::KeyboardShortcuts ? 460 :
            overlay_ == OverlayKind::Settings ? 560 : 760, dpi, 96);
        const int desiredHeight = overlay_ == OverlayKind::KeyboardShortcuts
            ? panelPadding + titleHeight + titleGap + static_cast<int>(kShortcutEntryCount) * rowHeight + panelPadding
            : overlay_ == OverlayKind::Settings ? MulDiv(170, dpi, 96) : MulDiv(350, dpi, 96);
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
        bool centerAlign = false) {
        ComPtr<IDWriteTextFormat> format;
        const float dpiScale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        if (FAILED(dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size * dpiScale, L"", &format))) return;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (verticallyCenter) format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (rightAlign) format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        else if (centerAlign) format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
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
        if (FAILED(DecodeImage((fs::path(modulePath).parent_path() / L"ViewtriousLogo.png").wstring(), source, width, height))) return false;
        return SUCCEEDED(renderTarget_->CreateBitmapFromWicBitmap(source.Get(), nullptr, &aboutLogo_));
    }

    RECT GetEmptyOpenFileButtonBounds() const {
        RECT client{};
        GetClientRect(window_, &client);
        const UINT dpi = GetDpiForWindow(window_);
        const int width = MulDiv(132, dpi, 96);
        const int height = MulDiv(38, dpi, 96);
        const int groupTop = (fullscreen_ ? 0 : GetFrameMetrics(window_).titleBarHeight) +
            std::max(0L, (client.bottom - (fullscreen_ ? 0 : GetFrameMetrics(window_).titleBarHeight) - MulDiv(300, dpi, 96)) / 2);
        const int top = groupTop + MulDiv(245, dpi, 96);
        const int left = (client.right - width) / 2;
        return { left, top, left + width, top + height };
    }

    void DrawEmptyState() {
        if (HasOverlay()) return;
        const bool dark = UseDarkAppMode();
        ComPtr<ID2D1SolidColorBrush> primary, secondary, button, buttonText;
        if (FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(D2D1::ColorF::White) : D2D1::ColorF(30.f/255,30.f/255,30.f/255), &primary)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(dark ? D2D1::ColorF(205.f/255,208.f/255,214.f/255) : D2D1::ColorF(78.f/255,78.f/255,78.f/255), &secondary)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f/255,120.f/255,212.f/255), &button)) ||
            FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &buttonText))) return;
        const UINT dpi = GetDpiForWindow(window_);
        const float scale = static_cast<float>(dpi) / 96.0f;
        const D2D1_SIZE_F target = renderTarget_->GetSize();
        const float top = fullscreen_ ? 0.0f : static_cast<float>(GetFrameMetrics(window_).titleBarHeight);
        const float groupTop = top + std::max(0.0f, (target.height - top - 300.0f * scale) / 2.0f);
        if (EnsureAboutLogo()) {
            const D2D1_SIZE_F logo = aboutLogo_->GetSize();
            const float width = std::min(440.0f * scale, target.width - 48.0f * scale);
            const float height = width * logo.height / logo.width;
            const float left = (target.width - width) / 2.0f;
            renderTarget_->DrawBitmap(aboutLogo_.Get(), D2D1::RectF(left, groupTop, left + width, groupTop + height), 1.0f,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        DrawOverlayText(error_.empty() ? L"Drop an image here or open a file" : error_.c_str(), 24.0f * scale,
            groupTop + 190.0f * scale, target.width - 48.0f * scale, 22.0f * scale, 13.0f,
            DWRITE_FONT_WEIGHT_NORMAL, secondary.Get(), true, false, true);
        const RECT buttonBounds = GetEmptyOpenFileButtonBounds();
        const D2D1_RECT_F buttonRect = D2D1::RectF(static_cast<float>(buttonBounds.left), static_cast<float>(buttonBounds.top),
            static_cast<float>(buttonBounds.right), static_cast<float>(buttonBounds.bottom));
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(buttonRect, 5.0f * scale, 5.0f * scale), button.Get());
        DrawOverlayText(L"Open File", buttonRect.left, buttonRect.top, buttonRect.right - buttonRect.left,
            buttonRect.bottom - buttonRect.top, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, buttonText.Get(), true, false, true);
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
        } else if (overlay_ == OverlayKind::Settings) {
            DrawOverlayText(L"Settings", left, static_cast<float>(bounds.top) + panelPadding,
                contentWidth, 24.0f * dpiScale, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get());
            const float rowTop = static_cast<float>(bounds.top) + panelPadding + 42.0f * dpiScale;
            const float boxSize = 18.0f * dpiScale;
            const D2D1_RECT_F checkbox = D2D1::RectF(left, rowTop + 5.0f * dpiScale, left + boxSize, rowTop + 5.0f * dpiScale + boxSize);
            renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(checkbox, 3.0f * dpiScale, 3.0f * dpiScale), borderBrush.Get(), 1.0f);
            if (includeHiddenImages_) {
                ComPtr<ID2D1SolidColorBrush> accent;
                if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.f/255,120.f/255,212.f/255), &accent))) {
                    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(checkbox, 3.0f * dpiScale, 3.0f * dpiScale), accent.Get());
                    DrawOverlayText(L"✓", checkbox.left, checkbox.top - 1.0f * dpiScale, boxSize, boxSize, 14.0f,
                        DWRITE_FONT_WEIGHT_SEMI_BOLD, primaryBrush.Get(), true, false, true);
                }
            }
            DrawOverlayText(L"Include hidden images in folder navigation", left + boxSize + 12.0f * dpiScale, rowTop,
                contentWidth - boxSize - 12.0f * dpiScale, 28.0f * dpiScale, 13.0f, DWRITE_FONT_WEIGHT_NORMAL,
                primaryBrush.Get(), true);
        } else {
            float logoBottom = static_cast<float>(bounds.top) + panelPadding;
            if (EnsureAboutLogo()) {
                const D2D1_SIZE_F logoSource = aboutLogo_->GetSize();
                const float logoWidth = std::min(520.0f * dpiScale, contentWidth);
                const float logoHeight = logoWidth * logoSource.height / logoSource.width;
                const float logoLeft = static_cast<float>(bounds.left) + 40.0f * dpiScale;
                const float logoTop = logoBottom;
                renderTarget_->DrawBitmap(aboutLogo_.Get(), D2D1::RectF(logoLeft, logoTop, logoLeft + logoWidth, logoTop + logoHeight));
                logoBottom = logoTop + logoHeight;
            }
            const float logoLeft = static_cast<float>(bounds.left) + 40.0f * dpiScale;
            const float logoWidth = std::min(520.0f * dpiScale, contentWidth);
            const float textTop = logoBottom + 16.0f * dpiScale;
            DrawOverlayText(L"Version " VIEWTRIOUS_VERSION, logoLeft, textTop, logoWidth, 20.0f * dpiScale,
                12.5f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), false, true);
            DrawOverlayText(L"Extremely lightweight image viewer", logoLeft, textTop + 25.0f * dpiScale, logoWidth,
                18.0f * dpiScale, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, secondaryBrush.Get(), false, true);
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
        const auto drawItem = [&](DropdownItem item, int top, const wchar_t* label) {
            if (dropdownPressed_ == item) renderTarget_->FillRectangle(row(top), pressedBrush.Get());
            else if (dropdownHovered_ == item) renderTarget_->FillRectangle(row(top), hoverBrush.Get());
            DrawOverlayText(label, static_cast<float>(bounds.left + MulDiv(14, dpi, 96)), static_cast<float>(top),
                static_cast<float>(bounds.right - bounds.left - MulDiv(28, dpi, 96)), static_cast<float>(rowHeight),
                13.0f, DWRITE_FONT_WEIGHT_NORMAL, textBrush.Get(), true);
        };
        drawItem(DropdownItem::OpenFile, firstTop, L"Open File...");
        drawItem(DropdownItem::Settings, firstTop + rowHeight, L"Settings");
        drawItem(DropdownItem::KeyboardShortcuts, firstTop + rowHeight * 2, L"Keyboard Shortcuts");
        drawItem(DropdownItem::About, firstTop + rowHeight * 3, L"About");
        drawItem(DropdownItem::Close, firstTop + rowHeight * 4, L"Close");
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
        const UINT dpi = GetDpiForWindow(window_); const int rowHeight = MulDiv(38, dpi, 96); const int gap = MulDiv(9, dpi, 96);
        int top = bounds.top + MulDiv(kContextMenuPaddingDip, dpi, 96);
        const auto drawItem = [&](ContextAction action, const wchar_t* label) {
            const bool enabled = ContextActionEnabled(action);
            const D2D1_RECT_F row = D2D1::RectF(static_cast<float>(bounds.left + 1), static_cast<float>(top), static_cast<float>(bounds.right - 1), static_cast<float>(top + rowHeight));
            if (enabled && contextPressed_ == action) renderTarget_->FillRectangle(row, pressedBrush.Get());
            else if (enabled && contextHovered_ == action) renderTarget_->FillRectangle(row, hoverBrush.Get());
            DrawOverlayText(label, static_cast<float>(bounds.left + MulDiv(14, dpi, 96)), static_cast<float>(top), static_cast<float>(bounds.right - bounds.left - MulDiv(28, dpi, 96)),
                static_cast<float>(rowHeight), 13.0f, DWRITE_FONT_WEIGHT_NORMAL, enabled ? textBrush.Get() : disabledBrush.Get(), true);
            top += rowHeight;
        };
        const auto separator = [&] {
            const float y = static_cast<float>(top + gap / 2);
            renderTarget_->DrawLine(D2D1::Point2F(static_cast<float>(bounds.left + MulDiv(12, dpi, 96)), y), D2D1::Point2F(static_cast<float>(bounds.right - MulDiv(12, dpi, 96)), y), borderBrush.Get());
            top += gap;
        };
        drawItem(ContextAction::RotateLeft, L"Rotate Left"); drawItem(ContextAction::RotateRight, L"Rotate Right"); separator();
        drawItem(ContextAction::OpenWith, L"Open With  >"); drawItem(ContextAction::Copy, L"Copy"); drawItem(ContextAction::Print, L"Print"); separator();
        drawItem(ContextAction::SetBackground, L"Set as Background"); drawItem(ContextAction::SetLockScreen, L"Set as Lock Screen"); separator(); drawItem(ContextAction::Delete, L"Delete");
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

    void DrawCopyFeedback() {
        if (!copyFeedbackActive_) return;
        const ULONGLONG elapsed = GetTickCount64() - copyFeedbackStart_;
        if (elapsed >= 1000) return;
        const float opacity = 0.75f * (1.0f - static_cast<float>(elapsed) / 1000.0f);
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
        const D2D1_ROUNDED_RECT rear=D2D1::RoundedRect(D2D1::RectF(x,y,x+glyph-offset,y+glyph-offset),18.f*scale,18.f*scale), front=D2D1::RoundedRect(D2D1::RectF(x+offset,y+offset,x+glyph,y+glyph),18.f*scale,18.f*scale);
        renderTarget_->DrawRoundedRectangle(rear,outline.Get(),stroke+3.f*scale); renderTarget_->DrawRoundedRectangle(front,outline.Get(),stroke+3.f*scale);
        renderTarget_->DrawRoundedRectangle(rear,brush.Get(),stroke); renderTarget_->DrawRoundedRectangle(front,brush.Get(),stroke);
        const float textY=y+glyph+gap;
        for (const POINT offsetPoint : { POINT{ -1, 0 }, POINT{ 1, 0 }, POINT{ 0, -1 }, POINT{ 0, 1 } }) DrawOverlayText(L"Copied to Clipboard",offsetPoint.x*scale,textY+offsetPoint.y*scale,size.width,textHeight,28.f,DWRITE_FONT_WEIGHT_SEMI_BOLD,textHalo.Get(),true,false,true);
        for (const POINT offsetPoint : { POINT{ -1, 0 }, POINT{ 1, 0 }, POINT{ 0, -1 }, POINT{ 0, 1 }, POINT{ -1, -1 }, POINT{ 1, -1 }, POINT{ -1, 1 }, POINT{ 1, 1 } }) DrawOverlayText(L"Copied to Clipboard",offsetPoint.x*scale,textY+offsetPoint.y*scale,size.width,textHeight,28.f,DWRITE_FONT_WEIGHT_SEMI_BOLD,textOutline.Get(),true,false,true);
        DrawOverlayText(L"Copied to Clipboard",0,textY,size.width,textHeight,28.f,DWRITE_FONT_WEIGHT_SEMI_BOLD,brush.Get(),true,false,true);
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

    void DrawErrorText() {
        RECT client{};
        GetClientRect(window_, &client);
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(220.0f / 255.0f, 220.0f / 255.0f, 220.0f / 255.0f), &brush))) return;
        const float top = fullscreen_ ? 0.0f : static_cast<float>(GetFrameMetrics(window_).titleBarHeight);
        DrawOverlayText(error_.c_str(), 24.0f, top, static_cast<float>(std::max(1L, client.right - 48L)), static_cast<float>(client.bottom) - top,
            13.0f, DWRITE_FONT_WEIGHT_NORMAL, brush.Get(), true, false, true);
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
    std::wstring rotationDiagnosticDetail_;
    std::vector<fs::path> navigationFiles_;
    D2D1_POINT_2F pan_ = D2D1::Point2F();
    POINT lastDragPoint_{};
    float zoom_ = 1.0f;
    bool fitToWindow_ = true;
    bool dragging_ = false;
    bool presented_ = false;
    bool navigationBuilt_ = false;
    bool navigationBuildQueued_ = false;
    bool includeHiddenImages_ = true;
    bool fullscreen_ = false;
    CaptionButton hoveredCaptionButton_ = CaptionButton::None;
    CaptionButton pressedCaptionButton_ = CaptionButton::None;
    bool hamburgerHovered_ = false;
    bool hamburgerPressed_ = false;
    OverlayKind overlay_ = OverlayKind::None;
    bool dropdownOpen_ = false;
    DropdownItem dropdownHovered_ = DropdownItem::None;
    DropdownItem dropdownPressed_ = DropdownItem::None;
    bool contextMenuOpen_ = false;
    ContextAction contextHovered_ = ContextAction::None;
    ContextAction contextPressed_ = ContextAction::None;
    POINT contextMenuAnchor_{};
    bool openWithSubmenuOpen_ = false;
    int openWithHovered_ = -1;
    std::wstring openWithExtension_;
    std::vector<OpenWithHandler> openWithHandlers_;
    bool copyFeedbackActive_ = false;
    ULONGLONG copyFeedbackStart_ = 0;
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
        if (viewer->HasOverlay() || viewer->DropdownOpen() || viewer->ContextMenuOpen()) return 0;
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(window, &point);
        viewer->ZoomAt(point, std::pow(kZoomStep, static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA));
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        if (viewer->HasOverlay() || viewer->DropdownOpen() || viewer->ContextMenuOpen()) return 0;
        const FrameMetrics frame = GetFrameMetrics(window);
        if (viewer->HasImage() && !PtInRect(&frame.hamburger, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) })) viewer->ToggleFullscreen();
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
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
            if (viewer->SettingsCheckboxContains(point)) { viewer->ToggleIncludeHiddenImages(); return 0; }
            if (!viewer->OverlayContains(point)) viewer->DismissOverlay();
            return 0;
        }
        const FrameMetrics frame = GetFrameMetrics(window);
        if (PtInRect(&frame.hamburger, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) })) {
            viewer->SetHamburgerPressed(true);
            SetCapture(window);
        } else if (viewer->EmptyOpenFileButtonContains(point)) {
            viewer->OpenFile();
        } else {
            viewer->BeginPan({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
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
    case WM_MOUSELEAVE: viewer->SetHamburgerHover(false); viewer->SetDropdownHover(DropdownItem::None); viewer->SetContextHover(ContextAction::None); return 0;
    case WM_LBUTTONUP: {
        if (viewer->PressedContextAction() != ContextAction::None) {
            const ContextAction pressed = viewer->PressedContextAction();
            const ContextAction released = viewer->ContextActionAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            viewer->ClearContextPressed();
            if (GetCapture() == window) ReleaseCapture();
            if (pressed == released) viewer->InvokeContextAction(pressed);
            return 0;
        }
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
        viewer->EndPan(); viewer->ClearCaptionButtonPressed(); viewer->SetHamburgerPressed(false); viewer->ClearDropdownPressed(); viewer->ClearContextPressed(); return 0;
    case WM_RBUTTONUP: viewer->OpenContextMenu({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }); return 0;
    case WM_TIMER: if (wParam == kCopyFeedbackTimer) { viewer->UpdateCopyFeedback(); return 0; } break;
    case WM_SETTINGCHANGE: ApplyTitleBarTheme(window); return 0;
    case kBuildNavigationMessage: viewer->BuildNavigation(); return 0;
    case WM_KEYDOWN:
        if (viewer->OpenWithSubmenuOpen()) { if (wParam == VK_ESCAPE) viewer->DismissOpenWithSubmenu(); return 0; }
        if (viewer->ContextMenuOpen()) {
            if (wParam == VK_ESCAPE) viewer->DismissContextMenu();
            return 0;
        }
        if (viewer->DropdownOpen()) {
            if (wParam == VK_ESCAPE) viewer->DismissDropdown();
            return 0;
        }
        if (viewer->HasOverlay()) {
            if (wParam == VK_ESCAPE) viewer->DismissOverlay();
            return 0;
        }
        if (GetKeyState(VK_CONTROL) < 0 && wParam == L'O') { viewer->OpenFile(); return 0; }
        if (GetKeyState(VK_CONTROL) < 0 && wParam == L'C') { viewer->InvokeContextAction(ContextAction::Copy); return 0; }
        if (GetKeyState(VK_CONTROL) < 0 && wParam == L'P') { viewer->InvokeContextAction(ContextAction::Print); return 0; }
        if (wParam == VK_DELETE) { viewer->InvokeContextAction(ContextAction::Delete); return 0; }
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
