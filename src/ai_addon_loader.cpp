#include "ai_addon_loader.h"

#include <filesystem>

namespace {
std::filesystem::path AddonDirectory() {
    wchar_t executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!length || length == ARRAYSIZE(executable)) return {};
    return std::filesystem::path(executable).parent_path() / L"Addons" / L"AI";
}
}

AiAddonLoader::~AiAddonLoader() { Shutdown(); }

bool AiAddonLoader::Initialize() {
    Shutdown();
    const std::filesystem::path directory = AddonDirectory();
    const std::filesystem::path path = directory / L"ViewtriousAI.dll";
    if (directory.empty() || !std::filesystem::exists(path)) return false;
    module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module_) return false;
    const auto version = reinterpret_cast<ViewtriousAiGetAbiVersionFn>(GetProcAddress(module_, "ViewtriousAiGetAbiVersion"));
    initialize_ = reinterpret_cast<ViewtriousAiInitializeFn>(GetProcAddress(module_, "ViewtriousAiInitialize"));
    analyze_ = reinterpret_cast<ViewtriousAiAnalyzeFn>(GetProcAddress(module_, "ViewtriousAiAnalyze"));
    shutdown_ = reinterpret_cast<ViewtriousAiShutdownFn>(GetProcAddress(module_, "ViewtriousAiShutdown"));
    if (!version || !initialize_ || !analyze_ || !shutdown_ || version() != kViewtriousAiAbiVersion || !initialize_(directory.c_str())) { Shutdown(); return false; }
    return true;
}

void AiAddonLoader::Shutdown() {
    if (module_ && shutdown_) shutdown_();
    initialize_ = nullptr; analyze_ = nullptr; shutdown_ = nullptr;
    if (module_) FreeLibrary(module_);
    module_ = nullptr;
}

bool AiAddonLoader::Analyze(const AiImageBuffer& image, ViewtriousAiAdjustmentResultV1& result) const {
    if (!Available() || !image.Valid()) return false;
    const ViewtriousAiImageV1 input{ image.pixels.data(), image.width, image.height, image.stride, kViewtriousAiPixelFormatBgra8 };
    return analyze_(&input, &result);
}
