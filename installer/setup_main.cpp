#include <windows.h>
#include <windowsx.h>

#include <bcrypt.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"ViewtriousSetupWindow";
constexpr float kClientWidth = 760.0f;
constexpr float kClientHeight = 450.0f;
constexpr UINT kInstallCompleteMessage = WM_APP + 1;
constexpr UINT_PTR kAnimationTimer = 1;
constexpr int kSetupIconResource = 101;
constexpr int kLicenseResource = 201;
constexpr int kNoticeResource = 202;
constexpr int kMinizResource = 203;
constexpr int kLogoResource = 204;
constexpr DWORD kBackendExplorerLocked = 20;
constexpr DWORD kBackendApplicationLocked = 21;
constexpr DWORD kBackendIntegrationFailed = 22;
constexpr DWORD kPayloadFailure = 1001;
constexpr DWORD kLaunchFailure = 1002;

#pragma pack(push, 1)
struct PayloadTrailer {
    std::array<char, 16> magic;
    std::uint32_t formatVersion;
    std::uint64_t payloadLength;
    std::array<std::uint8_t, 32> sha256;
};
#pragma pack(pop)

static_assert(sizeof(PayloadTrailer) == 60);

enum class SetupState {
    idle,
    installing,
    succeeded,
    failed,
};

enum class HitTarget {
    none,
    primary,
    secondary,
    desktopShortcut,
    license,
    close,
    overlayClose,
};

struct App {
    HWND window{};
    UINT dpi{96};
    SetupState state{SetupState::idle};
    DWORD failureCode{};
    bool desktopShortcut{};
    bool licenseOpen{};
    float licenseScroll{};
    float licenseExtent{};
    HitTarget hover{HitTarget::none};
    HitTarget pressed{HitTarget::none};
    int focusIndex{};
    ULONGLONG installStarted{};
    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<IDWriteFactory> writeFactory;
    ComPtr<IWICImagingFactory> wicFactory;
    ComPtr<ID2D1HwndRenderTarget> renderTarget;
    ComPtr<ID2D1Bitmap> logo;
    ComPtr<IDWriteTextFormat> titleFormat;
    ComPtr<IDWriteTextFormat> taglineFormat;
    ComPtr<IDWriteTextFormat> bodyFormat;
    ComPtr<IDWriteTextFormat> smallFormat;
    ComPtr<IDWriteTextFormat> buttonFormat;
    ComPtr<IDWriteTextFormat> overlayTitleFormat;
    std::wstring legalText;
};

App g_app;

D2D1_COLOR_F Color(UINT32 rgb, float alpha = 1.0f) {
    return D2D1::ColorF(rgb, alpha);
}

float PixelToDip(LPARAM coordinate, UINT dpi) {
    return static_cast<float>(coordinate) * 96.0f / static_cast<float>(dpi);
}

D2D1_RECT_F CloseRect() { return D2D1::RectF(716.0f, 14.0f, 746.0f, 44.0f); }
D2D1_RECT_F PrimaryRect() { return D2D1::RectF(338.0f, 235.0f, 714.0f, 291.0f); }
D2D1_RECT_F SecondaryRect() { return D2D1::RectF(338.0f, 304.0f, 714.0f, 346.0f); }
D2D1_RECT_F CheckboxRect() { return D2D1::RectF(338.0f, 310.0f, 620.0f, 338.0f); }
D2D1_RECT_F LicenseRect() { return D2D1::RectF(338.0f, 393.0f, 485.0f, 421.0f); }
D2D1_RECT_F OverlayCloseRect() { return D2D1::RectF(683.0f, 52.0f, 713.0f, 82.0f); }

bool Contains(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

HRESULT CreateTextFormat(
    IDWriteFactory* factory,
    float size,
    DWRITE_FONT_WEIGHT weight,
    DWRITE_TEXT_ALIGNMENT alignment,
    IDWriteTextFormat** output) {
    const HRESULT result = factory->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        weight,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        size,
        L"en-us",
        output);
    if (SUCCEEDED(result)) {
        (*output)->SetTextAlignment(alignment);
        (*output)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    return result;
}

std::wstring Utf8Resource(HINSTANCE instance, int identifier) {
    const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(identifier), RT_RCDATA);
    if (!resource) return {};
    const HGLOBAL loaded = LoadResource(instance, resource);
    if (!loaded) return {};
    const auto* bytes = static_cast<const char*>(LockResource(loaded));
    const DWORD byteCount = SizeofResource(instance, resource);
    if (!bytes || byteCount == 0) return {};
    int offset = 0;
    if (byteCount >= 3 && static_cast<unsigned char>(bytes[0]) == 0xef &&
        static_cast<unsigned char>(bytes[1]) == 0xbb && static_cast<unsigned char>(bytes[2]) == 0xbf) {
        offset = 3;
    }
    const int sourceLength = static_cast<int>(byteCount) - offset;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes + offset, sourceLength, nullptr, 0);
    if (length <= 0) return {};
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes + offset, sourceLength, text.data(), length) != length) {
        return {};
    }
    return text;
}

