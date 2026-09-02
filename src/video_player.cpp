#include "video_player.h"

#include <shlwapi.h>

#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {

void TraceVideo(HWND window, const wchar_t* stage, HRESULT result = S_OK, DWORD event = 0) {
    const DWORD uiThread = window ? GetWindowThreadProcessId(window, nullptr) : 0;
    const DWORD thread = GetCurrentThreadId();
    wchar_t line[512]{};
    swprintf_s(line, L"Viewtrious Video t=%llu tid=%lu %s stage=%s event=%lu hr=0x%08X\n",
        GetTickCount64(), thread, thread == uiThread ? L"UI" : L"MF/callback", stage, event, static_cast<unsigned>(result));
    OutputDebugStringW(line);
}

class MediaEngineNotify final : public IMFMediaEngineNotify {
public:
    explicit MediaEngineNotify(HWND window) : window_(window) {}
    STDMETHODIMP QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(IMFMediaEngineNotify)) {
            *result = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&references_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG references = InterlockedDecrement(&references_);
        if (!references) delete this;
        return references;
    }
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR, DWORD) override {
        TraceVideo(window_, L"MediaEngine event callback", S_OK, event);
        const BOOL posted = window_ ? PostMessageW(window_, WM_APP + 9, event, 0) : FALSE;
        TraceVideo(window_, L"MediaEngine event marshaled to UI", posted ? S_OK : HRESULT_FROM_WIN32(GetLastError()), event);
        return S_OK;
    }
private:
    LONG references_ = 1;
    HWND window_ = nullptr;
};

std::wstring FileUrl(const std::wstring& path) {
    DWORD length = 0;
    if (UrlCreateFromPathW(path.c_str(), nullptr, &length, 0) != E_POINTER || !length) return path;
    std::wstring result(length, L'\0');
    if (FAILED(UrlCreateFromPathW(path.c_str(), result.data(), &length, 0))) return path;
    result.resize(wcslen(result.c_str()));
    return result;
}

} // namespace

void VideoPlayer::Trace(HWND window, const wchar_t* stage, HRESULT result, DWORD event) { TraceVideo(window, stage, result, event); }

bool VideoPlayer::Open(HWND window, ID3D11Device* device, const std::wstring& path, std::wstring& error) {
    Trace(window, L"Video2D open entered");
    Shutdown();
    if (!window || !device) { error = L"The video graphics device is unavailable."; return false; }
    Trace(window, L"MFStartup begin");
    const HRESULT startup = MFStartup(MF_VERSION);
    Trace(window, L"MFStartup end", startup);
    if (FAILED(startup)) { error = L"Windows Media Foundation could not initialize."; return false; }
    mediaFoundationStarted_ = true;
    window_ = window;
    ReadNominalFrameRate(path);
    if (!RebindDevice(device, error)) { Shutdown(); return false; }
    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFMediaEngineClassFactory> factory;
    ComPtr<IMFMediaEngineNotify> notify = new (std::nothrow) MediaEngineNotify(window_);
    HRESULT hr = notify ? MFCreateAttributes(&attributes, 4) : E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify.Get());
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, deviceManager_.Get());
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, AudioCategory_Media);
    Trace(window_, L"MediaEngine class factory creation begin");
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    Trace(window_, L"MediaEngine class factory creation end", hr);
    Trace(window_, L"MediaEngine creation begin", hr);
    if (SUCCEEDED(hr)) hr = factory->CreateInstance(0, attributes.Get(), &engine_);
    Trace(window_, L"MediaEngine creation end", hr);
    if (SUCCEEDED(hr)) hr = engine_.As(&engineEx_);
    if (FAILED(hr) || !SetSourceFromPath(path, error)) {
        if (error.empty()) error = L"Windows could not prepare this MP4 for playback.";
        Shutdown();
        return false;
    }
    return true;
}

