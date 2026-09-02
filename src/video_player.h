#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <wrl/client.h>

#include <string>

// Small Media Foundation wrapper which leaves final composition to GraphicsHost.
class VideoPlayer {
public:
    bool Open(HWND window, ID3D11Device* device, const std::wstring& path, std::wstring& error);
    void Shutdown();
    bool RebindDevice(ID3D11Device* device, std::wstring& error);
    bool HandleMediaEvent(DWORD event, std::wstring& error);
    bool Draw(ID2D1DeviceContext* context, const RECT& canvas);
    void TogglePlayPause();
    bool Playing() const { return playing_; }
    bool Active() const { return engine_ != nullptr; }
    bool Failed() const { return failed_; }

private:
    bool CreateFrameTexture(std::wstring& error);
    bool SetSourceFromPath(const std::wstring& path, std::wstring& error);

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
};
