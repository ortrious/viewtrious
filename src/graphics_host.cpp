#include "graphics_host.h"

#include <dxgi1_6.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace {
enum class PresentationMode { HwndSwapChain, CompositionSwapChain, WindowsUiComposition };
constexpr PresentationMode kPresentationMode = PresentationMode::WindowsUiComposition;
enum class CompositionResizeMode { ExactCompositionResize, RetainedCompositionCapacity };
constexpr CompositionResizeMode kCompositionResizeMode = CompositionResizeMode::RetainedCompositionCapacity;
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

bool GraphicsHost::CreateWindowsUiCompositionTree(std::wstring& error) {
    try {
        if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
            DispatcherQueueOptions options{};
            options.dwSize = sizeof(options);
            options.threadType = DQTYPE_THREAD_CURRENT;
            options.apartmentType = DQTAT_COM_STA;
            winrt::check_hresult(CreateDispatcherQueueController(options, reinterpret_cast<PDISPATCHERQUEUECONTROLLER*>(winrt::put_abi(dispatcherQueueController_))));
        }
        uiCompositor_ = winrt::Windows::UI::Composition::Compositor();
        const auto desktopInterop = uiCompositor_.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        winrt::check_hresult(desktopInterop->CreateDesktopWindowTarget(window_, FALSE, reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(uiCompositionTarget_))));
        uiRootVisual_ = uiCompositor_.CreateContainerVisual();
        uiSurfaceVisual_ = uiCompositor_.CreateSpriteVisual();
        uiSurfaceBrush_ = uiCompositor_.CreateSurfaceBrush();
        uiSurfaceBrush_.Stretch(winrt::Windows::UI::Composition::CompositionStretch::None);
        uiSurfaceBrush_.HorizontalAlignmentRatio(0.0f);
        uiSurfaceBrush_.VerticalAlignmentRatio(0.0f);
        uiSurfaceVisual_.Brush(uiSurfaceBrush_);
        uiClip_ = uiCompositor_.as<winrt::Windows::UI::Composition::ICompositor7>().CreateRectangleClip();
        uiRootVisual_.Clip(uiClip_);
        uiRootVisual_.Children().InsertAtTop(uiSurfaceVisual_);
        uiCompositionTarget_.Root(uiRootVisual_);
        return RebindWindowsUiCompositionSurface(error);
    } catch (const winrt::hresult_error&) {
        error = L"The Windows UI Composition display surface could not initialize.";
        DestroyWindowsUiCompositionTree();
        return false;
    }
}

bool GraphicsHost::RebindWindowsUiCompositionSurface(std::wstring& error) {
    if (!uiCompositor_) return true;
    if (!uiSurfaceBrush_ || !swapChain_) return false;
    try {
        uiCompositionSurface_ = nullptr;
        const auto compositorInterop = uiCompositor_.as<ABI::Windows::UI::Composition::ICompositorInterop>();
        winrt::check_hresult(compositorInterop->CreateCompositionSurfaceForSwapChain(swapChain_.Get(), reinterpret_cast<ABI::Windows::UI::Composition::ICompositionSurface**>(winrt::put_abi(uiCompositionSurface_))));
        uiSurfaceBrush_.Surface(uiCompositionSurface_);
        return true;
    } catch (const winrt::hresult_error&) {
        error = L"The Windows UI Composition surface could not bind the swap chain.";
        return false;
    }
}

void GraphicsHost::DestroyWindowsUiCompositionTree() {
    uiCompositionSurface_ = nullptr;
    uiClip_ = nullptr;
    uiSurfaceBrush_ = nullptr;
    uiSurfaceVisual_ = nullptr;
    uiRootVisual_ = nullptr;
    uiCompositionTarget_ = nullptr;
    uiCompositor_ = nullptr;
    if (dispatcherQueueController_) {
        try { dispatcherQueueController_.ShutdownQueueAsync(); } catch (const winrt::hresult_error&) {}
        dispatcherQueueController_ = nullptr;
    }
}

