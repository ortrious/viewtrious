#include "video_thumbnail_reader.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>

using Microsoft::WRL::ComPtr;

namespace {

constexpr LONGLONG kHundredNanosecondsPerSecond = 10'000'000;

HRESULT CopyVideoSamplePixels(IMFSample* sample, IMFMediaType* mediaType, std::vector<BYTE>& pixels, UINT& width, UINT& height) {
    if (!sample || !mediaType) return E_INVALIDARG;
    HRESULT hr = MFGetAttributeSize(mediaType, MF_MT_FRAME_SIZE, &width, &height);
    if (FAILED(hr) || !width || !height || width > UINT_MAX / 4 || height > UINT_MAX / (width * 4)) return FAILED(hr) ? hr : E_FAIL;

    UINT32 strideValue = 0;
    INT32 stride = 0;
    if (SUCCEEDED(mediaType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideValue))) stride = static_cast<INT32>(strideValue);
    else stride = static_cast<INT32>(width * 4);
    const uint64_t absoluteStride = stride < 0 ? static_cast<uint64_t>(-static_cast<int64_t>(stride)) : static_cast<uint64_t>(stride);
    if (absoluteStride < static_cast<uint64_t>(width) * 4) return E_FAIL;

    ComPtr<IMFMediaBuffer> buffer;
    hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr)) return hr;
    BYTE* source = nullptr;
    DWORD maximumLength = 0;
    DWORD currentLength = 0;
    hr = buffer->Lock(&source, &maximumLength, &currentLength);
    if (FAILED(hr)) return hr;

    const uint64_t requiredBytes = absoluteStride * height;
    if (!source || currentLength < requiredBytes) {
        buffer->Unlock();
        return E_FAIL;
    }
    pixels.resize(static_cast<size_t>(width) * height * 4);
    const BYTE* row = source;
    if (stride < 0) row += absoluteStride * (height - 1);
    for (UINT y = 0; y < height; ++y) {
        BYTE* destination = pixels.data() + static_cast<size_t>(y) * width * 4;
        std::memcpy(destination, row, static_cast<size_t>(width) * 4);
        for (UINT x = 0; x < width; ++x) destination[x * 4 + 3] = 255;
        row += stride;
    }
    buffer->Unlock();
    return S_OK;
}

HRESULT ScaleVideoThumbnail(const std::vector<BYTE>& sourcePixels, UINT sourceWidth, UINT sourceHeight,
    UINT targetHeight, VideoThumbnailPixels& decoded, float& aspect) {
    if (sourcePixels.empty() || !sourceWidth || !sourceHeight || !targetHeight) return E_INVALIDARG;
    const float naturalAspect = static_cast<float>(sourceWidth) / sourceHeight;
    const float croppedAspect = std::clamp(naturalAspect, 2.0f / 3.0f, 16.0f / 9.0f);
    UINT cropWidth = sourceWidth;
    UINT cropHeight = sourceHeight;
    if (naturalAspect > croppedAspect) cropWidth = std::max(1u, static_cast<UINT>(std::lround(sourceHeight * croppedAspect)));
    else if (naturalAspect < croppedAspect) cropHeight = std::max(1u, static_cast<UINT>(std::lround(sourceWidth / croppedAspect)));
    const WICRect crop{ static_cast<INT>((sourceWidth - cropWidth) / 2), static_cast<INT>((sourceHeight - cropHeight) / 2),
        static_cast<INT>(cropWidth), static_cast<INT>(cropHeight) };
    const UINT targetWidth = std::max(1u, static_cast<UINT>(std::lround(targetHeight * croppedAspect)));
    if (targetWidth > UINT_MAX / 4 || targetHeight > UINT_MAX / (targetWidth * 4)) return E_OUTOFMEMORY;

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICBitmapClipper> clipper;
    ComPtr<IWICBitmapScaler> scaler;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateBitmapFromMemory(sourceWidth, sourceHeight, GUID_WICPixelFormat32bppPBGRA,
        sourceWidth * 4, static_cast<UINT>(sourcePixels.size()), sourcePixels.data(), &bitmap);
    if (SUCCEEDED(hr)) hr = factory->CreateBitmapClipper(&clipper);
    if (SUCCEEDED(hr)) hr = clipper->Initialize(bitmap.Get(), &crop);
    if (SUCCEEDED(hr)) hr = factory->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(hr)) hr = scaler->Initialize(clipper.Get(), targetWidth, targetHeight, WICBitmapInterpolationModeFant);
    if (FAILED(hr)) return hr;

    const UINT stride = targetWidth * 4;
    auto pixels = std::make_shared<std::vector<BYTE>>(static_cast<size_t>(stride) * targetHeight);
    hr = scaler->CopyPixels(nullptr, stride, static_cast<UINT>(pixels->size()), pixels->data());
    if (FAILED(hr)) return hr;
    decoded.width = targetWidth;
    decoded.height = targetHeight;
    decoded.stride = stride;
    decoded.pixels = std::move(pixels);
    aspect = static_cast<float>(targetWidth) / targetHeight;
    return S_OK;
}

} // namespace

