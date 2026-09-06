#pragma once

#include <cstdint>

constexpr uint32_t kViewtriousAiAbiVersion = 1;
constexpr uint32_t kViewtriousAiPixelFormatBgra8 = 1;

struct ViewtriousAiImageV1 {
    const uint8_t* pixels = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t pixelFormat = 0;
};

struct ViewtriousAiAdjustmentResultV1 {
    float brightness = 0.0f;
    float contrast = 0.0f;
    float shadows = 0.0f;
    float highlights = 0.0f;
    float confidence = 0.0f;
    uint32_t flags = 0;
};

extern "C" {
using ViewtriousAiGetAbiVersionFn = uint32_t (*)();
using ViewtriousAiInitializeFn = bool (*)(const wchar_t* addonDirectory);
using ViewtriousAiAnalyzeFn = bool (*)(const ViewtriousAiImageV1*, ViewtriousAiAdjustmentResultV1*);
using ViewtriousAiShutdownFn = void (*)();
}
