#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

struct VideoThumbnailPixels {
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    std::shared_ptr<std::vector<BYTE>> pixels;
};

// Decodes one static frame through a short-lived source reader. On return, pixels is
// Viewtrious-owned PBGRA RAM; no Media Foundation object or source-file handle survives.
HRESULT DecodeVideoThumbnailPixels(const std::wstring& path, UINT targetHeight, VideoThumbnailPixels& decoded, float& aspect);