void VideoPlayer::Shutdown() {
    Trace(window_, L"Video2D shutdown/teardown begin");
    playing_ = ready_ = failed_ = hasValidFrame_ = hasTransferredPts_ = bitmapRebuildPending_ = cachedFrameDrawAfterResizePending_ = hasFramesPerSecond_ = false;
    lastTransferredPts_ = 0;
    framesPerSecond_ = 0.0f;
    frameBitmap_.Reset(); frameTexture_.Reset(); engineEx_.Reset();
    if (engine_) { const HRESULT shutdown = engine_->Shutdown(); Trace(window_, L"MediaEngine shutdown", shutdown); }
    engine_.Reset(); deviceManager_.Reset(); device_.Reset();
    videoWidth_ = videoHeight_ = 0;
    if (mediaFoundationStarted_) { MFShutdown(); mediaFoundationStarted_ = false; Trace(window_, L"MFShutdown end"); }
    Trace(window_, L"Video2D shutdown/teardown end");
    window_ = nullptr;
}

bool VideoPlayer::RebindDevice(ID3D11Device* device, std::wstring& error) {
    if (!device) { error = L"The video graphics device is unavailable."; return false; }
    if (!EnsureMultithreadProtection(device, error)) return false;
    Trace(window_, L"DXGI device-manager creation begin");
    const HRESULT createManager = !deviceManager_ ? MFCreateDXGIDeviceManager(&deviceResetToken_, &deviceManager_) : S_OK;
    Trace(window_, L"DXGI device-manager creation end", createManager);
    if (FAILED(createManager)) {
        error = L"Windows could not create the video device manager."; return false;
    }
    Trace(window_, L"DXGI device-manager reset begin");
    const HRESULT reset = deviceManager_->ResetDevice(device, deviceResetToken_);
    Trace(window_, L"DXGI device-manager reset end", reset);
    if (FAILED(reset)) {
        error = L"Windows could not bind the video decoder to the graphics device."; return false;
    }
    device_ = device; frameBitmap_.Reset(); frameTexture_.Reset(); hasValidFrame_ = hasTransferredPts_ = cachedFrameDrawAfterResizePending_ = false; lastTransferredPts_ = 0;
    return !ready_ || CreateFrameTexture(error);
}

void VideoPlayer::HandleRenderTargetResize() {
    Trace(window_, L"Video2D resize begin");
    if (frameTexture_) Trace(window_, L"video texture preserved");
    if (frameBitmap_) {
        frameBitmap_.Reset();
        bitmapRebuildPending_ = true;
        Trace(window_, L"video D2D bitmap released for target rebuild");
    }
    if (hasValidFrame_) { cachedFrameDrawAfterResizePending_ = true; Trace(window_, L"cached frame preserved across resize"); }
    Trace(window_, L"Video2D resize end");
}

bool VideoPlayer::EnsureMultithreadProtection(ID3D11Device* device, std::wstring& error) {
    const UINT creationFlags = device->GetCreationFlags();
    wchar_t flagsStage[128]{};
    swprintf_s(flagsStage, L"D3D11 creation flags=0x%08X", creationFlags);
    Trace(window_, flagsStage);
    if (creationFlags & D3D11_CREATE_DEVICE_SINGLETHREADED) {
        Trace(window_, L"D3D11_CREATE_DEVICE_SINGLETHREADED unexpectedly present", E_FAIL);
        error = L"The shared graphics device does not support safe video threading.";
        return false;
    }
    Trace(window_, L"ID3D10Multithread query begin");
    ComPtr<ID3D10Multithread> multithread;
    const HRESULT query = device->QueryInterface(IID_PPV_ARGS(&multithread));
    Trace(window_, L"ID3D10Multithread query end", query);
    if (FAILED(query) || !multithread) {
        error = L"The shared graphics device does not expose multithread protection for video playback.";
        return false;
    }
    const BOOL wasProtected = multithread->GetMultithreadProtected();
    Trace(window_, wasProtected ? L"D3D multithread protection already enabled" : L"D3D multithread protection disabled");
    if (!wasProtected) {
        Trace(window_, L"D3D SetMultithreadProtected(TRUE) begin");
        multithread->SetMultithreadProtected(TRUE);
        Trace(window_, multithread->GetMultithreadProtected() ? L"D3D SetMultithreadProtected(TRUE) end: enabled" : L"D3D SetMultithreadProtected(TRUE) end: still disabled", multithread->GetMultithreadProtected() ? S_OK : E_FAIL);
        if (!multithread->GetMultithreadProtected()) {
            error = L"The shared graphics device could not enable multithread protection for video playback.";
            return false;
        }
    }
    return true;
}

