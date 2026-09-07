#include "video_hover_frame_stream.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
using Microsoft::WRL::ComPtr;
struct VideoHoverFrameStream::Impl { ComPtr<IMFSourceReader> reader; ComPtr<IMFMediaType> type; bool mf = false; UINT max = 768; };
VideoHoverFrameStream::VideoHoverFrameStream() = default;
VideoHoverFrameStream::~VideoHoverFrameStream() { Close(); }
void VideoHoverFrameStream::Close() { if (!impl_) return; impl_->reader.Reset(); impl_->type.Reset(); if (impl_->mf) MFShutdown(); impl_.reset(); generation_ = nullptr; }
HRESULT VideoHoverFrameStream::Open(const VideoHoverPreviewRequest& r, const std::atomic<uint64_t>* g) {
    Close(); if (r.path.empty() || !g || g->load(std::memory_order_acquire) != r.generation) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    auto p = std::make_unique<Impl>(); p->max = std::clamp(r.maximumDimension, 64u, 1024u); HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE); if (FAILED(hr)) return hr; p->mf = true;
    ComPtr<IMFAttributes> a; if (SUCCEEDED(hr)) hr = MFCreateAttributes(&a, 1); if (SUCCEEDED(hr)) hr = a->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE); if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(r.path.c_str(), a.Get(), &p->reader);
    const DWORD s = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM); if (SUCCEEDED(hr)) hr = p->reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE); if (SUCCEEDED(hr)) hr = p->reader->SetStreamSelection(s, TRUE); if (SUCCEEDED(hr)) hr = MFCreateMediaType(&p->type); if (SUCCEEDED(hr)) hr = p->type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); if (SUCCEEDED(hr)) hr = p->type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32); if (SUCCEEDED(hr)) hr = p->reader->SetCurrentMediaType(s, nullptr, p->type.Get()); if (SUCCEEDED(hr)) hr = p->reader->GetCurrentMediaType(s, &p->type);
    PROPVARIANT d{}; PropVariantInit(&d); if (SUCCEEDED(hr) && SUCCEEDED(p->reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &d)) && d.vt == VT_UI8) durationSeconds_ = d.uhVal.QuadPart / 10000000.0; PropVariantClear(&d); startSeconds_ = durationSeconds_ > 4.0 ? std::min(durationSeconds_ * .1, 1.0) : 0.0;
    if (SUCCEEDED(hr) && startSeconds_ > 0) { PROPVARIANT v{}; v.vt = VT_I8; v.hVal.QuadPart = static_cast<LONGLONG>(startSeconds_ * 10000000.0); p->reader->SetCurrentPosition(GUID_NULL, v); }
    if (FAILED(hr)) { if (p->mf) MFShutdown(); return hr; } impl_ = std::move(p); generation_ = g; requestGeneration_ = r.generation; return S_OK;
}
HRESULT VideoHoverFrameStream::ReadNext(VideoHoverPreviewFrame& f) {
    f = {}; if (!impl_ || generation_->load(std::memory_order_acquire) != requestGeneration_) { Close(); return HRESULT_FROM_WIN32(ERROR_CANCELLED); }
    DWORD stream=0, flags=0; ComPtr<IMFSample> sample; HRESULT hr=impl_->reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,&stream,&flags,&f.timestamp,&sample); if (FAILED(hr) || (flags & static_cast<DWORD>(MF_SOURCE_READERF_ENDOFSTREAM)) || !sample) { Close(); return FAILED(hr)?hr:S_FALSE; }
    UINT w=0,h=0; if (FAILED(MFGetAttributeSize(impl_->type.Get(),MF_MT_FRAME_SIZE,&w,&h)) || !w || !h) return E_FAIL; float scale=std::min(1.f,static_cast<float>(impl_->max)/std::max(w,h)); f.width=std::max(1u,static_cast<UINT>(std::lround(w*scale))); f.height=std::max(1u,static_cast<UINT>(std::lround(h*scale))); f.stride=f.width*4;
    ComPtr<IMFMediaBuffer>b; hr=sample->ConvertToContiguousBuffer(&b); BYTE* src=nullptr; DWORD mx=0, len=0; if(SUCCEEDED(hr))hr=b->Lock(&src,&mx,&len);
    // MFVideoFormat_RGB32 is BGRX in memory: its fourth byte is padding, not alpha.
    UINT32 sourceStrideValue = w * 4; impl_->type->GetUINT32(MF_MT_DEFAULT_STRIDE, &sourceStrideValue); const INT32 sourceStride = static_cast<INT32>(sourceStrideValue); const size_t sourcePitch = sourceStride < 0 ? static_cast<size_t>(-static_cast<int64_t>(sourceStride)) : static_cast<size_t>(sourceStride);
    if(FAILED(hr) || sourcePitch < static_cast<size_t>(w) * 4 || len < sourcePitch * h){if(b)b->Unlock();return FAILED(hr)?hr:E_FAIL;}
    auto raw=std::make_shared<std::vector<BYTE>>(static_cast<size_t>(w)*h*4); const BYTE* row = src; if (sourceStride < 0) row += sourcePitch * (h - 1);
    for (UINT y = 0; y < h; ++y) { BYTE* destination = raw->data() + static_cast<size_t>(y) * w * 4; std::memcpy(destination, row, static_cast<size_t>(w) * 4); for (UINT x = 0; x < w; ++x) destination[x * 4 + 3] = 255; row += sourceStride; }
    b->Unlock();
#ifdef _DEBUG
    wchar_t trace[192]{}; swprintf_s(trace, L"[Viewtrious] VIDEO_HOVER_MF_DECODE timestamp=%lld format=BGRX-to-PBGRA stride=%d alpha=%u\\n", f.timestamp, sourceStride, (*raw)[3]); OutputDebugStringW(trace);
#endif
    ComPtr<IWICImagingFactory> wf; ComPtr<IWICBitmap> wb; ComPtr<IWICBitmapScaler> sc; hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wf)); if(SUCCEEDED(hr))hr=wf->CreateBitmapFromMemory(w,h,GUID_WICPixelFormat32bppPBGRA,w*4,static_cast<UINT>(raw->size()),raw->data(),&wb); if(SUCCEEDED(hr))hr=wf->CreateBitmapScaler(&sc); if(SUCCEEDED(hr))hr=sc->Initialize(wb.Get(),f.width,f.height,WICBitmapInterpolationModeFant); f.pixels=std::make_shared<std::vector<BYTE>>(static_cast<size_t>(f.stride)*f.height); if(SUCCEEDED(hr))hr=sc->CopyPixels(nullptr,f.stride,static_cast<UINT>(f.pixels->size()),f.pixels->data()); if(SUCCEEDED(hr))for(size_t pixel=3;pixel<f.pixels->size();pixel+=4)(*f.pixels)[pixel]=255; if(FAILED(hr))f={}; return hr;
}