std::wstring ModulePath() {
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < static_cast<DWORD>(buffer.size() - 1)) return std::wstring(buffer.data(), length);
        buffer.resize(buffer.size() * 2);
    }
}

bool ReadExactly(HANDLE file, void* destination, DWORD bytes) {
    auto* output = static_cast<std::uint8_t*>(destination);
    DWORD remaining = bytes;
    while (remaining > 0) {
        DWORD read{};
        if (!ReadFile(file, output, remaining, &read, nullptr) || read == 0) return false;
        output += read;
        remaining -= read;
    }
    return true;
}

bool WriteExactly(HANDLE file, const void* source, DWORD bytes) {
    const auto* input = static_cast<const std::uint8_t*>(source);
    DWORD remaining = bytes;
    while (remaining > 0) {
        DWORD written{};
        if (!WriteFile(file, input, remaining, &written, nullptr) || written == 0) return false;
        input += written;
        remaining -= written;
    }
    return true;
}

std::wstring CreateUniqueTempDirectory() {
    std::array<wchar_t, MAX_PATH + 1> tempPath{};
    const DWORD tempLength = GetTempPathW(static_cast<DWORD>(tempPath.size()), tempPath.data());
    if (tempLength == 0 || tempLength >= static_cast<DWORD>(tempPath.size())) return {};
    for (int attempt = 0; attempt < 4; ++attempt) {
        GUID identifier{};
        if (FAILED(CoCreateGuid(&identifier))) return {};
        std::array<wchar_t, 40> guidText{};
        if (StringFromGUID2(identifier, guidText.data(), static_cast<int>(guidText.size())) == 0) return {};
        std::wstring compact;
        for (const wchar_t character : guidText) {
            if (character != L'{' && character != L'}' && character != L'\0') compact.push_back(character);
        }
        std::wstring directory = tempPath.data();
        directory += L"viewtrious-setup-";
        directory += compact;
        if (CreateDirectoryW(directory.c_str(), nullptr)) return directory;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return {};
    }
    return {};
}

bool ExtractPayload(const std::wstring& destination) {
    const std::wstring sourcePath = ModulePath();
    if (sourcePath.empty()) return false;
    const HANDLE source = CreateFileW(sourcePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (source == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize{};
    bool success = GetFileSizeEx(source, &fileSize) != FALSE &&
        fileSize.QuadPart > static_cast<LONGLONG>(sizeof(PayloadTrailer));
    PayloadTrailer trailer{};
    if (success) {
        LARGE_INTEGER trailerPosition{};
        trailerPosition.QuadPart = fileSize.QuadPart - static_cast<LONGLONG>(sizeof(PayloadTrailer));
        success = SetFilePointerEx(source, trailerPosition, nullptr, FILE_BEGIN) != FALSE &&
            ReadExactly(source, &trailer, static_cast<DWORD>(sizeof(trailer)));
    }
    constexpr std::array<char, 16> expectedMagic{
        'V', 'T', 'R', 'S', 'E', 'T', 'U', 'P', 'P', 'A', 'Y', 'L', 'O', 'A', 'D', '\0'};
    if (success) {
        const std::uint64_t maximumPayload = static_cast<std::uint64_t>(fileSize.QuadPart - sizeof(PayloadTrailer));
        success = trailer.magic == expectedMagic && trailer.formatVersion == 1 &&
            trailer.payloadLength > 0 && trailer.payloadLength <= maximumPayload;
    }

    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    std::vector<std::uint8_t> hashObject;
    std::array<std::uint8_t, 32> actualHash{};
    if (success) {
        success = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    }
    DWORD objectLength{};
    DWORD returned{};
    if (success) {
        success = BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength),
            sizeof(objectLength),
            &returned,
            0) >= 0;
    }
    if (success) {
        hashObject.resize(objectLength);
        success = BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength, nullptr, 0, 0) >= 0;
    }

    const HANDLE output = success
        ? CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr)
        : INVALID_HANDLE_VALUE;
    success = success && output != INVALID_HANDLE_VALUE;
    if (success) {
        LARGE_INTEGER payloadPosition{};
        payloadPosition.QuadPart = fileSize.QuadPart - static_cast<LONGLONG>(sizeof(PayloadTrailer)) -
            static_cast<LONGLONG>(trailer.payloadLength);
        success = SetFilePointerEx(source, payloadPosition, nullptr, FILE_BEGIN) != FALSE;
    }
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t remaining = trailer.payloadLength;
    while (success && remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::uint64_t>(remaining, buffer.size()));
        success = ReadExactly(source, buffer.data(), chunk) &&
            WriteExactly(output, buffer.data(), chunk) &&
            BCryptHashData(hash, buffer.data(), chunk, 0) >= 0;
        remaining -= chunk;
    }
    if (success) {
        success = BCryptFinishHash(hash, actualHash.data(), static_cast<ULONG>(actualHash.size()), 0) >= 0 &&
            actualHash == trailer.sha256;
    }

    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(source);
    if (!success) DeleteFileW(destination.c_str());
    return success;
}