bool GraphicsHost::Create(HWND window, ID2D1Factory1* factory, const LUID* preferredAdapter, std::wstring& error) {
    Destroy(); window_ = window;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
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
    const auto createCompositionSwapChain = [&] {
        DXGI_SWAP_CHAIN_DESC1 compositionDesc = desc;
        compositionDesc.Scaling = DXGI_SCALING_STRETCH;
        compositionDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (FAILED(factory2->CreateSwapChainForComposition(device_.Get(), &compositionDesc, nullptr, &swapChain_)) || FAILED(DCompositionCreateDevice2(dxgiDevice.Get(), IID_PPV_ARGS(&dcompDevice_))) || FAILED(dcompDevice_->CreateTargetForHwnd(window_, TRUE, &dcompTarget_)) || FAILED(dcompDevice_->CreateVisual(&dcompVisual_)) || FAILED(dcompDevice_->CreateRectangleClip(&dcompClip_)) || FAILED(dcompVisual_->SetClip(dcompClip_.Get())) || FAILED(dcompVisual_->SetContent(swapChain_.Get())) || FAILED(dcompTarget_->SetRoot(dcompVisual_.Get())) || FAILED(dcompDevice_->Commit())) {
            dcompClip_.Reset(); dcompVisual_.Reset(); dcompTarget_.Reset(); dcompDevice_.Reset(); swapChain_.Reset(); return false;
        }
        return true;
    };
    const auto createWindowsUiCompositionSwapChain = [&] {
        DXGI_SWAP_CHAIN_DESC1 compositionDesc = desc;
        compositionDesc.Scaling = DXGI_SCALING_STRETCH;
        compositionDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (FAILED(factory2->CreateSwapChainForComposition(device_.Get(), &compositionDesc, nullptr, &swapChain_)) || !CreateWindowsUiCompositionTree(error)) {
            DestroyWindowsUiCompositionTree(); swapChain_.Reset(); return false;
        }
        return true;
    };
    const bool swapChainCreated = kPresentationMode == PresentationMode::WindowsUiComposition
        ? (createWindowsUiCompositionSwapChain() || createCompositionSwapChain() || SUCCEEDED(factory2->CreateSwapChainForHwnd(device_.Get(), window_, &desc, nullptr, nullptr, &swapChain_)))
        : kPresentationMode == PresentationMode::CompositionSwapChain
            ? (createCompositionSwapChain() || SUCCEEDED(factory2->CreateSwapChainForHwnd(device_.Get(), window_, &desc, nullptr, nullptr, &swapChain_)))
            : SUCCEEDED(factory2->CreateSwapChainForHwnd(device_.Get(), window_, &desc, nullptr, nullptr, &swapChain_));
    if (!swapChainCreated || FAILED(factory->CreateDevice(dxgiDevice.Get(), &d2dDevice_)) || FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_))) { error = L"The graphics display surface could not initialize."; Destroy(); return false; }
    RECT client{}; GetClientRect(window_, &client);
    return Resize(std::max(1L, client.right - client.left), std::max(1L, client.bottom - client.top), static_cast<float>(GetDpiForWindow(window_)), error);
}

