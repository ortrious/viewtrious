#include "graphics_host.h"

#include <dxgi1_6.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace {
bool SameLuid(const LUID& left, const LUID& right) { return left.HighPart == right.HighPart && left.LowPart == right.LowPart; }
bool IsHardwareAdapter(IDXGIAdapter1* adapter) { DXGI_ADAPTER_DESC1 description{}; return adapter && SUCCEEDED(adapter->GetDesc1(&description)) && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE); }
ComPtr<IDXGIAdapter1> FindHardwareAdapter(const LUID* requested) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    const auto find = [&](bool highPerformance) {
        ComPtr<IDXGIFactory6> factory6;
        if (highPerformance && SUCCEEDED(factory.As(&factory6))) {
            for (UINT index = 0;; ++index) { ComPtr<IDXGIAdapter1> adapter; if (FAILED(factory6->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) break; DXGI_ADAPTER_DESC1 description{}; if (IsHardwareAdapter(adapter.Get()) && (!requested || (SUCCEEDED(adapter->GetDesc1(&description)) && SameLuid(description.AdapterLuid, *requested)))) return adapter; }
        } else {
            for (UINT index = 0;; ++index) { ComPtr<IDXGIAdapter1> adapter; if (FAILED(factory->EnumAdapters1(index, &adapter))) break; DXGI_ADAPTER_DESC1 description{}; if (IsHardwareAdapter(adapter.Get()) && (!requested || (SUCCEEDED(adapter->GetDesc1(&description)) && SameLuid(description.AdapterLuid, *requested)))) return adapter; }
        }
        return ComPtr<IDXGIAdapter1>{};
    };
    return requested ? find(false) : find(true);
}
}

std::vector<GraphicsAdapterInfo> GraphicsHost::EnumerateHardwareAdapters() {
    std::vector<GraphicsAdapterInfo> result;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;
    ComPtr<IDXGIFactory6> factory6;
    const bool highPerformance = SUCCEEDED(factory.As(&factory6));
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = highPerformance ? factory6->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) : factory->EnumAdapters1(index, &adapter);
        if (FAILED(hr)) break;
        DXGI_ADAPTER_DESC1 description{};
        if (!IsHardwareAdapter(adapter.Get()) || FAILED(adapter->GetDesc1(&description)) || std::any_of(result.begin(), result.end(), [&](const GraphicsAdapterInfo& item) { return SameLuid(item.luid, description.AdapterLuid); })) continue;
        result.push_back({ description.Description, description.AdapterLuid });
    }
    return result;
}

bool GraphicsHost::Create(HWND window, ID2D1Factory1* factory, const LUID* preferredAdapter, std::wstring& error) {
    Destroy(); window_ = window;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL level{};
    ComPtr<IDXGIAdapter1> adapter = FindHardwareAdapter(preferredAdapter);
    HRESULT hr = adapter ? D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, &level, &context_) : E_FAIL;
    if (FAILED(hr) && preferredAdapter) { adapter = FindHardwareAdapter(nullptr); if (adapter) hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, &level, &context_); }
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, &level, &context_);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, &level, &context_);
    if (FAILED(hr)) { error = L"Direct3D 11 could not initialize."; Destroy(); return false; }
    ComPtr<IDXGIDevice> dxgiDevice; ComPtr<IDXGIFactory2> factory2;
    ComPtr<IDXGIAdapter> activeAdapter;
    if (FAILED(device_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&activeAdapter)) || FAILED(activeAdapter->GetParent(IID_PPV_ARGS(&factory2)))) { error = L"The graphics adapter could not initialize."; Destroy(); return false; }
    DXGI_ADAPTER_DESC activeDescription{}; if (SUCCEEDED(activeAdapter->GetDesc(&activeDescription))) activeAdapterName_ = activeDescription.Description;
    DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width = 1; desc.Height = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    if (FAILED(factory2->CreateSwapChainForHwnd(device_.Get(), window_, &desc, nullptr, nullptr, &swapChain_)) || FAILED(factory->CreateDevice(dxgiDevice.Get(), &d2dDevice_)) || FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_))) { error = L"The graphics display surface could not initialize."; Destroy(); return false; }
    RECT client{}; GetClientRect(window_, &client);
    return Resize(std::max(1L, client.right - client.left), std::max(1L, client.bottom - client.top), static_cast<float>(GetDpiForWindow(window_)), error);
}

void GraphicsHost::DiscardTargets() { if (context_) context_->OMSetRenderTargets(0, nullptr, nullptr); if (d2dContext_) d2dContext_->SetTarget(nullptr); d2dTarget_.Reset(); renderTarget_.Reset(); backBuffer_.Reset(); }
void GraphicsHost::Destroy() { DiscardTargets(); d2dContext_.Reset(); d2dDevice_.Reset(); swapChain_.Reset(); context_.Reset(); device_.Reset(); window_ = nullptr; width_ = height_ = 0; activeAdapterName_.clear(); }
bool GraphicsHost::CreateTargets(float dpi, std::wstring& error) {
    ComPtr<IDXGISurface> surface;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer_))) || FAILED(device_->CreateRenderTargetView(backBuffer_.Get(), nullptr, &renderTarget_)) || FAILED(backBuffer_.As(&surface))) { error = L"The graphics back buffer could not initialize."; return false; }
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    if (FAILED(d2dContext_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &d2dTarget_))) { error = L"The Direct2D display surface could not initialize."; return false; }
    d2dContext_->SetTarget(d2dTarget_.Get()); return true;
}
bool GraphicsHost::Resize(UINT width, UINT height, float dpi, std::wstring& error) {
    if (!swapChain_ || !width || !height) return false;
    DiscardTargets(); if (FAILED(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) { error = L"The graphics surface could not resize."; return false; }
    width_ = width; height_ = height; return CreateTargets(dpi, error);
}
bool GraphicsHost::BeginDraw() { if (!Ready()) return false; d2dContext_->BeginDraw(); return true; }
HRESULT GraphicsHost::EndDraw() { return d2dContext_ ? d2dContext_->EndDraw() : E_FAIL; }
HRESULT GraphicsHost::Present() { return swapChain_ ? swapChain_->Present(1, 0) : E_FAIL; }