bool VideoPlayer::SetSourceFromPath(const std::wstring& path, std::wstring& error) {
    const std::wstring url = FileUrl(path);
    BSTR source = SysAllocString(url.c_str());
    if (!source) { error = L"Viewtrious could not prepare the video path."; return false; }
    const HRESULT set = engine_->SetSource(source);
    Trace(window_, L"MediaEngine SetSource", set);
    SysFreeString(source);
    const HRESULT load = SUCCEEDED(set) ? engine_->Load() : set;
    Trace(window_, L"MediaEngine Load", load);
    if (FAILED(set) || FAILED(load)) { error = L"Viewtrious could not open this MP4."; return false; }
    return true;
}

bool VideoPlayer::CreateFrameTexture(std::wstring& error) {
    if (!device_ || !videoWidth_ || !videoHeight_) return false;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = videoWidth_; description.Height = videoHeight_; description.MipLevels = 1; description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT; description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    const HRESULT createTexture = device_->CreateTexture2D(&description, nullptr, &frameTexture_);
    Trace(window_, L"video frame texture creation", createTexture);
    if (FAILED(createTexture)) {
        error = L"Viewtrious could not create the video frame surface."; return false;
    }
    return true;
}

bool VideoPlayer::HandleMediaEvent(DWORD event, std::wstring& error) {
    Trace(window_, L"MediaEngine event handled on UI", S_OK, event);
    if (!engine_) return false;
    if (event == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA || event == MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY) {
        Trace(window_, event == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA ? L"loaded metadata" : L"first frame ready", S_OK, event);
        const HRESULT size = engine_->GetNativeVideoSize(&videoWidth_, &videoHeight_);
        Trace(window_, L"GetNativeVideoSize", size, event);
        if (FAILED(size) || !videoWidth_ || !videoHeight_ || !CreateFrameTexture(error)) {
            if (error.empty()) error = L"Viewtrious could not read the MP4 video dimensions.";
            failed_ = true;
        } else ready_ = true;
    } else if (event == MF_MEDIA_ENGINE_EVENT_CANPLAY && !failed_) {
        Trace(window_, L"can-play/play request", S_OK, event);
        const HRESULT play = engine_->Play();
        Trace(window_, L"play request end", play, event);
        if (FAILED(play)) { error = L"Viewtrious could not start video playback."; failed_ = true; }
        else playing_ = true;
    } else if (event == MF_MEDIA_ENGINE_EVENT_PLAYING) {
        Trace(window_, L"playing", S_OK, event);
        playing_ = true;
    } else if (event == MF_MEDIA_ENGINE_EVENT_ENDED) {
        Trace(window_, L"ended", S_OK, event);
        playing_ = false;
    } else if (event == MF_MEDIA_ENGINE_EVENT_ERROR) {
        Trace(window_, L"error", S_OK, event);
        error = L"Viewtrious could not decode this MP4. It may be corrupt or use an unsupported codec.";
        failed_ = true; playing_ = false;
    }
    return true;
}

void VideoPlayer::TogglePlayPause() {
    if (!engine_ || failed_) return;
    Trace(window_, L"play/pause request");
    if (playing_) { const HRESULT pause = engine_->Pause(); Trace(window_, L"pause request end", pause); if (SUCCEEDED(pause)) playing_ = false; }
    else { const HRESULT play = engine_->Play(); Trace(window_, L"play request end", play); if (SUCCEEDED(play)) playing_ = true; }
}

