#include "video_player.h"

#include <shlwapi.h>

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

bool VideoPlayer::Open(HWND window, ID3D11Device* device, const std::wstring& path, std::wstring& error) {
    Shutdown();
    if (!window || !device) { error = L"The video graphics device is unavailable."; return false; }
    if (FAILED(MFStartup(MF_VERSION))) { error = L"Windows Media Foundation could not initialize."; return false; }
    mediaFoundationStarted_ = true;
    window_ = window;
    if (!RebindDevice(device, error)) { Shutdown(); return false; }
    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFMediaEngineClassFactory> factory;
    ComPtr<IMFMediaEngineNotify> notify = new (std::nothrow) MediaEngineNotify(window_);
    HRESULT hr = notify ? MFCreateAttributes(&attributes, 4) : E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify.Get());
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, deviceManager_.Get());
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, AudioCategory_Media);
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateInstance(0, attributes.Get(), &engine_);
    if (SUCCEEDED(hr)) hr = engine_.As(&engineEx_);
    if (FAILED(hr) || !SetSourceFromPath(path, error)) {
        if (error.empty()) error = L"Windows could not prepare this MP4 for playback.";
        Shutdown();
        return false;
    }
    return true;
}

void VideoPlayer::Shutdown() {
    playing_ = ready_ = failed_ = false;
    frameBitmap_.Reset(); frameTexture_.Reset(); engineEx_.Reset();
    if (engine_) engine_->Shutdown();
    engine_.Reset(); deviceManager_.Reset(); device_.Reset();
    window_ = nullptr; videoWidth_ = videoHeight_ = 0;
    if (mediaFoundationStarted_) { MFShutdown(); mediaFoundationStarted_ = false; }
}

bool VideoPlayer::RebindDevice(ID3D11Device* device, std::wstring& error) {
    if (!device) { error = L"The video graphics device is unavailable."; return false; }
    if (!deviceManager_ && FAILED(MFCreateDXGIDeviceManager(&deviceResetToken_, &deviceManager_))) {
        error = L"Windows could not create the video device manager."; return false;
    }
    if (FAILED(deviceManager_->ResetDevice(device, deviceResetToken_))) {
        error = L"Windows could not bind the video decoder to the graphics device."; return false;
    }
    device_ = device; frameBitmap_.Reset(); frameTexture_.Reset();
    return !ready_ || CreateFrameTexture(error);
}

bool VideoPlayer::SetSourceFromPath(const std::wstring& path, std::wstring& error) {
    const std::wstring url = FileUrl(path);
    BSTR source = SysAllocString(url.c_str());
    if (!source) { error = L"Viewtrious could not prepare the video path."; return false; }
    const HRESULT set = engine_->SetSource(source);
    SysFreeString(source);
    if (FAILED(set) || FAILED(engine_->Load())) { error = L"Viewtrious could not open this MP4."; return false; }
    return true;
}

bool VideoPlayer::CreateFrameTexture(std::wstring& error) {
    if (!device_ || !videoWidth_ || !videoHeight_) return false;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = videoWidth_; description.Height = videoHeight_; description.MipLevels = 1; description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT; description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&description, nullptr, &frameTexture_))) {
        error = L"Viewtrious could not create the video frame surface."; return false;
    }
    return true;
}

bool VideoPlayer::HandleMediaEvent(DWORD event, std::wstring& error) {
    if (!engine_) return false;
    if (event == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA || event == MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY) {
        if (FAILED(engine_->GetNativeVideoSize(&videoWidth_, &videoHeight_)) || !videoWidth_ || !videoHeight_ || !CreateFrameTexture(error)) {
            if (error.empty()) error = L"Viewtrious could not read the MP4 video dimensions.";
            failed_ = true;
        } else ready_ = true;
    } else if (event == MF_MEDIA_ENGINE_EVENT_CANPLAY && !failed_) {
        if (FAILED(engine_->Play())) { error = L"Viewtrious could not start video playback."; failed_ = true; }
        else playing_ = true;
    } else if (event == MF_MEDIA_ENGINE_EVENT_ENDED) {
        playing_ = false;
    } else if (event == MF_MEDIA_ENGINE_EVENT_ERROR) {
        error = L"Viewtrious could not decode this MP4. It may be corrupt or use an unsupported codec.";
        failed_ = true; playing_ = false;
    }
    return true;
}

void VideoPlayer::TogglePlayPause() {
    if (!engine_ || failed_) return;
    if (playing_) { if (SUCCEEDED(engine_->Pause())) playing_ = false; }
    else if (SUCCEEDED(engine_->Play())) playing_ = true;
}

bool VideoPlayer::Draw(ID2D1DeviceContext* context, const RECT& canvas) {
    if (!context || !engineEx_ || !frameTexture_ || !videoWidth_ || !videoHeight_ || failed_) return false;
    const RECT source{ 0, 0, static_cast<LONG>(videoWidth_), static_cast<LONG>(videoHeight_) };
    const MFARGB border{};
    if (FAILED(engineEx_->TransferVideoFrame(frameTexture_.Get(), nullptr, &source, &border))) return false;
    if (!frameBitmap_) {
        ComPtr<IDXGISurface> surface;
        if (FAILED(frameTexture_.As(&surface))) return false;
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(context->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &frameBitmap_))) return false;
    }
    const float canvasWidth = static_cast<float>(std::max(1L, canvas.right - canvas.left));
    const float canvasHeight = static_cast<float>(std::max(1L, canvas.bottom - canvas.top));
    const float scale = std::min(canvasWidth / static_cast<float>(videoWidth_), canvasHeight / static_cast<float>(videoHeight_));
    const float width = videoWidth_ * scale, height = videoHeight_ * scale;
    const D2D1_RECT_F destination = D2D1::RectF(canvas.left + (canvasWidth - width) * 0.5f, canvas.top + (canvasHeight - height) * 0.5f,
        canvas.left + (canvasWidth + width) * 0.5f, canvas.top + (canvasHeight + height) * 0.5f);
    context->DrawBitmap(frameBitmap_.Get(), destination, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
    return true;
}
