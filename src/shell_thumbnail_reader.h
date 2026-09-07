#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

struct ShellThumbnailPixels {
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    std::shared_ptr<std::vector<BYTE>> pixels;
};

// Obtains a Windows Shell thumbnail, copies it to Viewtrious-owned PBGRA RAM, and
// releases the Shell item, thumbnail factory, and HBITMAP before returning.
HRESULT DecodeShellVideoThumbnailPixels(const std::wstring& path, UINT requestedSize, ShellThumbnailPixels& decoded, float& aspect);