DWORD RunBackend(bool desktopShortcut) {
    const std::wstring tempDirectory = CreateUniqueTempDirectory();
    if (tempDirectory.empty()) return kPayloadFailure;
    const std::wstring backendPath = tempDirectory + L"\\viewtrious-core.exe";
    DWORD result = kPayloadFailure;
    if (ExtractPayload(backendPath)) {
        std::wstring command = L"\"" + backendPath + L"\" /S /VIEWTRIOUS_CUSTOM_UI /DESKTOP_SHORTCUT=";
        command += desktopShortcut ? L"1" : L"0";
        std::vector<wchar_t> commandBuffer(command.begin(), command.end());
        commandBuffer.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(
                backendPath.c_str(),
                commandBuffer.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                tempDirectory.c_str(),
                &startup,
                &process)) {
            WaitForSingleObject(process.hProcess, INFINITE);
            if (!GetExitCodeProcess(process.hProcess, &result)) result = kLaunchFailure;
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        } else {
            result = kLaunchFailure;
        }
    }
    DeleteFileW(backendPath.c_str());
    RemoveDirectoryW(tempDirectory.c_str());
    return result;
}

HRESULT EnsureDeviceResources() {
    if (g_app.renderTarget) return S_OK;
    RECT client{};
    GetClientRect(g_app.window, &client);
    const D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(client.right - client.left),
        static_cast<UINT32>(client.bottom - client.top));
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN),
        static_cast<float>(g_app.dpi),
        static_cast<float>(g_app.dpi));
    HRESULT result = g_app.d2dFactory->CreateHwndRenderTarget(
        properties,
        D2D1::HwndRenderTargetProperties(g_app.window, size),
        &g_app.renderTarget);
    if (FAILED(result)) return result;

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(kLogoResource), RT_RCDATA);
    const HGLOBAL loaded = resource ? LoadResource(instance, resource) : nullptr;
    auto* logoBytes = loaded ? static_cast<BYTE*>(LockResource(loaded)) : nullptr;
    const DWORD logoByteCount = resource ? SizeofResource(instance, resource) : 0;
    if (!logoBytes || logoByteCount == 0) return HRESULT_FROM_WIN32(ERROR_RESOURCE_DATA_NOT_FOUND);

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    result = g_app.wicFactory->CreateStream(&stream);
    if (SUCCEEDED(result)) result = stream->InitializeFromMemory(logoBytes, logoByteCount);
    if (SUCCEEDED(result)) {
        result = g_app.wicFactory->CreateDecoderFromStream(
            stream.Get(),
            nullptr,
            WICDecodeMetadataCacheOnLoad,
            &decoder);
    }
    if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(result)) result = g_app.wicFactory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result)) {
        result = converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(result)) {
        result = g_app.renderTarget->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &g_app.logo);
    }
    if (FAILED(result)) g_app.renderTarget.Reset();
    return result;
}

ComPtr<ID2D1SolidColorBrush> Brush(const D2D1_COLOR_F& color) {
    ComPtr<ID2D1SolidColorBrush> brush;
    g_app.renderTarget->CreateSolidColorBrush(color, &brush);
    return brush;
}

void FillRounded(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color) {
    const auto brush = Brush(color);
    g_app.renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
}

void StrokeRounded(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color, float width = 1.0f) {
    const auto brush = Brush(color);
    g_app.renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), width);
}

void DrawTextBlock(
    const std::wstring& text,
    IDWriteTextFormat* format,
    const D2D1_RECT_F& rect,
    const D2D1_COLOR_F& color,
    D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_CLIP) {
    const auto brush = Brush(color);
    g_app.renderTarget->DrawTextW(
        text.c_str(),
        static_cast<UINT32>(text.size()),
        format,
        rect,
        brush.Get(),
        options);
}

void DrawFacet(float x1, float y1, float x2, float y2, float x3, float y3, const D2D1_COLOR_F& color) {
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(g_app.d2dFactory->CreatePathGeometry(&geometry))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink))) return;
    sink->BeginFigure(D2D1::Point2F(x1, y1), D2D1_FIGURE_BEGIN_FILLED);
    const D2D1_POINT_2F points[] = {D2D1::Point2F(x2, y2), D2D1::Point2F(x3, y3)};
    sink->AddLines(points, 2);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return;
    const auto brush = Brush(color);
    g_app.renderTarget->FillGeometry(geometry.Get(), brush.Get());
}

