#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <d3d10_1.h>
#include <AudioSessionTypes.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <wrl/client.h>

#include <string>

// Small Media Foundation wrapper which leaves final composition to GraphicsHost.
class VideoPlayer {
public:
    static void Trace(HWND window, const wchar_t* stage, HRESULT result = S_OK, DWORD event = 0);
    bool Open(HWND window, ID3D11Device* device, const std::wstring& path, std::wstring& error);
    void Shutdown();
    bool RebindDevice(ID3D11Device* device, std::wstring& error);
    void HandleRenderTargetResize();
    bool HandleMediaEvent(DWORD event, std::wstring& error);
    bool Draw(ID2D1DeviceContext* context, const RECT& canvas);
    void TogglePlayPause();
    bool GetNativeVideoSize(DWORD& width, DWORD& height) const;
    bool TryGetFramesPerSecond(float& framesPerSecond);
    bool Playing() const { return playing_; }
    bool Active() const { return engine_ != nullptr; }
    bool Failed() const { return failed_; }

private:
    bool CreateFrameTexture(std::wstring& error);
    bool ReadNominalFrameRate(const std::wstring& path);
    bool SetSourceFromPath(const std::wstring& path, std::wstring& error);
    bool EnsureMultithreadProtection(ID3D11Device* device, std::wstring& error);

    HWND window_ = nullptr;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> deviceManager_;
    Microsoft::WRL::ComPtr<IMFMediaEngine> engine_;
    Microsoft::WRL::ComPtr<IMFMediaEngineEx> engineEx_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> frameTexture_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> frameBitmap_;
    UINT deviceResetToken_ = 0;
    DWORD videoWidth_ = 0;
    DWORD videoHeight_ = 0;
    bool mediaFoundationStarted_ = false;
    bool ready_ = false;
    bool playing_ = false;
    bool failed_ = false;
    bool hasValidFrame_ = false;
    bool hasTransferredPts_ = false;
    bool bitmapRebuildPending_ = false;
    bool cachedFrameDrawAfterResizePending_ = false;
    bool hasFramesPerSecond_ = false;
    float framesPerSecond_ = 0.0f;
    LONGLONG lastTransferredPts_ = 0;
};