bool VideoPlayer::GetNativeVideoSize(DWORD& width, DWORD& height) const {
    width = videoWidth_; height = videoHeight_;
    return width != 0 && height != 0;
}

bool VideoPlayer::ReadNominalFrameRate(const std::wstring& path) {
    ComPtr<IMFSourceResolver> resolver;
    ComPtr<IUnknown> sourceObject;
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFPresentationDescriptor> presentation;
    MF_OBJECT_TYPE objectType = MF_OBJECT_INVALID;
    HRESULT result = MFCreateSourceResolver(&resolver);
    if (SUCCEEDED(result)) {
        const std::wstring url = FileUrl(path);
        result = resolver->CreateObjectFromURL(url.c_str(), MF_RESOLUTION_MEDIASOURCE, nullptr, &objectType, &sourceObject);
    }
    if (SUCCEEDED(result) && objectType != MF_OBJECT_MEDIASOURCE) result = E_NOINTERFACE;
    if (SUCCEEDED(result)) result = sourceObject.As(&source);
    if (SUCCEEDED(result)) result = source->CreatePresentationDescriptor(&presentation);

    DWORD streamCount = 0;
    if (SUCCEEDED(result)) result = presentation->GetStreamDescriptorCount(&streamCount);
    for (DWORD index = 0; SUCCEEDED(result) && index < streamCount; ++index) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> stream;
        result = presentation->GetStreamDescriptorByIndex(index, &selected, &stream);
        if (FAILED(result) || !selected) continue;

        ComPtr<IMFMediaTypeHandler> handler;
        ComPtr<IMFMediaType> mediaType;
        GUID majorType{};
        if (FAILED(stream->GetMediaTypeHandler(&handler)) || FAILED(handler->GetCurrentMediaType(&mediaType)) ||
            FAILED(mediaType->GetGUID(MF_MT_MAJOR_TYPE, &majorType)) || majorType != MFMediaType_Video) continue;

        UINT32 numerator = 0, denominator = 0;
        if (SUCCEEDED(MFGetAttributeRatio(mediaType.Get(), MF_MT_FRAME_RATE, &numerator, &denominator)) && numerator && denominator) {
            framesPerSecond_ = static_cast<float>(numerator) / static_cast<float>(denominator);
            hasFramesPerSecond_ = std::isfinite(framesPerSecond_) && framesPerSecond_ > 0.0f;
            break;
        }
    }
    if (source) source->Shutdown();
    Trace(window_, hasFramesPerSecond_ ? L"nominal video frame rate read" : L"nominal video frame rate unavailable", result);
    return hasFramesPerSecond_;
}

bool VideoPlayer::TryGetFramesPerSecond(float& framesPerSecond) {
    if (!hasFramesPerSecond_) return false;
    framesPerSecond = framesPerSecond_;
    return true;
}

