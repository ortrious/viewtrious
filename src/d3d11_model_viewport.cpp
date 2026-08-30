#include "d3d11_model_viewport.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t kViewportClass[] = L"ViewtriousModelViewport";
struct Vertex { Float3 position; Float3 normal; };
struct Constants { Matrix4 viewProjection; Float3 eye; float pad = 0; };
const char kShader[] = R"(
cbuffer Constants : register(b0) { row_major float4x4 viewProjection; float3 eye; float padding; };
struct VSIn { float3 position : POSITION; float3 normal : NORMAL; };
struct VSOut { float4 position : SV_POSITION; float3 normal : NORMAL; float3 view : TEXCOORD0; };
VSOut VSMain(VSIn input) { VSOut output; output.position=mul(float4(input.position,1),viewProjection); output.normal=input.normal; output.view=normalize(eye-input.position); return output; }
float4 PSMain(VSOut input) : SV_TARGET { float3 n=normalize(input.normal); float3 l0=normalize(float3(0.45,0.75,0.55)); float3 l1=normalize(float3(-0.55,0.20,-0.65)); float light=0.22+0.55*abs(dot(n,l0))+0.23*abs(dot(n,l1)); return float4(float3(0.72,0.75,0.80)*light,1); }
)";

void TraceViewportFailure(const wchar_t* stage, HRESULT hr = S_OK, DWORD error = ERROR_SUCCESS) {
#if defined(_DEBUG)
    wchar_t message[256]{};
    swprintf_s(message, L"Viewtrious D3D viewport: %s hr=0x%08X win32=%lu\n", stage,
        static_cast<unsigned>(hr), static_cast<unsigned long>(error));
    OutputDebugStringW(message);
#else
    (void)stage; (void)hr; (void)error;
#endif
}
}

struct D3D11ModelViewport::Resources {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11RenderTargetView> renderTarget; ComPtr<ID3D11Texture2D> depthTexture; ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11VertexShader> vertexShader; ComPtr<ID3D11PixelShader> pixelShader; ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11Buffer> constants; ComPtr<ID3D11Buffer> vertices; ComPtr<ID3D11Buffer> indices;
    ComPtr<ID3D11RasterizerState> rasterizer; ComPtr<ID3D11DepthStencilState> depthState; UINT indexCount = 0;
};

