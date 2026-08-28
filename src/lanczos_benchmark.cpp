#include "lanczos_resampler.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {
struct PixelBuffer { UINT width = 0, height = 0; std::vector<BYTE> pixels; };
bool IsPremultipliedPbgra(const std::vector<BYTE>& pixels) {
    for (size_t index = 0; index < pixels.size(); index += 4)
        if (pixels[index] > pixels[index + 3] || pixels[index + 1] > pixels[index + 3] || pixels[index + 2] > pixels[index + 3]) return false;
    return true;
}
bool DecodePbgra(const fs::path& path, PixelBuffer& decoded) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = frame->GetSize(&decoded.width, &decoded.height);
    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr) || !decoded.width || !decoded.height) return false;
    decoded.pixels.resize(static_cast<size_t>(decoded.width) * decoded.height * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, decoded.width * 4, static_cast<UINT>(decoded.pixels.size()), decoded.pixels.data()));
}
bool RunCase(const PixelBuffer& source, float scale) {
    const UINT width = std::max(1u, static_cast<UINT>(std::lround(source.width * scale)));
    const UINT height = std::max(1u, static_cast<UINT>(std::lround(source.height * scale)));
    viewtrious::Lanczos3Scaler scaler;
    if (!scaler.Initialize(source.width, source.height, width, height)) return false;
    std::vector<BYTE> output;
    constexpr int kIterations = 3;
    scaler.Scale(source.pixels.data(), source.width * 4, output);
    const auto start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < kIterations; ++iteration) scaler.Scale(source.pixels.data(), source.width * 4, output);
    const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / kIterations;
    std::wprintf(L"%5.1f%%  %4u x %-4u  %7.2f ms  working %7.2f MiB  output %7.2f MiB\n", scale * 100.0f, width, height,
        elapsed, scaler.WorkingBytes() / (1024.0 * 1024.0), output.size() / (1024.0 * 1024.0));
    return IsPremultipliedPbgra(output);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const fs::path path = argc > 1 ? fs::path(argv[1]) : fs::path(L"assets/ViewtriousLogoRuntime.png");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    PixelBuffer source;
    if ((FAILED(com) && com != RPC_E_CHANGED_MODE) || !DecodePbgra(path, source)) {
        std::fwprintf(stderr, L"Could not decode 32bppPBGRA benchmark input: %ls\n", path.c_str());
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    std::wprintf(L"Lanczos3 benchmark (32bppPBGRA): %ls (%u x %u)\n", path.c_str(), source.width, source.height);
    std::wprintf(L" scale  output          average       bounded storage      output storage\n");
    bool pbgraValid = IsPremultipliedPbgra(source.pixels);
    for (const float scale : { 1.25f, 1.50f, 2.00f, 3.00f, 0.50f, 0.25f, 0.125f }) pbgraValid = RunCase(source, scale) && pbgraValid;
    std::wprintf(L"premultiplied-alpha invariant: %ls\n", pbgraValid ? L"passed" : L"FAILED");
    if (SUCCEEDED(com)) CoUninitialize();
    return pbgraValid ? 0 : 1;
}