HRESULT DecodeVideoThumbnailPixels(const std::wstring& path, UINT targetHeight, VideoThumbnailPixels& decoded, float& aspect) {
    decoded = {};
    aspect = 1.0f;
    if (path.empty() || !targetHeight) return E_INVALIDARG;

    const HRESULT startup = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(startup)) return startup;

    HRESULT hr = E_FAIL;
    std::vector<BYTE> sourcePixels;
    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    {
        ComPtr<IMFAttributes> attributes;
        ComPtr<IMFSourceReader> reader;
        ComPtr<IMFMediaType> outputType;
        hr = MFCreateAttributes(&attributes, 1);
        if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader);

        DWORD videoStream = MF_SOURCE_READER_FIRST_VIDEO_STREAM;
        if (SUCCEEDED(hr)) {
            for (DWORD stream = 0;; ++stream) {
                ComPtr<IMFMediaType> nativeType;
                const HRESULT native = reader->GetNativeMediaType(stream, 0, &nativeType);
                if (native == MF_E_INVALIDSTREAMNUMBER) { hr = MF_E_INVALIDMEDIATYPE; break; }
                if (FAILED(native)) continue;
                GUID majorType{};
                if (SUCCEEDED(nativeType->GetGUID(MF_MT_MAJOR_TYPE, &majorType)) && majorType == MFMediaType_Video) {
                    videoStream = stream;
                    break;
                }
            }
        }
        if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(videoStream, TRUE);
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&outputType);
        if (SUCCEEDED(hr)) hr = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(videoStream, nullptr, outputType.Get());
        if (SUCCEEDED(hr)) hr = reader->GetCurrentMediaType(videoStream, &outputType);

        PROPVARIANT duration{};
        PropVariantInit(&duration);
        if (SUCCEEDED(hr) && SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration)) &&
            duration.vt == VT_UI8 && duration.uhVal.QuadPart > 0) {
            const LONGLONG position = std::min<LONGLONG>(static_cast<LONGLONG>(duration.uhVal.QuadPart / 10), kHundredNanosecondsPerSecond);
            if (position > 0) {
                PROPVARIANT seek{};
                seek.vt = VT_I8;
                seek.hVal.QuadPart = position;
                reader->SetCurrentPosition(GUID_NULL, seek);
                PropVariantClear(&seek);
            }
        }
        PropVariantClear(&duration);

        for (UINT attempt = 0; SUCCEEDED(hr) && attempt < 64; ++attempt) {
            DWORD stream = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            hr = reader->ReadSample(videoStream, 0, &stream, &flags, &timestamp, &sample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
            if (sample) { hr = CopyVideoSamplePixels(sample.Get(), outputType.Get(), sourcePixels, sourceWidth, sourceHeight); break; }
        }
    }
    MFShutdown();
    if (FAILED(hr) || sourcePixels.empty()) return FAILED(hr) ? hr : MF_E_END_OF_STREAM;
    return ScaleVideoThumbnail(sourcePixels, sourceWidth, sourceHeight, targetHeight, decoded, aspect);
}