void DrawButton(const D2D1_RECT_F& rect, const wchar_t* label, HitTarget target, bool primary, bool focused) {
    D2D1_COLOR_F fill = primary ? Color(0x078bd6) : Color(0x2c313a);
    if (g_app.hover == target) fill = primary ? Color(0x159de9) : Color(0x373d47);
    if (g_app.pressed == target) fill = primary ? Color(0x0477b8) : Color(0x232830);
    FillRounded(rect, 7.0f, fill);
    if (focused) StrokeRounded(D2D1::RectF(rect.left - 2.0f, rect.top - 2.0f, rect.right + 2.0f, rect.bottom + 2.0f), 9.0f, Color(0x72c7f4), 1.0f);
    DrawTextBlock(label, g_app.buttonFormat.Get(), rect, Color(0xffffff));
}

void DrawClose(const D2D1_RECT_F& rect, HitTarget target) {
    if (g_app.pressed == target) FillRounded(rect, 5.0f, Color(0x242930));
    else if (g_app.hover == target) FillRounded(rect, 5.0f, Color(0x3a4049));
    const auto brush = Brush(Color(0xd7dce4));
    g_app.renderTarget->DrawLine(D2D1::Point2F(rect.left + 9.0f, rect.top + 9.0f), D2D1::Point2F(rect.right - 9.0f, rect.bottom - 9.0f), brush.Get(), 1.5f);
    g_app.renderTarget->DrawLine(D2D1::Point2F(rect.right - 9.0f, rect.top + 9.0f), D2D1::Point2F(rect.left + 9.0f, rect.bottom - 9.0f), brush.Get(), 1.5f);
}

void DrawLegalOverlay() {
    FillRounded(D2D1::RectF(34.0f, 34.0f, 726.0f, 416.0f), 12.0f, Color(0x20242b));
    StrokeRounded(D2D1::RectF(34.0f, 34.0f, 726.0f, 416.0f), 12.0f, Color(0x4a515d));
    DrawTextBlock(L"License & notices", g_app.overlayTitleFormat.Get(), D2D1::RectF(58.0f, 48.0f, 500.0f, 82.0f), Color(0xffffff));
    DrawClose(OverlayCloseRect(), HitTarget::overlayClose);
    const D2D1_RECT_F content = D2D1::RectF(58.0f, 94.0f, 696.0f, 390.0f);
    g_app.renderTarget->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(g_app.writeFactory->CreateTextLayout(
            g_app.legalText.c_str(),
            static_cast<UINT32>(g_app.legalText.size()),
            g_app.bodyFormat.Get(),
            content.right - content.left - 18.0f,
            100000.0f,
            &layout))) {
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(layout->GetMetrics(&metrics))) {
            g_app.licenseExtent = std::max(0.0f, metrics.height - (content.bottom - content.top));
            g_app.licenseScroll = std::clamp(g_app.licenseScroll, 0.0f, g_app.licenseExtent);
        }
        const auto brush = Brush(Color(0xd6dbe4));
        g_app.renderTarget->DrawTextLayout(
            D2D1::Point2F(content.left, content.top - g_app.licenseScroll),
            layout.Get(),
            brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    g_app.renderTarget->PopAxisAlignedClip();
    if (g_app.licenseExtent > 0.0f) {
        const float trackHeight = content.bottom - content.top;
        const float thumbHeight = std::max(28.0f, trackHeight * trackHeight / (trackHeight + g_app.licenseExtent));
        const float thumbY = content.top + (trackHeight - thumbHeight) * (g_app.licenseScroll / g_app.licenseExtent);
        FillRounded(D2D1::RectF(704.0f, content.top, 708.0f, content.bottom), 2.0f, Color(0x353b45));
        FillRounded(D2D1::RectF(704.0f, thumbY, 708.0f, thumbY + thumbHeight), 2.0f, Color(0x82909f));
    }
}