D3D11ModelViewport::D3D11ModelViewport() = default;
D3D11ModelViewport::~D3D11ModelViewport() { Destroy(); }
bool D3D11ModelViewport::Create(HWND parent, const RECT& bounds, std::shared_ptr<ModelDocument> document, std::wstring& error) {
    Destroy(); document_ = std::move(document); if (!document_) { error=L"No model was loaded."; return false; }
    WNDCLASSW wc{}; wc.lpfnWndProc=WindowProc; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=kViewportClass; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); static const ATOM atom=RegisterClassW(&wc); if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { TraceViewportFailure(L"RegisterClassW", E_FAIL, GetLastError()); error=L"The 3D viewport could not be registered."; return false; }
    window_=CreateWindowExW(0,kViewportClass,L"",WS_CHILD|WS_VISIBLE, bounds.left,bounds.top,std::max(1L,bounds.right-bounds.left),std::max(1L,bounds.bottom-bounds.top),parent,nullptr,GetModuleHandleW(nullptr),this);
    if(!window_){TraceViewportFailure(L"CreateWindowExW", E_FAIL, GetLastError());error=L"The 3D viewport could not be created."; Destroy(); return false;} if(!InitializeD3D(error) || !UploadModel(error)){Destroy();return false;} Fit(); Render(); return true;
}
void D3D11ModelViewport::Destroy() { if(window_){DestroyWindow(window_);window_=nullptr;} DiscardD3D(); document_.reset(); }
void D3D11ModelViewport::SetBounds(const RECT& bounds) { if(window_) SetWindowPos(window_,nullptr,bounds.left,bounds.top,std::max(1L,bounds.right-bounds.left),std::max(1L,bounds.bottom-bounds.top),SWP_NOZORDER|SWP_NOACTIVATE); }
void D3D11ModelViewport::SetVisible(bool visible) { if(window_) ShowWindow(window_,visible?SW_SHOWNA:SW_HIDE); if(visible) Render(); }
bool D3D11ModelViewport::IsVisible() const { return window_ && IsWindowVisible(window_); }
void D3D11ModelViewport::Fit() { if(document_){camera_.Fit(document_->bounds,height_?float(width_)/height_:1.0f);NotifyCameraChanged();} }
void D3D11ModelViewport::ApplySpaceMouse(float x,float y,float z,float pitch,float yaw,float roll){camera_.ApplySpaceMouse(x,y,z,pitch,yaw,roll);NotifyCameraChanged();Render();}
bool D3D11ModelViewport::SetNavLibCameraState(const OrbitCamera::State& state){if(!camera_.SetFromNavLibState(state))return false;NotifyCameraChanged();Render();return true;}
bool D3D11ModelViewport::InitializeD3D(std::wstring& error) {
    resources_=std::make_unique<Resources>(); UINT flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT; D3D_FEATURE_LEVEL level{}; const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0}; HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,flags,levels,2,D3D11_SDK_VERSION,&resources_->device,&level,&resources_->context); if(FAILED(hr)) { TraceViewportFailure(L"D3D11CreateDevice hardware", hr); hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,flags,levels,2,D3D11_SDK_VERSION,&resources_->device,&level,&resources_->context); } if(FAILED(hr)){TraceViewportFailure(L"D3D11CreateDevice WARP",hr);error=L"Direct3D 11 could not initialize for this model.";return false;}
    DXGI_SWAP_CHAIN_DESC desc{}; desc.BufferCount=2; desc.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.OutputWindow=window_; desc.SampleDesc.Count=1; desc.Windowed=TRUE; desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD; ComPtr<IDXGIDevice> dxgiDevice; hr=resources_->device.As(&dxgiDevice); ComPtr<IDXGIAdapter> adapter; if(SUCCEEDED(hr)) hr=dxgiDevice->GetAdapter(&adapter); ComPtr<IDXGIFactory> factory; if(SUCCEEDED(hr)) hr=adapter->GetParent(IID_PPV_ARGS(&factory)); if(FAILED(hr)||FAILED(hr=factory->CreateSwapChain(resources_->device.Get(),&desc,&resources_->swapChain))){TraceViewportFailure(L"CreateSwapChain",hr);error=L"The model display surface could not initialize.";return false;}
    ComPtr<ID3DBlob> vs,ps,errors; if(FAILED(D3DCompile(kShader,sizeof(kShader)-1,nullptr,nullptr,nullptr,"VSMain","vs_4_0",0,0,&vs,&errors))||FAILED(D3DCompile(kShader,sizeof(kShader)-1,nullptr,nullptr,nullptr,"PSMain","ps_4_0",0,0,&ps,&errors))){error=L"The model renderer shader could not initialize.";return false;} resources_->device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&resources_->vertexShader); resources_->device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&resources_->pixelShader); D3D11_INPUT_ELEMENT_DESC layout[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}}; resources_->device->CreateInputLayout(layout,2,vs->GetBufferPointer(),vs->GetBufferSize(),&resources_->inputLayout);
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Constants);cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;resources_->device->CreateBuffer(&cb,nullptr,&resources_->constants); D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;resources_->device->CreateRasterizerState(&rs,&resources_->rasterizer); D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;resources_->device->CreateDepthStencilState(&ds,&resources_->depthState); Resize();return true;
}
bool D3D11ModelViewport::UploadModel(std::wstring& error) { if(!resources_||document_->geometries.empty()){error=L"The model has no renderable geometry.";return false;} const auto& mesh=document_->geometries.front(); if(mesh.positions.empty()||mesh.positions.size()!=mesh.normals.size()||mesh.indices.empty()){error=L"The model geometry is invalid.";return false;} std::vector<Vertex> vertices;vertices.reserve(mesh.positions.size());for(size_t i=0;i<mesh.positions.size();++i)vertices.push_back({mesh.positions[i],mesh.normals[i]});D3D11_BUFFER_DESC vb{};vb.ByteWidth=UINT(vertices.size()*sizeof(Vertex));vb.Usage=D3D11_USAGE_DEFAULT;vb.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA vdata{vertices.data()};D3D11_BUFFER_DESC ib{};ib.ByteWidth=UINT(mesh.indices.size()*sizeof(uint32_t));ib.Usage=D3D11_USAGE_DEFAULT;ib.BindFlags=D3D11_BIND_INDEX_BUFFER;D3D11_SUBRESOURCE_DATA idata{mesh.indices.data()};if(FAILED(resources_->device->CreateBuffer(&vb,&vdata,&resources_->vertices))||FAILED(resources_->device->CreateBuffer(&ib,&idata,&resources_->indices))){error=L"The model is too large for the graphics device.";return false;}resources_->indexCount=UINT(mesh.indices.size());return true;}
void D3D11ModelViewport::DiscardTargets(){if(resources_){resources_->depthView.Reset();resources_->depthTexture.Reset();resources_->renderTarget.Reset();}}
void D3D11ModelViewport::DiscardD3D(){DiscardTargets();resources_.reset();}
bool D3D11ModelViewport::CreateTargets(){if(!resources_||!width_||!height_)return false;ComPtr<ID3D11Texture2D> back; if(FAILED(resources_->swapChain->GetBuffer(0,IID_PPV_ARGS(&back)))||FAILED(resources_->device->CreateRenderTargetView(back.Get(),nullptr,&resources_->renderTarget)))return false;D3D11_TEXTURE2D_DESC d{};d.Width=width_;d.Height=height_;d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_DEPTH_STENCIL;return SUCCEEDED(resources_->device->CreateTexture2D(&d,nullptr,&resources_->depthTexture))&&SUCCEEDED(resources_->device->CreateDepthStencilView(resources_->depthTexture.Get(),nullptr,&resources_->depthView));}
void D3D11ModelViewport::Resize(){if(!window_||!resources_)return;RECT client{};GetClientRect(window_,&client);const UINT w=std::max(0L,client.right-client.left),h=std::max(0L,client.bottom-client.top);if(!w||!h)return;width_=w;height_=h;DiscardTargets();if(SUCCEEDED(resources_->swapChain->ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN,0)))CreateTargets();camera_.SetAspectRatio(float(w)/h);Render();}
void D3D11ModelViewport::Render(){if(!resources_||!resources_->renderTarget||!resources_->vertices||!IsVisible())return;float bg[]={0.07f,0.075f,0.085f,1};auto*c=resources_->context.Get();c->OMSetRenderTargets(1,resources_->renderTarget.GetAddressOf(),resources_->depthView.Get());c->ClearRenderTargetView(resources_->renderTarget.Get(),bg);c->ClearDepthStencilView(resources_->depthView.Get(),D3D11_CLEAR_DEPTH,1,0);D3D11_VIEWPORT v{0,0,float(width_),float(height_),0,1};c->RSSetViewports(1,&v);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);UINT stride=sizeof(Vertex),offset=0;c->IASetVertexBuffers(0,1,resources_->vertices.GetAddressOf(),&stride,&offset);c->IASetIndexBuffer(resources_->indices.Get(),DXGI_FORMAT_R32_UINT,0);c->IASetInputLayout(resources_->inputLayout.Get());c->RSSetState(resources_->rasterizer.Get());c->OMSetDepthStencilState(resources_->depthState.Get(),0);Constants constants{camera_.ViewProjection(),camera_.Position()};c->UpdateSubresource(resources_->constants.Get(),0,nullptr,&constants,0,0);c->VSSetConstantBuffers(0,1,resources_->constants.GetAddressOf());c->VSSetShader(resources_->vertexShader.Get(),nullptr,0);c->PSSetShader(resources_->pixelShader.Get(),nullptr,0);c->DrawIndexed(resources_->indexCount,0,0);const HRESULT hr=resources_->swapChain->Present(1,0);if(hr==DXGI_ERROR_DEVICE_REMOVED||hr==DXGI_ERROR_DEVICE_RESET){/* next content activation rebuilds from CPU model */DiscardD3D();}}
void D3D11ModelViewport::NotifyCameraChanged(){if(cameraChanged_)cameraChanged_(cameraContext_);}
LRESULT CALLBACK D3D11ModelViewport::WindowProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam){auto*self=reinterpret_cast<D3D11ModelViewport*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(message==WM_NCCREATE){self=reinterpret_cast<D3D11ModelViewport*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);if(!self)return FALSE;self->window_=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));return TRUE;}return self?self->HandleMessage(message,wParam,lParam):DefWindowProcW(window,message,wParam,lParam);}
LRESULT D3D11ModelViewport::HandleMessage(UINT message,WPARAM wParam,LPARAM lParam){switch(message){case WM_PAINT:{PAINTSTRUCT p{};BeginPaint(window_,&p);Render();EndPaint(window_,&p);return 0;}case WM_SIZE:Resize();return 0;case WM_LBUTTONDOWN:SetFocus(window_);drag_=Drag::Orbit;dragStart_={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};SetCapture(window_);return 0;case WM_MBUTTONDOWN:SetFocus(window_);drag_=Drag::Pan;dragStart_={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};SetCapture(window_);return 0;case WM_MOUSEMOVE:if(drag_!=Drag::None){POINT now{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};const float dx=float(now.x-dragStart_.x)/std::max(1u,width_),dy=float(now.y-dragStart_.y)/std::max(1u,height_);if(drag_==Drag::Orbit)camera_.Orbit(dx*4,dy*4);else camera_.Pan(dx*2,dy*2);dragStart_=now;NotifyCameraChanged();Render();}return 0;case WM_LBUTTONUP:case WM_MBUTTONUP:if(drag_!=Drag::None){drag_=Drag::None;if(GetCapture()==window_)ReleaseCapture();}return 0;case WM_MOUSEWHEEL:camera_.Dolly(float(GET_WHEEL_DELTA_WPARAM(wParam))/WHEEL_DELTA);NotifyCameraChanged();Render();return 0;case WM_KEYDOWN:if(wParam==L'0'||wParam==VK_NUMPAD0){Fit();Render();return 0;}return SendMessageW(GetParent(window_),message,wParam,lParam);case WM_CAPTURECHANGED:drag_=Drag::None;return 0;}return DefWindowProcW(window_,message,wParam,lParam);}
