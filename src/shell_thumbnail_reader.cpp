#include "shell_thumbnail_reader.h"

#include <shobjidl_core.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <limits>

using Microsoft::WRL::ComPtr;

HRESULT DecodeShellVideoThumbnailPixels(const std::wstring& path, UINT requestedSize, ShellThumbnailPixels& decoded, float& aspect) {
    decoded = {};
    aspect = 1.0f;
    if (path.empty() || !requestedSize) return E_INVALIDARG;

    HRESULT hr = E_FAIL;
    UINT width = 0;
    UINT height = 0;
    {
        ComPtr<IShellItem> item;
        ComPtr<IShellItemImageFactory> imageFactory;
        HBITMAP thumbnail = nullptr;
        hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
        if (SUCCEEDED(hr)) hr = item.As(&imageFactory);
        const SIZE size{ static_cast<LONG>(requestedSize), static_cast<LONG>(requestedSize) };
        const SIIGBF flags = static_cast<SIIGBF>(SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY);
        if (SUCCEEDED(hr)) hr = imageFactory->GetImage(size, flags, &thumbnail);

        BITMAP bitmap{};
        if (SUCCEEDED(hr) && (!thumbnail || !GetObjectW(thumbnail, sizeof(bitmap), &bitmap) || bitmap.bmWidth <= 0 || bitmap.bmHeight == 0))
            hr = E_FAIL;
        if (SUCCEEDED(hr)) {
            width = static_cast<UINT>(bitmap.bmWidth);
            height = static_cast<UINT>(std::abs(bitmap.bmHeight));
            if (width > UINT_MAX / 4 || height > UINT_MAX / (width * 4)) hr = E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hr)) {
            auto pixels = std::make_shared<std::vector<BYTE>>(static_cast<size_t>(width) * height * 4);
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = static_cast<LONG>(width);
            info.bmiHeader.biHeight = -static_cast<LONG>(height);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            HDC dc = CreateCompatibleDC(nullptr);
            const int copied = dc ? GetDIBits(dc, thumbnail, 0, height, pixels->data(), &info, DIB_RGB_COLORS) : 0;
            if (dc) DeleteDC(dc);
            if (copied != static_cast<int>(height)) hr = E_FAIL;
            if (SUCCEEDED(hr)) {
                // Video thumbnails are opaque. Keep the shared thumbnail path PBGRA-compatible.
                for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) (*pixels)[(static_cast<size_t>(y) * width + x) * 4 + 3] = 255;
                decoded.width = width;
                decoded.height = height;
                decoded.stride = width * 4;
                decoded.pixels = std::move(pixels);
            }
        }
        if (thumbnail) DeleteObject(thumbnail);
        imageFactory.Reset();
        item.Reset();
    }
    if (SUCCEEDED(hr) && decoded.pixels) aspect = static_cast<float>(decoded.width) / decoded.height;
    return SUCCEEDED(hr) && decoded.pixels ? S_OK : FAILED(hr) ? hr : E_FAIL;
}
