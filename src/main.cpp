#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <d2d1.h>
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

void ApplyTitleBarTheme(HWND window) {
    const BOOL dark = UseDarkAppMode() ? TRUE : FALSE;
    DwmSetWindowAttribute(window, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
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
            navigationFiles_.clear();
            navigationBuilt_ = false;
            error_ = L"Unable to open this image. It may be corrupt or use an unsupported codec.";
        }
        return hr;
    }

    void SetWindow(HWND window) { window_ = window; }
    void LoadWatermark() {
        HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(102), RT_RCDATA);
        if (!resource) return;
        const BYTE* data = static_cast<const BYTE*>(LockResource(LoadResource(nullptr, resource)));
        const DWORD size = SizeofResource(nullptr, resource);
        ComPtr<IWICStream> stream; ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICFormatConverter> converter;
        HRESULT hr = wicFactory_->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(const_cast<BYTE*>(data), size);
        if (SUCCEEDED(hr)) hr = wicFactory_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
        if (SUCCEEDED(hr)) hr = frame->GetSize(&watermarkWidth_, &watermarkHeight_);
        if (SUCCEEDED(hr)) hr = wicFactory_->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr) && watermarkWidth_ && watermarkHeight_) watermarkSource_ = converter;
        InvalidateRect(window_, nullptr, FALSE);
    }

    void Paint() {
        PAINTSTRUCT paint{};
        BeginPaint(window_, &paint);
        EnsureRenderTarget();
        if (renderTarget_) {
            renderTarget_->BeginDraw();
            renderTarget_->Clear(D2D1::ColorF(0.10f, 0.10f, 0.10f));
            if (watermarkSource_ && !watermarkBitmap_) renderTarget_->CreateBitmapFromWicBitmap(watermarkSource_.Get(), nullptr, &watermarkBitmap_);
            if (watermarkBitmap_) { const D2D1_SIZE_F target = renderTarget_->GetSize(); const float width = 420.0f; const float height = width * watermarkHeight_ / watermarkWidth_; renderTarget_->DrawBitmap(watermarkBitmap_.Get(), D2D1::RectF((target.width - width) / 2.0f, (target.height - height) / 2.0f, (target.width + width) / 2.0f, (target.height + height) / 2.0f), 0.5f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR); }
            if (source_) {
                EnsureBitmap();
                if (bitmap_) DrawImage();
            }
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

    void DrawImage() {
        const D2D1_SIZE_F target = renderTarget_->GetSize();
        const float scale = CurrentScale();
        const D2D1_POINT_2F topLeft = ImageTopLeft(scale, target);
        const D2D1_RECT_F destination = D2D1::RectF(topLeft.x, topLeft.y,
            topLeft.x + imageWidth_ * scale, topLeft.y + imageHeight_ * scale);
        renderTarget_->DrawBitmap(bitmap_.Get(), destination, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
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

    void DiscardRenderResources() { bitmap_.Reset(); watermarkBitmap_.Reset(); renderTarget_.Reset(); }

    const StartupTimer& timer_;
    HWND window_ = nullptr;
    ComPtr<IWICImagingFactory> wicFactory_;
    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<IWICBitmapSource> source_;
    ComPtr<ID2D1HwndRenderTarget> renderTarget_;
    ComPtr<ID2D1Bitmap> bitmap_;
    ComPtr<IWICBitmapSource> watermarkSource_;
    ComPtr<ID2D1Bitmap> watermarkBitmap_;
    UINT imageWidth_ = 0;
    UINT imageHeight_ = 0;
    UINT watermarkWidth_ = 0;
    UINT watermarkHeight_ = 0;
    std::wstring currentPath_;
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
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* viewer = reinterpret_cast<Viewer*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        viewer = static_cast<Viewer*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(viewer));
        viewer->SetWindow(window);
    }
    if (!viewer) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CREATE: PostMessageW(window, WM_APP + 2, 0, 0); return 0;
    case WM_APP + 2: viewer->LoadWatermark(); return 0;
    case WM_PAINT: viewer->Paint(); return 0;
    case WM_SIZE: viewer->Resize(); return 0;
    case WM_DROPFILES: viewer->DropFile(reinterpret_cast<HDROP>(wParam)); return 0;
    case WM_MOUSEWHEEL: {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(window, &point);
        viewer->ZoomAt(point, std::pow(kZoomStep, static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA));
        return 0;
    }
    case WM_LBUTTONDOWN: viewer->BeginPan({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }); return 0;
    case WM_MOUSEMOVE: viewer->PanTo({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }); return 0;
    case WM_LBUTTONUP: viewer->EndPan(); return 0;
    case WM_CAPTURECHANGED: viewer->EndPan(); return 0;
    case WM_SETTINGCHANGE: ApplyTitleBarTheme(window); return 0;
    case kBuildNavigationMessage: viewer->BuildNavigation(); return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { DestroyWindow(window); return 0; }
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
    HWND window = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, &viewer);
    if (!window) { CoUninitialize(); return 1; }

    SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(windowClass.hIcon));
    SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(windowClass.hIconSm));
    DragAcceptFiles(window, TRUE);
    ApplyTitleBarTheme(window);
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


