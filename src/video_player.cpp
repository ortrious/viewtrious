#include "video_player.h"

#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

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
        if (window_) PostMessageW(window_, WM_APP + 9, event, 0);
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

void VideoPlayer::RecordFramePacingSchedule(double intervalMs, LONGLONG deadlineQpc) {
#if defined(_DEBUG)
    RecordFramePacingEvent(FramePacingEvent::Schedule, 0, S_OK, intervalMs);
    if (framePacingFrequency_ && framePacingRecordCount_) {
        FramePacingRecord& record = framePacingRecords_[(framePacingRecordStart_ + framePacingRecordCount_ - 1) % kFramePacingRecordCapacity];
        record.second = 1000.0 * static_cast<double>(deadlineQpc - framePacingRecords_[framePacingRecordStart_].qpc) / static_cast<double>(framePacingFrequency_);
    }
#else
    (void)intervalMs; (void)deadlineQpc;
#endif
}

void VideoPlayer::RecordFramePacingTimer(LONGLONG wakeQpc, LONGLONG deadlineQpc) {
#if defined(_DEBUG)
    const double latenessMs = deadlineQpc && framePacingFrequency_ ? 1000.0 * static_cast<double>(wakeQpc - deadlineQpc) / static_cast<double>(framePacingFrequency_) : 0.0;
    RecordFramePacingEventAtQpc(FramePacingEvent::Timer, wakeQpc, 0, S_OK, latenessMs);
#else
    (void)wakeQpc; (void)deadlineQpc;
#endif
}

void VideoPlayer::RecordFramePacingPaint() {
#if defined(_DEBUG)
    const LONGLONG pts = hasTransferredPts_ ? lastTransferredPts_ : -1;
    const bool changed = !framePacingHaveLastPaintPts_ || pts != framePacingLastPaintPts_;
    RecordFramePacingEvent(FramePacingEvent::Paint, pts, S_OK, changed ? 1.0 : 0.0);
    framePacingLastPaintPts_ = pts; framePacingHaveLastPaintPts_ = true;
#endif
}

void VideoPlayer::RecordFramePacingPresent(HRESULT result) {
#if defined(_DEBUG)
    const LONGLONG pts = hasTransferredPts_ ? lastTransferredPts_ : -1;
    const bool changed = !framePacingHaveLastPresentPts_ || pts != framePacingLastPresentPts_;
    RecordFramePacingEvent(FramePacingEvent::Present, pts, result, changed ? 1.0 : 0.0);
    framePacingLastPresentPts_ = pts; framePacingHaveLastPresentPts_ = true;
#else
    (void)result;
#endif
}

void VideoPlayer::FlushFramePacingDiagnostics() {
#if defined(_DEBUG)
    if (!framePacingRecordCount_ || !framePacingFrequency_) return;
    static const std::array<const wchar_t*, static_cast<size_t>(FramePacingEvent::Count)> names = { L"begin", L"pause", L"resume", L"seek", L"end", L"schedule", L"timer", L"scheduler-acquire", L"initial-acquire", L"seek-acquire", L"tick", L"transfer", L"cache", L"paint", L"present" };
    OutputDebugStringW(L"Viewtrious VIDEO PACING trace begin\n");
    std::array<LONGLONG, static_cast<size_t>(FramePacingEvent::Count)> previous{};
    for (size_t index = 0; index < framePacingRecordCount_; ++index) {
        const FramePacingRecord& record = framePacingRecords_[(framePacingRecordStart_ + index) % kFramePacingRecordCapacity];
        const size_t event = static_cast<size_t>(record.event);
        const double elapsedMs = 1000.0 * static_cast<double>(record.qpc - framePacingRecords_[framePacingRecordStart_].qpc) / static_cast<double>(framePacingFrequency_);
        const double deltaMs = previous[event] ? 1000.0 * static_cast<double>(record.qpc - previous[event]) / static_cast<double>(framePacingFrequency_) : 0.0;
        previous[event] = record.qpc;
        wchar_t line[320]{};
        swprintf_s(line, L"VFP t=%.3f dt=%.3f type=%s pts=%lld hr=0x%08X a=%.3f b=%.3f\n", elapsedMs, deltaMs, names[event], record.pts, static_cast<unsigned>(record.result), record.first, record.second);
        OutputDebugStringW(line);
    }
    OutputDebugStringW(L"Viewtrious VIDEO PACING trace end\n");
    framePacingRecordStart_ = framePacingRecordCount_ = 0;
#endif
}

#if defined(_DEBUG)
void VideoPlayer::ResetFramePacingDiagnostics() {
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    framePacingRecordStart_ = framePacingRecordCount_ = 0;
    framePacingFrequency_ = frequency.QuadPart;
    framePacingLastPaintPts_ = framePacingLastPresentPts_ = 0;
    framePacingHaveLastPaintPts_ = framePacingHaveLastPresentPts_ = false;
}

