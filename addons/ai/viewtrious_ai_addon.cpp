#include "viewtrious_ai_abi.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

namespace {
constexpr int kModelEdge = 320;
Ort::Env g_environment(ORT_LOGGING_LEVEL_WARNING, "ViewtriousAI");
std::mutex g_mutex;
std::unique_ptr<Ort::Session> g_session;
Ort::SessionOptions g_options;

float Luminance(const uint8_t* pixel) {
    const float alpha = pixel[3] / 255.0f;
    if (alpha <= 0.001f) return -1.0f;
    return std::clamp((0.0722f * pixel[0] + 0.7152f * pixel[1] + 0.2126f * pixel[2]) / (255.0f * alpha), 0.0f, 1.0f);
}

float Percentile(std::vector<float> values, float fraction) {
    if (values.empty()) return 0.5f;
    const size_t index = std::min(values.size() - 1, static_cast<size_t>(std::floor(fraction * (values.size() - 1))));
    std::nth_element(values.begin(), values.begin() + index, values.end());
    return values[index];
}

float WeightedPercentile(std::vector<std::pair<float, float>> values, float fraction) {
    if (values.empty()) return 0.5f;
    std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
    float total = 0.0f; for (const auto& value : values) total += value.second;
    float accumulated = 0.0f;
    for (const auto& value : values) { accumulated += value.second; if (accumulated >= total * fraction) return value.first; }
    return values.back().first;
}

void ComputeAdjustments(const ViewtriousAiImageV1& image, const float* mask, ViewtriousAiAdjustmentResultV1& result) {
    std::vector<float> global, background;
    std::vector<std::pair<float, float>> salient;
    float salientWeight = 0.0f, allWeight = 0.0f, maskConfidence = 0.0f;
    for (uint32_t y = 0; y < image.height; ++y) for (uint32_t x = 0; x < image.width; ++x) {
        const uint8_t* pixel = image.pixels + static_cast<size_t>(y) * image.stride + x * 4;
        const float luminance = Luminance(pixel); if (luminance < 0.0f) continue;
        const float alpha = pixel[3] / 255.0f;
        const int mx = std::min(kModelEdge - 1, static_cast<int>(x * kModelEdge / image.width));
        const int my = std::min(kModelEdge - 1, static_cast<int>(y * kModelEdge / image.height));
        const float saliency = std::clamp(mask[my * kModelEdge + mx], 0.0f, 1.0f) * alpha;
        global.push_back(luminance); allWeight += alpha; maskConfidence += saliency;
        if (saliency >= 0.20f) { salient.emplace_back(luminance, saliency); salientWeight += saliency; }
        if (saliency < 0.20f) background.push_back(luminance);
    }
    if (global.empty()) return;
    const float p10 = Percentile(global, .10f), p25 = Percentile(global, .25f), p50 = Percentile(global, .50f), p95 = Percentile(global, .95f), p99 = Percentile(global, .99f);
    const float shadowOccupancy = static_cast<float>(std::count_if(global.begin(), global.end(), [](float value) { return value < .16f; })) / global.size();
    const float highlightOccupancy = static_cast<float>(std::count_if(global.begin(), global.end(), [](float value) { return value > .88f; })) / global.size();
    const float coverage = allWeight > 0.0f ? salientWeight / allWeight : 0.0f;
    const bool confident = coverage >= .04f && maskConfidence / std::max(1.0f, allWeight) >= .03f;
    const float foregroundP25 = confident ? WeightedPercentile(salient, .25f) : p25;
    const float foregroundP50 = confident ? WeightedPercentile(salient, .50f) : p50;
    const float backgroundP95 = background.empty() ? p95 : Percentile(background, .95f);
    const bool backlit = foregroundP50 < .34f && backgroundP95 > .78f && shadowOccupancy > .12f;
    result.brightness = std::clamp((.46f - foregroundP50) * (backlit ? .55f : .34f), -.35f, .45f);
    result.shadows = std::clamp((.30f - foregroundP25) * (backlit ? 1.55f : .78f), -.20f, .65f);
    result.highlights = std::clamp((.84f - std::max(p95, p99 * .96f)) * .55f, -.50f, .25f);
    if (backlit || highlightOccupancy > .08f) result.highlights = std::min(result.highlights, 0.0f);
    const float spread = p95 - p10;
    result.contrast = backlit || shadowOccupancy > .20f ? std::clamp(-.10f - shadowOccupancy * .35f, -.35f, 0.0f) : std::clamp((.55f - spread) * .35f, -.35f, .30f);
    result.confidence = std::clamp(confident ? .45f + coverage : .22f, 0.0f, 1.0f);
}
}