void Paint() {
    PAINTSTRUCT paint{};
    BeginPaint(g_app.window, &paint);
    if (FAILED(EnsureDeviceResources())) {
        EndPaint(g_app.window, &paint);
        return;
    }
    g_app.renderTarget->BeginDraw();
    g_app.renderTarget->Clear(Color(0x171a1f));

    const auto leftBrush = Brush(Color(0x11151a));
    g_app.renderTarget->FillRectangle(D2D1::RectF(0.0f, 0.0f, 302.0f, kClientHeight), leftBrush.Get());
    DrawFacet(0.0f, 0.0f, 302.0f, 0.0f, 104.0f, 188.0f, Color(0x073a59, 0.68f));
    DrawFacet(302.0f, 0.0f, 302.0f, 238.0f, 104.0f, 188.0f, Color(0x123b27, 0.48f));
    DrawFacet(0.0f, 450.0f, 0.0f, 188.0f, 216.0f, 326.0f, Color(0x4a290e, 0.52f));
    DrawFacet(302.0f, 450.0f, 216.0f, 326.0f, 302.0f, 238.0f, Color(0x073249, 0.56f));
    DrawFacet(0.0f, 188.0f, 104.0f, 188.0f, 216.0f, 326.0f, Color(0x162027, 0.82f));
    FillRounded(D2D1::RectF(54.0f, 106.0f, 248.0f, 300.0f), 97.0f, Color(0x0c1116, 0.72f));
    StrokeRounded(D2D1::RectF(54.0f, 106.0f, 248.0f, 300.0f), 97.0f, Color(0x1b789f, 0.42f));
    if (g_app.logo) {
        g_app.renderTarget->DrawBitmap(
            g_app.logo.Get(),
            D2D1::RectF(62.0f, 114.0f, 240.0f, 292.0f),
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    DrawClose(CloseRect(), HitTarget::close);
    DrawTextBlock(L"viewtrious", g_app.titleFormat.Get(), D2D1::RectF(338.0f, 75.0f, 714.0f, 126.0f), Color(0xffffff));
    DrawTextBlock(L"extremely lightweight media viewer", g_app.taglineFormat.Get(), D2D1::RectF(338.0f, 127.0f, 714.0f, 159.0f), Color(0xc5ccd5));
    DrawTextBlock(L"version " VIEWTRIOUS_SETUP_VERSION, g_app.smallFormat.Get(), D2D1::RectF(338.0f, 164.0f, 714.0f, 188.0f), Color(0x7f8b99));

    if (g_app.state == SetupState::idle) {
        DrawButton(PrimaryRect(), L"INSTALL", HitTarget::primary, true, g_app.focusIndex == 0);
        const D2D1_RECT_F box = D2D1::RectF(340.0f, 314.0f, 358.0f, 332.0f);
        D2D1_COLOR_F checkboxFill = g_app.desktopShortcut ? Color(0x078bd6) : Color(0x242a32);
        if (g_app.pressed == HitTarget::desktopShortcut) checkboxFill = Color(0x1d2229);
        else if (!g_app.desktopShortcut && g_app.hover == HitTarget::desktopShortcut) checkboxFill = Color(0x303741);
        FillRounded(box, 4.0f, checkboxFill);
        StrokeRounded(box, 4.0f, g_app.focusIndex == 1 ? Color(0x72c7f4) : Color(0x596370));
        if (g_app.desktopShortcut) {
            const auto check = Brush(Color(0xffffff));
            g_app.renderTarget->DrawLine(D2D1::Point2F(344.0f, 323.0f), D2D1::Point2F(349.0f, 328.0f), check.Get(), 2.0f);
            g_app.renderTarget->DrawLine(D2D1::Point2F(349.0f, 328.0f), D2D1::Point2F(355.0f, 318.0f), check.Get(), 2.0f);
        }
        DrawTextBlock(L"Create a desktop shortcut", g_app.bodyFormat.Get(), D2D1::RectF(368.0f, 307.0f, 620.0f, 340.0f), Color(0xd6dbe4));
        DrawTextBlock(L"install location  %LOCALAPPDATA%\\viewtrious", g_app.smallFormat.Get(), D2D1::RectF(338.0f, 351.0f, 714.0f, 378.0f), Color(0x8995a3));
        D2D1_COLOR_F licenseColor = Color(0x29a8ed);
        if (g_app.pressed == HitTarget::license) licenseColor = Color(0x1586c1);
        else if (g_app.hover == HitTarget::license) licenseColor = Color(0x5ec5ff);
        DrawTextBlock(L"License & notices", g_app.bodyFormat.Get(), LicenseRect(), licenseColor);
        if (g_app.focusIndex == 2) StrokeRounded(LicenseRect(), 4.0f, Color(0x72c7f4));
    } else if (g_app.state == SetupState::installing) {
        DrawTextBlock(L"installing viewtrious", g_app.overlayTitleFormat.Get(), D2D1::RectF(338.0f, 226.0f, 714.0f, 264.0f), Color(0xffffff));
        FillRounded(D2D1::RectF(338.0f, 285.0f, 714.0f, 289.0f), 2.0f, Color(0x303640));
        const float phase = static_cast<float>((GetTickCount64() - g_app.installStarted) % 1450) / 1450.0f;
        const float left = 338.0f + phase * 446.0f - 70.0f;
        const D2D1_RECT_F progress = D2D1::RectF(std::max(338.0f, left), 285.0f, std::min(714.0f, left + 70.0f), 289.0f);
        if (progress.right > progress.left) FillRounded(progress, 2.0f, Color(0x16a9e8));
        DrawTextBlock(L"this usually takes only a moment", g_app.smallFormat.Get(), D2D1::RectF(338.0f, 306.0f, 714.0f, 332.0f), Color(0x8995a3));
    } else if (g_app.state == SetupState::succeeded) {
        DrawTextBlock(L"viewtrious is ready", g_app.overlayTitleFormat.Get(), D2D1::RectF(338.0f, 205.0f, 714.0f, 244.0f), Color(0xffffff));
        DrawButton(PrimaryRect(), L"LAUNCH VIEWTRIOUS", HitTarget::primary, true, g_app.focusIndex == 0);
        DrawButton(SecondaryRect(), L"CLOSE", HitTarget::secondary, false, g_app.focusIndex == 1);
    } else {
        std::wstring message = L"viewtrious could not be installed";
        if (g_app.failureCode == kBackendExplorerLocked) {
            message = L"Explorer is using the Viewtrious STL thumbnail extension.\nClose open File Explorer windows, then try again.";
        } else if (g_app.failureCode == kBackendApplicationLocked) {
            message = L"viewtrious is currently running.\nClose viewtrious, then try again.";
        } else if (g_app.failureCode == kBackendIntegrationFailed) {
            message = L"Windows integration could not be registered.\nTry the installation again.";
        }
        DrawTextBlock(message, g_app.bodyFormat.Get(), D2D1::RectF(338.0f, 190.0f, 714.0f, 244.0f), Color(0xe6e9ee));
        DrawButton(PrimaryRect(), L"RETRY", HitTarget::primary, true, g_app.focusIndex == 0);
        DrawButton(SecondaryRect(), L"CLOSE", HitTarget::secondary, false, g_app.focusIndex == 1);
        const std::wstring code = L"backend exit code " + std::to_wstring(g_app.failureCode);
        DrawTextBlock(code, g_app.smallFormat.Get(), D2D1::RectF(338.0f, 360.0f, 714.0f, 385.0f), Color(0x8995a3));
    }
    if (g_app.licenseOpen) DrawLegalOverlay();

    const HRESULT result = g_app.renderTarget->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) {
        g_app.logo.Reset();
        g_app.renderTarget.Reset();
    }
    EndPaint(g_app.window, &paint);
}

HitTarget HitTest(float x, float y) {
    if (g_app.licenseOpen) return Contains(OverlayCloseRect(), x, y) ? HitTarget::overlayClose : HitTarget::none;
    if (Contains(CloseRect(), x, y)) return HitTarget::close;
    if (g_app.state == SetupState::idle) {
        if (Contains(PrimaryRect(), x, y)) return HitTarget::primary;
        if (Contains(CheckboxRect(), x, y)) return HitTarget::desktopShortcut;
        if (Contains(LicenseRect(), x, y)) return HitTarget::license;
    } else if (g_app.state == SetupState::succeeded || g_app.state == SetupState::failed) {
        if (Contains(PrimaryRect(), x, y)) return HitTarget::primary;
        if (Contains(SecondaryRect(), x, y)) return HitTarget::secondary;
    }
    return HitTarget::none;
}

void StartInstall() {
    if (g_app.state == SetupState::installing) return;
    g_app.state = SetupState::installing;
    g_app.failureCode = 0;
    g_app.installStarted = GetTickCount64();
    g_app.hover = HitTarget::none;
    g_app.pressed = HitTarget::none;
    SetTimer(g_app.window, kAnimationTimer, 16, nullptr);
    InvalidateRect(g_app.window, nullptr, FALSE);
    const HWND window = g_app.window;
    const bool desktopShortcut = g_app.desktopShortcut;
    std::thread([window, desktopShortcut]() {
        const DWORD result = RunBackend(desktopShortcut);
        const ULONGLONG elapsed = GetTickCount64() - g_app.installStarted;
        if (elapsed < 550) Sleep(static_cast<DWORD>(550 - elapsed));
        PostMessageW(window, kInstallCompleteMessage, static_cast<WPARAM>(result), 0);
    }).detach();
}

void LaunchInstalledApplication() {
    PWSTR localAppData{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        g_app.state = SetupState::failed;
        g_app.failureCode = kLaunchFailure;
        return;
    }
    const std::wstring application = std::wstring(localAppData) + L"\\viewtrious\\app\\Viewtrious.exe";
    CoTaskMemFree(localAppData);
    const HINSTANCE launched = ShellExecuteW(g_app.window, L"open", application.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(launched) > 32) {
        DestroyWindow(g_app.window);
    } else {
        g_app.state = SetupState::failed;
        g_app.failureCode = kLaunchFailure;
        InvalidateRect(g_app.window, nullptr, FALSE);
    }
}

void Activate(HitTarget target) {
    if (g_app.licenseOpen) {
        if (target == HitTarget::overlayClose) {
            g_app.licenseOpen = false;
            InvalidateRect(g_app.window, nullptr, FALSE);
        }
        return;
    }
    switch (target) {
    case HitTarget::primary:
        if (g_app.state == SetupState::idle || g_app.state == SetupState::failed) StartInstall();
        else if (g_app.state == SetupState::succeeded) LaunchInstalledApplication();
        break;
    case HitTarget::secondary:
        if (g_app.state == SetupState::succeeded || g_app.state == SetupState::failed) DestroyWindow(g_app.window);
        break;
    case HitTarget::desktopShortcut:
        if (g_app.state == SetupState::idle) {
            g_app.desktopShortcut = !g_app.desktopShortcut;
            InvalidateRect(g_app.window, nullptr, FALSE);
        }
        break;
    case HitTarget::license:
        if (g_app.state == SetupState::idle) {
            g_app.licenseOpen = true;
            g_app.licenseScroll = 0.0f;
            InvalidateRect(g_app.window, nullptr, FALSE);
        }
        break;
    case HitTarget::close:
        if (g_app.state != SetupState::installing) DestroyWindow(g_app.window);
        else MessageBeep(MB_ICONINFORMATION);
        break;
    default:
        break;
    }
}

void HandleKeyboard(WPARAM key) {
    if (g_app.licenseOpen) {
        if (key == VK_ESCAPE) Activate(HitTarget::overlayClose);
        else if (key == VK_HOME) g_app.licenseScroll = 0.0f;
        else if (key == VK_END) g_app.licenseScroll = g_app.licenseExtent;
        else if (key == VK_PRIOR) g_app.licenseScroll = std::max(0.0f, g_app.licenseScroll - 260.0f);
        else if (key == VK_NEXT) g_app.licenseScroll = std::min(g_app.licenseExtent, g_app.licenseScroll + 260.0f);
        else return;
        InvalidateRect(g_app.window, nullptr, FALSE);
        return;
    }
    if (key == VK_ESCAPE) {
        if (g_app.state != SetupState::installing) DestroyWindow(g_app.window);
        return;
    }
    if (key == VK_TAB) {
        const int focusCount = g_app.state == SetupState::idle ? 3 : 2;
        const int direction = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
        g_app.focusIndex = (g_app.focusIndex + direction + focusCount) % focusCount;
        InvalidateRect(g_app.window, nullptr, FALSE);
        return;
    }
    if (key != VK_RETURN && key != VK_SPACE) return;
    if (g_app.state == SetupState::idle) {
        const HitTarget targets[] = {HitTarget::primary, HitTarget::desktopShortcut, HitTarget::license};
        Activate(targets[std::clamp(g_app.focusIndex, 0, 2)]);
    } else if (g_app.state == SetupState::succeeded || g_app.state == SetupState::failed) {
        Activate(g_app.focusIndex == 0 ? HitTarget::primary : HitTarget::secondary);
    }
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT:
        Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_app.renderTarget) {
            g_app.renderTarget->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        return 0;
    case WM_DPICHANGED: {
        g_app.dpi = HIWORD(wParam);
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOACTIVATE | SWP_NOZORDER);
        if (g_app.renderTarget) g_app.renderTarget->SetDpi(static_cast<float>(g_app.dpi), static_cast<float>(g_app.dpi));
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float x = PixelToDip(GET_X_LPARAM(lParam), g_app.dpi);
        const float y = PixelToDip(GET_Y_LPARAM(lParam), g_app.dpi);
        const HitTarget target = HitTest(x, y);
        if (target != g_app.hover) {
            g_app.hover = target;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        SetCursor(LoadCursorW(nullptr, target == HitTarget::none ? IDC_ARROW : IDC_HAND));
        return 0;
    }
    case WM_MOUSELEAVE:
        g_app.hover = HitTarget::none;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(window);
        const float x = PixelToDip(GET_X_LPARAM(lParam), g_app.dpi);
        const float y = PixelToDip(GET_Y_LPARAM(lParam), g_app.dpi);
        g_app.pressed = HitTest(x, y);
        if (g_app.pressed != HitTarget::none) {
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
        } else if (!g_app.licenseOpen && g_app.state != SetupState::installing && (x < 302.0f || y < 62.0f)) {
            ReleaseCapture();
            SendMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const float x = PixelToDip(GET_X_LPARAM(lParam), g_app.dpi);
        const float y = PixelToDip(GET_Y_LPARAM(lParam), g_app.dpi);
        const HitTarget released = HitTest(x, y);
        const HitTarget pressed = g_app.pressed;
        g_app.pressed = HitTarget::none;
        ReleaseCapture();
        if (pressed != HitTarget::none && released == pressed) Activate(pressed);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (g_app.licenseOpen) {
            const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA);
            g_app.licenseScroll = std::clamp(g_app.licenseScroll - delta * 54.0f, 0.0f, g_app.licenseExtent);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        HandleKeyboard(wParam);
        return 0;
    case WM_CLOSE:
        if (g_app.state == SetupState::installing) {
            MessageBeep(MB_ICONINFORMATION);
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_TIMER:
        if (wParam == kAnimationTimer && g_app.state == SetupState::installing) {
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    case kInstallCompleteMessage:
        KillTimer(window, kAnimationTimer);
        g_app.failureCode = static_cast<DWORD>(wParam);
        g_app.state = g_app.failureCode == 0 ? SetupState::succeeded : SetupState::failed;
        g_app.focusIndex = 0;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool InitializeFactories(HINSTANCE instance) {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_app.d2dFactory.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(g_app.writeFactory.GetAddressOf())))) return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_app.wicFactory)))) return false;
    if (FAILED(CreateTextFormat(g_app.writeFactory.Get(), 35.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, &g_app.titleFormat))) return false;
    if (FAILED(CreateTextFormat(g_app.writeFactory.Get(), 16.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, &g_app.taglineFormat))) return false;
    if (FAILED(CreateTextFormat(g_app.writeFactory.Get(), 14.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, &g_app.bodyFormat))) return false;
    if (FAILED(CreateTextFormat(g_app.writeFactory.Get(), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, &g_app.smallFormat))) return false;
    if (FAILED(CreateTextFormat(g_app.writeFactory.Get(), 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, &g_app.buttonFormat))) return false;
    if (FAILED(CreateTextFormat(g_app.writeFactory.Get(), 20.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, &g_app.overlayTitleFormat))) return false;
    g_app.bodyFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    g_app.legalText = L"VIEWTRIOUS LICENSE\n\n" + Utf8Resource(instance, kLicenseResource) +
        L"\n\n\nTHIRD-PARTY NOTICES\n\n" + Utf8Resource(instance, kNoticeResource) +
        L"\n\nminiz\n\n" + Utf8Resource(instance, kMinizResource);
    return !g_app.legalText.empty();
}