void VideoPlayer::RecordFramePacingEvent(FramePacingEvent event, LONGLONG pts, HRESULT result, double first, double second) {
    if (!framePacingFrequency_) ResetFramePacingDiagnostics();
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    RecordFramePacingEventAtQpc(event, now.QuadPart, pts, result, first, second);
}

void VideoPlayer::RecordFramePacingEventAtQpc(FramePacingEvent event, LONGLONG qpc, LONGLONG pts, HRESULT result, double first, double second) {
    if (!framePacingFrequency_) ResetFramePacingDiagnostics();
    const size_t index = (framePacingRecordStart_ + framePacingRecordCount_) % kFramePacingRecordCapacity;
    framePacingRecords_[index] = { qpc, pts, result, event, first, second };
    if (framePacingRecordCount_ < kFramePacingRecordCapacity) ++framePacingRecordCount_;
    else framePacingRecordStart_ = (framePacingRecordStart_ + 1) % kFramePacingRecordCapacity;
}
#else
void VideoPlayer::ResetFramePacingDiagnostics() {}
void VideoPlayer::RecordFramePacingEvent(FramePacingEvent, LONGLONG, HRESULT, double, double) {}
void VideoPlayer::RecordFramePacingEventAtQpc(FramePacingEvent, LONGLONG, LONGLONG, HRESULT, double, double) {}
#endif

bool VideoPlayer::Open(HWND window, ID3D11Device* device, const std::wstring& path, std::wstring& error) {
    Shutdown();
    ResetFramePacingDiagnostics();
    if (!window || !device) { error = L"The video graphics device is unavailable."; return false; }
    const HRESULT startup = MFStartup(MF_VERSION);
    if (FAILED(startup)) { error = L"Windows Media Foundation could not initialize."; return false; }
    mediaFoundationStarted_ = true;
    ReadNominalFrameRate(path);
    if (!RebindDevice(device, error)) { Shutdown(); return false; }
    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFMediaEngineClassFactory> factory;
    ComPtr<IMFMediaEngineNotify> notify = new (std::nothrow) MediaEngineNotify(window);
    HRESULT hr = notify ? MFCreateAttributes(&attributes, 4) : E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify.Get());
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, deviceManager_.Get());
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, AudioCategory_Media);
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateInstance(0, attributes.Get(), &engine_);
    if (SUCCEEDED(hr)) hr = engine_.As(&engineEx_);
    if (FAILED(hr) || !SetSourceFromPath(path, error)) {
        if (error.empty()) error = L"Windows could not prepare this video for playback.";
        Shutdown();
        return false;
    }
    return true;
}

void VideoPlayer::Shutdown() {
    FlushFramePacingDiagnostics();
    playing_ = ready_ = failed_ = hasValidFrame_ = adjustedFrameValid_ = hasTransferredPts_ = hasFramesPerSecond_ = false;
    lastTransferredPts_ = 0;
    framesPerSecond_ = 0.0f;
    adjustedFrameBitmap_.Reset(); frameBitmap_.Reset(); frameTexture_.Reset(); adjustmentProcessor_.Reset(); engineEx_.Reset();
    if (engine_) engine_->Shutdown();
    engine_.Reset(); deviceManager_.Reset(); device_.Reset();
    videoWidth_ = videoHeight_ = 0;
    if (mediaFoundationStarted_) { MFShutdown(); mediaFoundationStarted_ = false; }
}

bool VideoPlayer::RebindDevice(ID3D11Device* device, std::wstring& error) {
    if (!device) { error = L"The video graphics device is unavailable."; return false; }
    if (!EnsureMultithreadProtection(device, error)) return false;
    const HRESULT createManager = !deviceManager_ ? MFCreateDXGIDeviceManager(&deviceResetToken_, &deviceManager_) : S_OK;
    if (FAILED(createManager)) {
        error = L"Windows could not create the video device manager."; return false;
    }
    const HRESULT reset = deviceManager_->ResetDevice(device, deviceResetToken_);
    if (FAILED(reset)) {
        error = L"Windows could not bind the video decoder to the graphics device."; return false;
    }
    device_ = device; adjustedFrameBitmap_.Reset(); frameBitmap_.Reset(); frameTexture_.Reset(); adjustmentProcessor_.Initialize(device); hasValidFrame_ = adjustedFrameValid_ = hasTransferredPts_ = false; lastTransferredPts_ = 0;
    return !ready_ || CreateFrameTexture(error);
}

void VideoPlayer::HandleRenderTargetResize() {
    adjustedFrameBitmap_.Reset();
    if (frameBitmap_) frameBitmap_.Reset();
}

