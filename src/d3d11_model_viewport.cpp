#include "d3d11_model_viewport.h"

#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
struct Vertex { Float3 position; Float3 normal; };
struct Constants { Matrix4 viewProjection; Float3 eye; float pad = 0; };
const char kShader[] = R"(
cbuffer Constants : register(b0) { row_major float4x4 viewProjection; float3 eye; float padding; };
struct VSIn { float3 position : POSITION; float3 normal : NORMAL; };
struct VSOut { float4 position : SV_POSITION; float3 normal : NORMAL; };
VSOut VSMain(VSIn i) { VSOut o; o.position=mul(float4(i.position,1),viewProjection); o.normal=i.normal; return o; }
float4 PSMain(VSOut i) : SV_TARGET { float3 n=normalize(i.normal); float3 l0=normalize(float3(.45,.75,.55)); float3 l1=normalize(float3(-.55,.20,-.65)); float light=.22+.55*abs(dot(n,l0))+.23*abs(dot(n,l1)); return float4(float3(.72,.75,.80)*light,1); }
)";
}
struct D3D11ModelViewport::Resources {
    ComPtr<ID3D11VertexShader> vertexShader; ComPtr<ID3D11PixelShader> pixelShader; ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11Buffer> constants, vertices, indices; ComPtr<ID3D11RasterizerState> rasterizer; ComPtr<ID3D11DepthStencilState> depthState;
    ComPtr<ID3D11Texture2D> depthTexture; ComPtr<ID3D11DepthStencilView> depthView; UINT indexCount = 0;
};
D3D11ModelViewport::D3D11ModelViewport() = default;
D3D11ModelViewport::~D3D11ModelViewport() { Destroy(); }
bool D3D11ModelViewport::Create(GraphicsHost& host, std::shared_ptr<ModelDocument> document, std::wstring& error) {
    Destroy(); document_ = std::move(document); if (!document_) { error=L"No model was loaded."; return false; }
    if (!InitializeD3D(host,error) || !UploadModel(error)) { Destroy(); return false; } Fit(); return true;
}
void D3D11ModelViewport::Destroy() { DiscardD3D(); document_.reset(); drag_=Drag::None; width_=height_=0; }
bool D3D11ModelViewport::InitializeD3D(GraphicsHost& host, std::wstring& error) {
    resources_=std::make_unique<Resources>(); ID3D11Device* device=host.Device(); if(!device){error=L"The shared graphics device is unavailable.";return false;}
    ComPtr<ID3DBlob> vs,ps; if(FAILED(D3DCompile(kShader,sizeof(kShader)-1,nullptr,nullptr,nullptr,"VSMain","vs_4_0",0,0,&vs,nullptr))||FAILED(D3DCompile(kShader,sizeof(kShader)-1,nullptr,nullptr,nullptr,"PSMain","ps_4_0",0,0,&ps,nullptr))||FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&resources_->vertexShader))||FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&resources_->pixelShader))){error=L"The model renderer shader could not initialize.";return false;}
    D3D11_INPUT_ELEMENT_DESC layout[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Constants);cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(device->CreateInputLayout(layout,ARRAYSIZE(layout),vs->GetBufferPointer(),vs->GetBufferSize(),&resources_->inputLayout))||FAILED(device->CreateBuffer(&cb,nullptr,&resources_->constants))){error=L"The model renderer could not initialize.";return false;}
    D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;device->CreateRasterizerState(&rs,&resources_->rasterizer);
    D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;device->CreateDepthStencilState(&ds,&resources_->depthState);return true;
}
bool D3D11ModelViewport::UploadModel(std::wstring& error) {
    if(!resources_||!document_||document_->geometries.empty()){error=L"The model has no renderable geometry.";return false;} const auto& mesh=document_->geometries.front();if(mesh.positions.empty()||mesh.positions.size()!=mesh.normals.size()||mesh.indices.empty()){error=L"The model geometry is invalid.";return false;}
    std::vector<Vertex> vertices;vertices.reserve(mesh.positions.size());for(size_t i=0;i<mesh.positions.size();++i)vertices.push_back({mesh.positions[i],mesh.normals[i]});D3D11_BUFFER_DESC vb{};vb.ByteWidth=UINT(vertices.size()*sizeof(Vertex));vb.Usage=D3D11_USAGE_DEFAULT;vb.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA vd{vertices.data()};D3D11_BUFFER_DESC ib{};ib.ByteWidth=UINT(mesh.indices.size()*sizeof(uint32_t));ib.Usage=D3D11_USAGE_DEFAULT;ib.BindFlags=D3D11_BIND_INDEX_BUFFER;D3D11_SUBRESOURCE_DATA id{mesh.indices.data()};ComPtr<ID3D11Device> device;resources_->constants->GetDevice(&device);if(FAILED(device->CreateBuffer(&vb,&vd,&resources_->vertices))||FAILED(device->CreateBuffer(&ib,&id,&resources_->indices))){error=L"The model is too large for the graphics device.";return false;}resources_->indexCount=UINT(mesh.indices.size());return true;
}
void D3D11ModelViewport::DiscardTargets(){if(resources_){resources_->depthView.Reset();resources_->depthTexture.Reset();}}
void D3D11ModelViewport::DiscardD3D(){DiscardTargets();resources_.reset();}
bool D3D11ModelViewport::CreateTargets(){return false;}
void D3D11ModelViewport::ResizeDepth(GraphicsHost& host,UINT width,UINT height){if(!resources_||!width||!height||(width_==width&&height_==height&&resources_->depthView))return;DiscardTargets();width_=width;height_=height;D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_DEPTH_STENCIL;host.Device()->CreateTexture2D(&d,nullptr,&resources_->depthTexture);if(resources_->depthTexture)host.Device()->CreateDepthStencilView(resources_->depthTexture.Get(),nullptr,&resources_->depthView);camera_.SetAspectRatio(float(width)/height);}
void D3D11ModelViewport::Resize(GraphicsHost& host,const RECT& bounds){ResizeDepth(host,host.Width(),host.Height());camera_.SetAspectRatio(float(std::max(1L,bounds.right-bounds.left))/std::max(1L,bounds.bottom-bounds.top));}
void D3D11ModelViewport::Render(GraphicsHost& host,const RECT& bounds){if(!resources_||!resources_->vertices)return;Resize(host,bounds);auto*c=host.Context();if(!c||!host.RenderTarget()||!resources_->depthView)return;const float bg[]={.07f,.075f,.085f,1};c->OMSetRenderTargets(1,host.RenderTargetAddress(),resources_->depthView.Get());c->ClearRenderTargetView(host.RenderTarget(),bg);c->ClearDepthStencilView(resources_->depthView.Get(),D3D11_CLEAR_DEPTH,1,0);D3D11_VIEWPORT v{float(bounds.left),float(bounds.top),float(width_),float(height_),0,1};c->RSSetViewports(1,&v);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);UINT stride=sizeof(Vertex),offset=0;c->IASetVertexBuffers(0,1,resources_->vertices.GetAddressOf(),&stride,&offset);c->IASetIndexBuffer(resources_->indices.Get(),DXGI_FORMAT_R32_UINT,0);c->IASetInputLayout(resources_->inputLayout.Get());c->RSSetState(resources_->rasterizer.Get());c->OMSetDepthStencilState(resources_->depthState.Get(),0);Constants constants{camera_.ViewProjection(),camera_.Position()};c->UpdateSubresource(resources_->constants.Get(),0,nullptr,&constants,0,0);c->VSSetConstantBuffers(0,1,resources_->constants.GetAddressOf());c->VSSetShader(resources_->vertexShader.Get(),nullptr,0);c->PSSetShader(resources_->pixelShader.Get(),nullptr,0);c->DrawIndexed(resources_->indexCount,0,0);}
void D3D11ModelViewport::Fit(){if(document_){camera_.Fit(document_->bounds,height_?float(width_)/height_:1.0f);NotifyCameraChanged();}}
void D3D11ModelViewport::ApplySpaceMouse(float x,float y,float z,float pitch,float yaw,float roll){camera_.ApplySpaceMouse(x,y,z,pitch,yaw,roll);NotifyCameraChanged();}
bool D3D11ModelViewport::SetNavLibCameraState(const OrbitCamera::State& state){if(!camera_.SetFromNavLibState(state))return false;NotifyCameraChanged();return true;}
void D3D11ModelViewport::BeginOrbit(POINT point){drag_=Drag::Orbit;dragStart_=point;}
void D3D11ModelViewport::BeginPan(POINT point){drag_=Drag::Pan;dragStart_=point;}
void D3D11ModelViewport::ContinueDrag(POINT point,UINT width,UINT height){if(drag_==Drag::None)return;const float pixelDx=float(point.x-dragStart_.x),pixelDy=float(point.y-dragStart_.y);if(pixelDx==0.0f&&pixelDy==0.0f)return;if(drag_==Drag::Orbit)camera_.Orbit(pixelDx/std::max(1u,width)*4,pixelDy/std::max(1u,height)*4);else camera_.PanPixels(pixelDx,pixelDy,width,height);dragStart_=point;NotifyCameraChanged();}
void D3D11ModelViewport::EndDrag(){drag_=Drag::None;}
void D3D11ModelViewport::Dolly(float steps){camera_.Dolly(steps);NotifyCameraChanged();}
void D3D11ModelViewport::NotifyCameraChanged(){if(cameraChanged_)cameraChanged_(cameraContext_);}