void GraphicsHost::DiscardModelTargets() { modelRenderTarget_.Reset(); modelBackBuffer_.Reset(); }
void GraphicsHost::DiscardTargets() { if (context_) context_->OMSetRenderTargets(0, nullptr, nullptr); if (d2dContext_) d2dContext_->SetTarget(nullptr); d2dTarget_.Reset(); DiscardModelTargets(); renderTarget_.Reset(); backBuffer_.Reset(); }
void GraphicsHost::Destroy() { DiscardTargets(); d2dContext_.Reset(); d2dDevice_.Reset(); dcompClip_.Reset(); dcompVisual_.Reset(); dcompTarget_.Reset(); dcompDevice_.Reset(); DestroyWindowsUiCompositionTree(); swapChain_.Reset(); context_.Reset(); device_.Reset(); window_ = nullptr; clientWidth_ = clientHeight_ = capacityWidth_ = capacityHeight_ = 0; activeAdapterName_.clear(); }
bool GraphicsHost::CreateTargets(float dpi, std::wstring& error) {
    ComPtr<IDXGISurface> surface;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer_))) || FAILED(device_->CreateRenderTargetView(backBuffer_.Get(), nullptr, &renderTarget_)) || FAILED(backBuffer_.As(&surface))) { error = L"The graphics back buffer could not initialize."; return false; }
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    if (FAILED(d2dContext_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &d2dTarget_))) { error = L"The Direct2D display surface could not initialize."; return false; }
    d2dContext_->SetTarget(d2dTarget_.Get()); return CreateModelTargets(error);
}
bool GraphicsHost::CreateModelTargets(std::wstring& error) {
    DiscardModelTargets();
    if (!((dcompClip_ || uiClip_) && kCompositionResizeMode == CompositionResizeMode::RetainedCompositionCapacity)) return true;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = clientWidth_; description.Height = clientHeight_; description.MipLevels = 1; description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; description.SampleDesc.Count = 1; description.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&description, nullptr, &modelBackBuffer_)) || FAILED(device_->CreateRenderTargetView(modelBackBuffer_.Get(), nullptr, &modelRenderTarget_))) { error = L"The model display surface could not initialize."; DiscardModelTargets(); return false; }
    return true;
}
SIZE GraphicsHost::CompositionCapacity(UINT minimumWidth, UINT minimumHeight) const {
    MONITORINFO monitor{ sizeof(monitor) };
    if (window_ && GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const UINT width = std::max(minimumWidth, static_cast<UINT>(monitor.rcMonitor.right - monitor.rcMonitor.left));
        const UINT height = std::max(minimumHeight, static_cast<UINT>(monitor.rcMonitor.bottom - monitor.rcMonitor.top));
        return { static_cast<LONG>(width), static_cast<LONG>(height) };
    }
    return { static_cast<LONG>(minimumWidth), static_cast<LONG>(minimumHeight) };
}
bool GraphicsHost::UpdateCompositionClip(UINT width, UINT height, std::wstring& error) {
    if (uiClip_) {
        try {
            uiRootVisual_.Size({ static_cast<float>(width), static_cast<float>(height) });
            uiSurfaceVisual_.Size({ static_cast<float>(width), static_cast<float>(height) });
            uiClip_.Left(0.0f); uiClip_.Top(0.0f); uiClip_.Right(static_cast<float>(width)); uiClip_.Bottom(static_cast<float>(height));
            return true;
        } catch (const winrt::hresult_error&) { error = L"The Windows UI Composition display surface could not update."; return false; }
    }
    if (!dcompClip_) return true;
    if (FAILED(dcompClip_->SetLeft(0.0f)) || FAILED(dcompClip_->SetTop(0.0f)) || FAILED(dcompClip_->SetRight(static_cast<float>(width))) || FAILED(dcompClip_->SetBottom(static_cast<float>(height))) || FAILED(dcompDevice_->Commit())) { error = L"The composition display surface could not update."; return false; }
    return true;
}
bool GraphicsHost::Resize(UINT width, UINT height, float dpi, std::wstring& error) {
    if (!swapChain_ || !width || !height) return false;
    clientWidth_ = width; clientHeight_ = height;
    const bool retainedComposition = (dcompClip_ || uiClip_) && kCompositionResizeMode == CompositionResizeMode::RetainedCompositionCapacity;
    if (retainedComposition && width <= capacityWidth_ && height <= capacityHeight_) return CreateModelTargets(error) && UpdateCompositionClip(width, height, error);
    const SIZE requestedCapacity = retainedComposition ? CompositionCapacity(width, height) : SIZE{ static_cast<LONG>(width), static_cast<LONG>(height) };
    DiscardTargets();
    if (uiCompositionSurface_) { uiSurfaceBrush_.Surface(nullptr); uiCompositionSurface_ = nullptr; }
    if (FAILED(swapChain_->ResizeBuffers(0, static_cast<UINT>(requestedCapacity.cx), static_cast<UINT>(requestedCapacity.cy), DXGI_FORMAT_UNKNOWN, 0))) {
        if (!retainedComposition || (requestedCapacity.cx == static_cast<LONG>(width) && requestedCapacity.cy == static_cast<LONG>(height)) || FAILED(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) { error = L"The graphics surface could not resize."; return false; }
        capacityWidth_ = width; capacityHeight_ = height;
    } else { capacityWidth_ = static_cast<UINT>(requestedCapacity.cx); capacityHeight_ = static_cast<UINT>(requestedCapacity.cy); }
    return RebindWindowsUiCompositionSurface(error) && CreateTargets(dpi, error) && UpdateCompositionClip(width, height, error);
}
bool GraphicsHost::BeginDraw() { if (!Ready()) return false; if (modelBackBuffer_) { context_->OMSetRenderTargets(0, nullptr, nullptr); context_->CopySubresourceRegion(backBuffer_.Get(), 0, 0, 0, 0, modelBackBuffer_.Get(), 0, nullptr); } d2dContext_->BeginDraw(); return true; }
HRESULT GraphicsHost::EndDraw() { return d2dContext_ ? d2dContext_->EndDraw() : E_FAIL; }
HRESULT GraphicsHost::Present() { return swapChain_ ? swapChain_->Present(1, 0) : E_FAIL; }