bool VideoPlayer::Draw(ID2D1DeviceContext* context, const RECT& canvas) {
    // Previously, "ready" meant only that the texture and dimensions existed. That allowed
    // every timer-driven paint to call TransferVideoFrame, including repeated stale frames.
    const bool frameReady = context && engine_ && engineEx_ && frameTexture_ && videoWidth_ && videoHeight_ && !failed_;
    Trace(window_, frameReady ? L"frame-ready decision: resources ready; checking stream tick" : L"frame-ready decision: resources not ready");
    if (!frameReady) return false;
    LONGLONG pts = 0;
    Trace(window_, L"OnVideoStreamTick begin");
    const HRESULT tick = engine_->OnVideoStreamTick(&pts);
    wchar_t tickStage[160]{};
    swprintf_s(tickStage, L"OnVideoStreamTick end pts=%lld", pts);
    Trace(window_, tickStage, tick);
    const RECT source{ 0, 0, static_cast<LONG>(videoWidth_), static_cast<LONG>(videoHeight_) };
    const MFARGB border{};
    if (tick == S_OK && !(hasTransferredPts_ && pts == lastTransferredPts_)) {
        wchar_t transferStage[160]{};
        swprintf_s(transferStage, L"frame-ready decision: new PTS=%lld; transfer allowed", pts);
        Trace(window_, transferStage);
        Trace(window_, L"TransferVideoFrame begin");
        const HRESULT transfer = engineEx_->TransferVideoFrame(frameTexture_.Get(), nullptr, &source, &border);
        Trace(window_, L"TransferVideoFrame end", transfer);
        if (SUCCEEDED(transfer)) {
            hasValidFrame_ = hasTransferredPts_ = true;
            lastTransferredPts_ = pts;
            wchar_t transferredStage[160]{};
            swprintf_s(transferredStage, L"new frame transferred pts=%lld", pts);
            Trace(window_, transferredStage);
        } else Trace(window_, L"frame transfer failed; reusing previous frame", transfer);
    } else if (tick == S_FALSE) {
        Trace(window_, L"no new frame; retaining previous frame", tick);
    } else if (tick == S_OK) {
        Trace(window_, L"frame-ready decision: duplicate PTS; reusing previous frame");
    } else {
        Trace(window_, L"frame-ready decision: tick failed; reusing previous frame", tick);
    }
    if (!hasValidFrame_) { Trace(window_, L"no valid video frame yet"); return false; }
    wchar_t sourceStage[160]{};
    swprintf_s(sourceStage, L"video source rectangle calculated (%ld,%ld)-(%ld,%ld)", source.left, source.top, source.right, source.bottom);
    Trace(window_, sourceStage);
    const float canvasWidth = static_cast<float>(std::max(1L, canvas.right - canvas.left));
    const float canvasHeight = static_cast<float>(std::max(1L, canvas.bottom - canvas.top));
    const float scale = std::min(canvasWidth / static_cast<float>(videoWidth_), canvasHeight / static_cast<float>(videoHeight_));
    const float width = videoWidth_ * scale, height = videoHeight_ * scale;
    const D2D1_RECT_F destination = D2D1::RectF(canvas.left + (canvasWidth - width) * 0.5f, canvas.top + (canvasHeight - height) * 0.5f,
        canvas.left + (canvasWidth + width) * 0.5f, canvas.top + (canvasHeight + height) * 0.5f);
    wchar_t destinationStage[200]{};
    swprintf_s(destinationStage, L"video destination rectangle calculated (%.1f,%.1f)-(%.1f,%.1f)", destination.left, destination.top, destination.right, destination.bottom);
    Trace(window_, destinationStage);
    if (!frameBitmap_) {
        Trace(window_, L"IDXGISurface access begin");
        ComPtr<IDXGISurface> surface;
        const HRESULT surfaceResult = frameTexture_.As(&surface);
        Trace(window_, L"IDXGISurface access end", surfaceResult);
        if (FAILED(surfaceResult)) return false;
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        Trace(window_, bitmapRebuildPending_ ? L"video D2D bitmap recreated from cached texture begin" : L"D2D bitmap creation from video texture begin");
        const HRESULT bitmapResult = context->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &frameBitmap_);
        Trace(window_, bitmapRebuildPending_ ? L"video D2D bitmap recreated from cached texture end" : L"D2D bitmap creation from video texture end", bitmapResult);
        if (FAILED(bitmapResult)) return false;
        bitmapRebuildPending_ = false;
    } else Trace(window_, L"D2D bitmap reuse from video texture");
    if (cachedFrameDrawAfterResizePending_) { Trace(window_, L"drawing cached frame after resize"); cachedFrameDrawAfterResizePending_ = false; }
    Trace(window_, L"drawing cached video frame");
    Trace(window_, L"D2D DrawBitmap video composite begin");
    context->DrawBitmap(frameBitmap_.Get(), destination, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
    Trace(window_, L"D2D DrawBitmap video composite end");
    return true;
}