bool VideoPlayer::EnsureMultithreadProtection(ID3D11Device* device, std::wstring& error) {
    const UINT creationFlags = device->GetCreationFlags();
    if (creationFlags & D3D11_CREATE_DEVICE_SINGLETHREADED) {
        error = L"The shared graphics device does not support safe video threading.";
        return false;
    }
    ComPtr<ID3D10Multithread> multithread;
    const HRESULT query = device->QueryInterface(IID_PPV_ARGS(&multithread));
    if (FAILED(query) || !multithread) {
        error = L"The shared graphics device does not expose multithread protection for video playback.";
        return false;
    }
    const BOOL wasProtected = multithread->GetMultithreadProtected();
    if (!wasProtected) {
        multithread->SetMultithreadProtected(TRUE);
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
    SysFreeString(source);
    const HRESULT load = SUCCEEDED(set) ? engine_->Load() : set;
    if (FAILED(set) || FAILED(load)) { error = L"Viewtrious could not open this video."; return false; }
    return true;
}

bool VideoPlayer::CreateFrameTexture(std::wstring& error) {
    if (!device_ || !videoWidth_ || !videoHeight_) return false;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = videoWidth_; description.Height = videoHeight_; description.MipLevels = 1; description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT; description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    const HRESULT createTexture = device_->CreateTexture2D(&description, nullptr, &frameTexture_);
    if (FAILED(createTexture)) {
        error = L"Viewtrious could not create the video frame surface."; return false;
    }
    return true;
}

bool VideoPlayer::HandleMediaEvent(DWORD event, std::wstring& error) {
    if (!engine_) return false;
    if (event == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA || event == MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY) {
        const HRESULT size = engine_->GetNativeVideoSize(&videoWidth_, &videoHeight_);
        if (FAILED(size) || !videoWidth_ || !videoHeight_ || !CreateFrameTexture(error)) {
            if (error.empty()) error = L"Viewtrious could not read the video dimensions.";
            failed_ = true;
        } else ready_ = true;
    } else if (event == MF_MEDIA_ENGINE_EVENT_CANPLAY && !failed_) {
        const HRESULT play = engine_->Play();
        if (FAILED(play)) { error = L"Viewtrious could not start video playback."; failed_ = true; }
        else { playing_ = true; RecordFramePacingEvent(FramePacingEvent::PlaybackBegin); }
    } else if (event == MF_MEDIA_ENGINE_EVENT_PLAYING) {
        playing_ = true; RecordFramePacingEvent(FramePacingEvent::PlaybackResume);
    } else if (event == MF_MEDIA_ENGINE_EVENT_ENDED) {
        playing_ = false; RecordFramePacingEvent(FramePacingEvent::PlaybackEnd);
    } else if (event == MF_MEDIA_ENGINE_EVENT_ERROR) {
        error = L"Viewtrious could not decode this video. It may be corrupt or use an unsupported codec.";
        failed_ = true; playing_ = false;
    }
    return true;
}

HRESULT VideoPlayer::TogglePlayPause() {
    if (!engine_ || failed_) return E_FAIL;
    if (playing_) {
        const HRESULT pause = engine_->Pause();
        if (SUCCEEDED(pause)) { playing_ = false; RecordFramePacingEvent(FramePacingEvent::PlaybackPause); }
        return pause;
    }
    else {
        if (engine_->IsEnded()) {
            const HRESULT restart = engine_->SetCurrentTime(0.0);
            if (FAILED(restart)) return restart;
        }
        const HRESULT play = engine_->Play();
        if (SUCCEEDED(play)) { playing_ = true; RecordFramePacingEvent(FramePacingEvent::PlaybackResume); }
        return play;
    }
}

bool VideoPlayer::GetPlaybackTimes(double& currentSeconds, double& durationSeconds) const {
    currentSeconds = durationSeconds = 0.0;
    if (!engine_) return false;
    const double current = engine_->GetCurrentTime();
    const double duration = engine_->GetDuration();
    if (!std::isfinite(current) || !std::isfinite(duration) || current < 0.0 || duration <= 0.0) return false;
    currentSeconds = std::clamp(current, 0.0, duration);
    durationSeconds = duration;
    return true;
}

bool VideoPlayer::Seek(double seconds) {
    double current = 0.0, duration = 0.0;
    if (!GetPlaybackTimes(current, duration)) return false;
    RecordFramePacingEvent(FramePacingEvent::PlaybackSeek, 0, S_OK, seconds);
    const HRESULT result = engine_->SetCurrentTime(std::clamp(seconds, 0.0, duration));
    return SUCCEEDED(result);
}

bool VideoPlayer::ToggleMute() {
    if (!engine_) return false;
    const HRESULT result = engine_->SetMuted(engine_->GetMuted() ? FALSE : TRUE);
    return SUCCEEDED(result);
}

bool VideoPlayer::Muted() const { return engine_ && engine_->GetMuted() != FALSE; }

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
    return hasFramesPerSecond_;
}