void EnablePerMonitorDpiAwareness() {
    using SetDpiAwarenessContextFunction = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto setAwareness = reinterpret_cast<SetDpiAwarenessContextFunction>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setAwareness) setAwareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    EnablePerMonitorDpiAwareness();
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult)) return 1;
    if (!InitializeFactories(instance)) {
        CoUninitialize();
        return 1;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kSetupIconResource));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIconSm = windowClass.hIcon;
    if (!RegisterClassExW(&windowClass)) {
        CoUninitialize();
        return 1;
    }

    g_app.dpi = GetDpiForSystem();
    RECT windowRect{0, 0,
        MulDiv(static_cast<int>(kClientWidth), static_cast<int>(g_app.dpi), 96),
        MulDiv(static_cast<int>(kClientHeight), static_cast<int>(g_app.dpi), 96)};
    const DWORD style = WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&windowRect, style, FALSE, 0, g_app.dpi);
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    GetMonitorInfoW(monitor, &monitorInfo);
    const int x = monitorInfo.rcWork.left + ((monitorInfo.rcWork.right - monitorInfo.rcWork.left) - width) / 2;
    const int y = monitorInfo.rcWork.top + ((monitorInfo.rcWork.bottom - monitorInfo.rcWork.top) - height) / 2;

    g_app.window = CreateWindowExW(
        0,
        kWindowClass,
        L"viewtrious setup",
        style,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!g_app.window) {
        CoUninitialize();
        return 1;
    }
    const UINT windowDpi = GetDpiForWindow(g_app.window);
    if (windowDpi != 0) g_app.dpi = windowDpi;
    RECT finalRect{0, 0,
        MulDiv(static_cast<int>(kClientWidth), static_cast<int>(g_app.dpi), 96),
        MulDiv(static_cast<int>(kClientHeight), static_cast<int>(g_app.dpi), 96)};
    AdjustWindowRectExForDpi(&finalRect, style, FALSE, 0, g_app.dpi);
    const int finalWidth = finalRect.right - finalRect.left;
    const int finalHeight = finalRect.bottom - finalRect.top;
    SetWindowPos(
        g_app.window,
        nullptr,
        monitorInfo.rcWork.left + ((monitorInfo.rcWork.right - monitorInfo.rcWork.left) - finalWidth) / 2,
        monitorInfo.rcWork.top + ((monitorInfo.rcWork.bottom - monitorInfo.rcWork.top) - finalHeight) / 2,
        finalWidth,
        finalHeight,
        SWP_NOACTIVATE | SWP_NOZORDER);
    constexpr DWM_WINDOW_CORNER_PREFERENCE cornerPreference = DWMWCP_ROUND;
    DwmSetWindowAttribute(
        g_app.window,
        DWMWA_WINDOW_CORNER_PREFERENCE,
        &cornerPreference,
        sizeof(cornerPreference));
    ShowWindow(g_app.window, showCommand == 0 ? SW_SHOWNORMAL : showCommand);
    UpdateWindow(g_app.window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_app.logo.Reset();
    g_app.renderTarget.Reset();
    g_app.wicFactory.Reset();
    g_app.writeFactory.Reset();
    g_app.d2dFactory.Reset();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