extern "C" __declspec(dllexport) uint32_t ViewtriousAiGetAbiVersion() { return kViewtriousAiAbiVersion; }

extern "C" __declspec(dllexport) bool ViewtriousAiInitialize(const wchar_t* addonDirectory) {
    std::scoped_lock lock(g_mutex);
    try {
        if (!addonDirectory) return false;
        const std::filesystem::path model = std::filesystem::path(addonDirectory) / L"u2netp.onnx";
        if (!std::filesystem::exists(model)) return false;
        g_options.SetIntraOpNumThreads(1); g_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
        g_session = std::make_unique<Ort::Session>(g_environment, model.c_str(), g_options);
        return true;
    } catch (...) { g_session.reset(); return false; }
}

extern "C" __declspec(dllexport) bool ViewtriousAiAnalyze(const ViewtriousAiImageV1* image, ViewtriousAiAdjustmentResultV1* result) {
    std::scoped_lock lock(g_mutex);
    if (!g_session || !image || !result || !image->pixels || image->pixelFormat != kViewtriousAiPixelFormatBgra8 || !image->width || !image->height || image->stride < image->width * 4) return false;
    try {
        std::vector<float> input(static_cast<size_t>(3) * kModelEdge * kModelEdge);
        for (int y = 0; y < kModelEdge; ++y) for (int x = 0; x < kModelEdge; ++x) {
            const uint32_t sx = std::min(image->width - 1, static_cast<uint32_t>(x * image->width / kModelEdge));
            const uint32_t sy = std::min(image->height - 1, static_cast<uint32_t>(y * image->height / kModelEdge));
            const uint8_t* pixel = image->pixels + static_cast<size_t>(sy) * image->stride + sx * 4;
            const size_t index = static_cast<size_t>(y) * kModelEdge + x;
            input[index] = pixel[2] / 255.0f; input[kModelEdge * kModelEdge + index] = pixel[1] / 255.0f; input[2 * kModelEdge * kModelEdge + index] = pixel[0] / 255.0f;
        }
        const std::array<int64_t, 4> shape{ 1, 3, kModelEdge, kModelEdge };
        Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());
        Ort::AllocatorWithDefaultOptions allocator;
        auto inputName = g_session->GetInputNameAllocated(0, allocator);
        auto outputName = g_session->GetOutputNameAllocated(0, allocator);
        const char* inputNames[] = { inputName.get() }; const char* outputNames[] = { outputName.get() };
        auto outputs = g_session->Run(Ort::RunOptions{ nullptr }, inputNames, &tensor, 1, outputNames, 1);
        const float* raw = outputs[0].GetTensorData<float>();
        float minimum = raw[0], maximum = raw[0]; for (int index = 1; index < kModelEdge * kModelEdge; ++index) { minimum = std::min(minimum, raw[index]); maximum = std::max(maximum, raw[index]); }
        std::vector<float> normalized(kModelEdge * kModelEdge); const float range = std::max(.0001f, maximum - minimum);
        for (int index = 0; index < kModelEdge * kModelEdge; ++index) normalized[index] = (raw[index] - minimum) / range;
        *result = {}; ComputeAdjustments(*image, normalized.data(), *result); return result->confidence > 0.0f;
    } catch (...) { return false; }
}

extern "C" __declspec(dllexport) void ViewtriousAiShutdown() { std::scoped_lock lock(g_mutex); g_session.reset(); }