bool VideoPlayer::TryGetFramesPerSecond(float& framesPerSecond) {
    if (!hasFramesPerSecond_) return false;
    framesPerSecond = framesPerSecond_;
    return true;
}

void VideoPlayer::SetDisplayAdjustments(const MediaAdjustments& adjustments) {
    displayAdjustments_ = adjustments;
    adjustedFrameValid_ = false;
    adjustedFrameBitmap_.Reset();
    if (hasValidFrame_ && !displayAdjustments_.IsNeutral()) adjustedFrameValid_ = adjustmentProcessor_.Process(frameTexture_.Get(), videoWidth_, videoHeight_, displayAdjustments_);
}

bool VideoPlayer::AutoDisplayAdjustments(MediaAdjustments& adjustments) {
    if (!hasValidFrame_ || !adjustmentProcessor_.Analyze(frameTexture_.Get(), adjustments)) return false;
    SetDisplayAdjustments(adjustments);
    return true;
}

bool VideoPlayer::UpdateFrame(FrameAcquisitionReason reason) {
    const bool frameReady = engine_ && engineEx_ && frameTexture_ && videoWidth_ && videoHeight_ && !failed_;
    const FramePacingEvent acquisition = reason == FrameAcquisitionReason::Scheduler ? FramePacingEvent::SchedulerAcquire :
        reason == FrameAcquisitionReason::InitialLoad ? FramePacingEvent::InitialLoadAcquire : FramePacingEvent::SeekAcquire;
    RecordFramePacingEvent(acquisition);
    if (!frameReady) return false;
    bool transferred = false;
    LONGLONG pts = 0;
    const HRESULT tick = engine_->OnVideoStreamTick(&pts);
    RecordFramePacingEvent(FramePacingEvent::StreamTick, pts, tick, tick == S_OK ? 1.0 : 0.0, hasTransferredPts_ && pts == lastTransferredPts_ ? 1.0 : 0.0);
    const RECT source{ 0, 0, static_cast<LONG>(videoWidth_), static_cast<LONG>(videoHeight_) };
    const MFARGB border{};
    if (tick == S_OK && !(hasTransferredPts_ && pts == lastTransferredPts_)) {
        const HRESULT transfer = engineEx_->TransferVideoFrame(frameTexture_.Get(), nullptr, &source, &border);
        if (SUCCEEDED(transfer)) {
            RecordFramePacingEvent(FramePacingEvent::Transfer, pts, transfer);
            hasValidFrame_ = hasTransferredPts_ = true;
            lastTransferredPts_ = pts;
            adjustedFrameBitmap_.Reset();
            adjustedFrameValid_ = !displayAdjustments_.IsNeutral() && adjustmentProcessor_.Process(frameTexture_.Get(), videoWidth_, videoHeight_, displayAdjustments_);
            RecordFramePacingEvent(FramePacingEvent::CachePublish, pts);
            transferred = true;
        }
    }
    return transferred;
}

bool VideoPlayer::Draw(ID2D1DeviceContext* context, const RECT& canvas, float scale, D2D1_POINT_2F pan) {
    if (!context || !frameTexture_ || !videoWidth_ || !videoHeight_ || !hasValidFrame_ || failed_) return false;
    const float canvasWidth = static_cast<float>(std::max(1L, canvas.right - canvas.left));
    const float canvasHeight = static_cast<float>(std::max(1L, canvas.bottom - canvas.top));
    const float width = videoWidth_ * scale, height = videoHeight_ * scale;
    const D2D1_RECT_F destination = D2D1::RectF(canvas.left + (canvasWidth - width) * 0.5f + pan.x, canvas.top + (canvasHeight - height) * 0.5f + pan.y,
        canvas.left + (canvasWidth + width) * 0.5f + pan.x, canvas.top + (canvasHeight + height) * 0.5f + pan.y);
    ID3D11Texture2D* displayTexture = frameTexture_.Get();
    ComPtr<ID2D1Bitmap1>* displayBitmap = &frameBitmap_;
    if (adjustedFrameValid_ && adjustmentProcessor_.OutputTexture()) {
        displayTexture = adjustmentProcessor_.OutputTexture();
        displayBitmap = &adjustedFrameBitmap_;
    }
    if (!*displayBitmap) {
        ComPtr<IDXGISurface> surface;
        const HRESULT surfaceResult = displayTexture->QueryInterface(IID_PPV_ARGS(&surface));
        if (FAILED(surfaceResult)) return false;
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        const HRESULT bitmapResult = context->CreateBitmapFromDxgiSurface(surface.Get(), &properties, displayBitmap->GetAddressOf());
        if (FAILED(bitmapResult)) return false;
    }
    context->DrawBitmap(displayBitmap->Get(), destination, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
    return true;
}
