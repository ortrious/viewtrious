#pragma once

#include "viewtrious_ai_abi.h"

#include <windows.h>

#include <string>
#include <vector>

struct AiImageBuffer {
    std::vector<unsigned char> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    bool Valid() const { return !pixels.empty() && width && height && stride >= width * 4; }
};

class AiAddonLoader {
public:
    ~AiAddonLoader();
    bool Initialize();
    void Shutdown();
    bool Available() const { return module_ != nullptr && analyze_ != nullptr; }
    bool Analyze(const AiImageBuffer& image, ViewtriousAiAdjustmentResultV1& result) const;

private:
    HMODULE module_ = nullptr;
    ViewtriousAiInitializeFn initialize_ = nullptr;
    ViewtriousAiAnalyzeFn analyze_ = nullptr;
    ViewtriousAiShutdownFn shutdown_ = nullptr;
};
