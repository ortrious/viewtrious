#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>
#include <vector>

struct GraphicsAdapterInfo {
    std::wstring name;
    LUID luid{};
};

// Owns the single main-window DXGI back buffer and its Direct2D overlay target.
class GraphicsHost {
public:
    bool Create(HWND window, ID2D1Factory1* factory, const LUID* preferredAdapter, std::wstring& error);
    static std::vector<GraphicsAdapterInfo> EnumerateHardwareAdapters();
    void Destroy();
    bool Resize(UINT width, UINT height, float dpi, std::wstring& error);
    bool BeginDraw();
    HRESULT EndDraw();
    HRESULT Present();
    bool Ready() const { return d2dContext_ && renderTarget_; }
    ID2D1DeviceContext* D2DContext() const { return d2dContext_.Get(); }
    ID3D11Device* Device() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }
    ID3D11RenderTargetView* RenderTarget() const { return renderTarget_.Get(); }
    ID3D11RenderTargetView* const* RenderTargetAddress() const { return renderTarget_.GetAddressOf(); }
    ID3D11Texture2D* BackBuffer() const { return backBuffer_.Get(); }
    UINT Width() const { return width_; }
    UINT Height() const { return height_; }
    const std::wstring& ActiveAdapterName() const { return activeAdapterName_; }

private:
    bool CreateTargets(float dpi, std::wstring& error);
    void DiscardTargets();
    HWND window_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> dcompDevice_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcompTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> dcompVisual_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2dContext_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> d2dTarget_;
    UINT width_ = 0, height_ = 0;
    std::wstring activeAdapterName_;
};
