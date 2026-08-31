#include "d3d11_model_viewport.h"

#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
struct Vertex { Float3 position; Float3 normal; };
struct Constants { Matrix4 viewProjection; Float3 eye; float pad = 0; };
Float3 Add(Float3 a,Float3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Float3 Sub(Float3 a,Float3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
Float3 Mul(Float3 a,float scalar){return {a.x*scalar,a.y*scalar,a.z*scalar};}
float Dot(Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Float3 Cross(Float3 a,Float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
Float3 Normalize(Float3 value){const float length=std::sqrt(Dot(value,value));return length>1e-6f?Mul(value,1.0f/length):Float3{0,0,1};}
const char kShader[] = R"(
cbuffer Constants : register(b0) { row_major float4x4 viewProjection; float3 eye; float padding; };
struct VSIn { float3 position : POSITION; float3 normal : NORMAL; };
struct VSOut { float4 position : SV_POSITION; float3 normal : NORMAL; };
VSOut VSMain(VSIn i) { VSOut o; o.position=mul(float4(i.position,1),viewProjection); o.normal=i.normal; return o; }
float4 PSMain(VSOut i) : SV_TARGET { float3 n=normalize(i.normal); float3 l0=normalize(float3(.45,.75,.55)); float3 l1=normalize(float3(-.55,.20,-.65)); float light=.22+.55*abs(dot(n,l0))+.23*abs(dot(n,l1)); return float4(float3(.72,.75,.80)*light,1); }
float4 PSHighlight(VSOut i) : SV_TARGET { return float4(float3(214.0/255.0,90.0/255.0,31.0/255.0),.75); }
)";
}
struct D3D11ModelViewport::Resources {
    ComPtr<ID3D11VertexShader> vertexShader; ComPtr<ID3D11PixelShader> pixelShader, highlightPixelShader; ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11Buffer> constants, vertices, indices, selectedIndices; ComPtr<ID3D11RasterizerState> rasterizer; ComPtr<ID3D11DepthStencilState> depthState; ComPtr<ID3D11BlendState> highlightBlend;
    ComPtr<ID3D11Texture2D> depthTexture; ComPtr<ID3D11DepthStencilView> depthView; UINT indexCount = 0, selectedIndexCount = 0;
};
D3D11ModelViewport::D3D11ModelViewport() = default;
D3D11ModelViewport::~D3D11ModelViewport() { Destroy(); }
bool D3D11ModelViewport::Create(GraphicsHost& host, std::shared_ptr<ModelDocument> document, std::wstring& error) {
    Destroy(); document_ = std::move(document); if (!document_) { error=L"No model was loaded."; return false; }
    if (!InitializeD3D(host,error) || !UploadModel(error)) { Destroy(); return false; } Fit(); return true;
}
void D3D11ModelViewport::Destroy() { DiscardD3D(); document_.reset(); drag_=Drag::None; homeAnimationActive_=false; width_=height_=0; sceneViewport_={}; selectedSnapPlane_=-1; uploadedSnapPlane_=-2; }
bool D3D11ModelViewport::InitializeD3D(GraphicsHost& host, std::wstring& error) {
    resources_=std::make_unique<Resources>(); ID3D11Device* device=host.Device(); if(!device){error=L"The shared graphics device is unavailable.";return false;}
    ComPtr<ID3DBlob> vs,ps,highlightPs; if(FAILED(D3DCompile(kShader,sizeof(kShader)-1,nullptr,nullptr,nullptr,"VSMain","vs_4_0",0,0,&vs,nullptr))||FAILED(D3DCompile(kShader,sizeof(kShader)-1,nullptr,nullptr,nullptr,"PSMain","ps_4_0",0,0,&ps,nullptr))||FAILED(D3DCompile(kShader,sizeof(kShader)-1,nullptr,nullptr,nullptr,"PSHighlight","ps_4_0",0,0,&highlightPs,nullptr))||FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&resources_->vertexShader))||FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&resources_->pixelShader))||FAILED(device->CreatePixelShader(highlightPs->GetBufferPointer(),highlightPs->GetBufferSize(),nullptr,&resources_->highlightPixelShader))){error=L"The model renderer shader could not initialize.";return false;}
    D3D11_INPUT_ELEMENT_DESC layout[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Constants);cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(device->CreateInputLayout(layout,ARRAYSIZE(layout),vs->GetBufferPointer(),vs->GetBufferSize(),&resources_->inputLayout))||FAILED(device->CreateBuffer(&cb,nullptr,&resources_->constants))){error=L"The model renderer could not initialize.";return false;}
    D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;device->CreateRasterizerState(&rs,&resources_->rasterizer);
    D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;device->CreateDepthStencilState(&ds,&resources_->depthState);D3D11_BLEND_DESC blend{};blend.RenderTarget[0].BlendEnable=TRUE;blend.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA;blend.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;blend.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;blend.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;blend.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;blend.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;device->CreateBlendState(&blend,&resources_->highlightBlend);return resources_->rasterizer&&resources_->depthState&&resources_->highlightBlend;
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
void D3D11ModelViewport::Render(GraphicsHost& host,const RECT& bounds){if(!resources_||!resources_->vertices)return;Resize(host,bounds);auto*c=host.Context();if(!c||!host.RenderTarget()||!resources_->depthView)return;const float bg[]={.07f,.075f,.085f,1};c->OMSetRenderTargets(1,host.RenderTargetAddress(),resources_->depthView.Get());c->ClearRenderTargetView(host.RenderTarget(),bg);c->ClearDepthStencilView(resources_->depthView.Get(),D3D11_CLEAR_DEPTH,1,0);sceneViewport_={float(bounds.left),float(bounds.top),float(width_),float(height_),0,1};c->RSSetViewports(1,&sceneViewport_);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);UINT stride=sizeof(Vertex),offset=0;c->IASetVertexBuffers(0,1,resources_->vertices.GetAddressOf(),&stride,&offset);c->IASetIndexBuffer(resources_->indices.Get(),DXGI_FORMAT_R32_UINT,0);c->IASetInputLayout(resources_->inputLayout.Get());c->RSSetState(resources_->rasterizer.Get());c->OMSetDepthStencilState(resources_->depthState.Get(),0);Constants constants{camera_.ViewProjection(),camera_.Position()};c->UpdateSubresource(resources_->constants.Get(),0,nullptr,&constants,0,0);c->VSSetConstantBuffers(0,1,resources_->constants.GetAddressOf());c->VSSetShader(resources_->vertexShader.Get(),nullptr,0);c->PSSetShader(resources_->pixelShader.Get(),nullptr,0);c->DrawIndexed(resources_->indexCount,0,0);if(UploadSelectedSnapPlane()&&resources_->selectedIndices){const float blendFactor[]={0,0,0,0};c->OMSetBlendState(resources_->highlightBlend.Get(),blendFactor,0xffffffff);c->IASetIndexBuffer(resources_->selectedIndices.Get(),DXGI_FORMAT_R32_UINT,0);c->PSSetShader(resources_->highlightPixelShader.Get(),nullptr,0);c->DrawIndexed(resources_->selectedIndexCount,0,0);c->OMSetBlendState(nullptr,blendFactor,0xffffffff);}}
void D3D11ModelViewport::Fit(){homeAnimationActive_=false;if(document_){camera_.Fit(document_->bounds,height_?float(width_)/height_:1.0f);NotifyCameraChanged();}}
bool D3D11ModelViewport::BeginAnimatedHome(){if(!document_)return false;homeAnimationStart_=camera_.CaptureAnimationState();OrbitCamera home;home.SetFieldOfView(camera_.FieldOfView());home.SetProjectionMode(camera_.ProjectionMode());home.Fit(document_->bounds,camera_.AspectRatio());homeAnimationTarget_=home.CaptureAnimationState();homeAnimationActive_=true;return true;}
bool D3D11ModelViewport::BeginAnimatedOrientation(Float3 forward,Float3 up){if(!document_)return false;homeAnimationStart_=camera_.CaptureAnimationState();homeAnimationTarget_=homeAnimationStart_;homeAnimationTarget_.forward=forward;homeAnimationTarget_.up=up;homeAnimationActive_=true;return true;}
bool D3D11ModelViewport::BeginAnimatedSnapView(Float3 forward,Float3 up,Float3 hitPoint){
    if(!document_)return false;homeAnimationStart_=camera_.CaptureAnimationState();homeAnimationTarget_=homeAnimationStart_;homeAnimationTarget_.forward=Normalize(forward);const Float3 viewRight=Normalize(Cross(up,homeAnimationTarget_.forward));homeAnimationTarget_.up=Normalize(Cross(homeAnimationTarget_.forward,viewRight));const Float3 framingRight=Normalize(Cross(homeAnimationTarget_.forward,homeAnimationTarget_.up));const Float3 hitFromPivot=Sub(hitPoint,homeAnimationTarget_.pivot);homeAnimationTarget_.framingRight=Dot(hitFromPivot,framingRight);homeAnimationTarget_.framingUp=Dot(hitFromPivot,homeAnimationTarget_.up);homeAnimationActive_=true;
#if defined(_DEBUG)
    const Float3 eye=Add(Sub(homeAnimationTarget_.pivot,Mul(homeAnimationTarget_.forward,homeAnimationTarget_.distance)),Add(Mul(framingRight,homeAnimationTarget_.framingRight),Mul(homeAnimationTarget_.up,homeAnimationTarget_.framingUp)));const Float3 cameraHit=Sub(hitPoint,eye);const float depth=Dot(homeAnimationTarget_.forward,cameraHit),f=1.0f/std::tan(homeAnimationTarget_.fieldOfView*.5f);const float ndcX=depth>1e-6f?Dot(viewRight,cameraHit)*f/homeAnimationTarget_.aspect/depth:0,ndcY=depth>1e-6f?Dot(homeAnimationTarget_.up,cameraHit)*f/depth:0;wchar_t message[640]{};swprintf_s(message,L"Viewtrious Snap target: currentFrame=(%.5f,%.5f) targetFrame=(%.5f,%.5f) currentDistance=%.5f targetDistance=%.5f viewRight=(%.5f,%.5f,%.5f) framingRight=(%.5f,%.5f,%.5f) targetUp=(%.5f,%.5f,%.5f) hitCamera=(%.5f,%.5f,%.5f) hitNdc=(%.6f,%.6f) valid=%d\\n",homeAnimationStart_.framingRight,homeAnimationStart_.framingUp,homeAnimationTarget_.framingRight,homeAnimationTarget_.framingUp,homeAnimationStart_.distance,homeAnimationTarget_.distance,viewRight.x,viewRight.y,viewRight.z,framingRight.x,framingRight.y,framingRight.z,homeAnimationTarget_.up.x,homeAnimationTarget_.up.y,homeAnimationTarget_.up.z,cameraHit.x,cameraHit.y,depth,ndcX,ndcY,std::fabs(ndcX)<1e-4f&&std::fabs(ndcY)<1e-4f);OutputDebugStringW(message);
#endif
    return true;
}
bool D3D11ModelViewport::AdvanceAnimatedHome(float progress){if(!homeAnimationActive_)return false;if(progress>=1.0f){camera_.ApplyInterpolatedAnimationState(homeAnimationStart_,homeAnimationTarget_,1.0f);homeAnimationActive_=false;NotifyCameraChanged();return false;}camera_.ApplyInterpolatedAnimationState(homeAnimationStart_,homeAnimationTarget_,progress);NotifyCameraChanged();return true;}
void D3D11ModelViewport::ApplySpaceMouse(float x,float y,float z,float pitch,float yaw,float roll){homeAnimationActive_=false;camera_.ApplySpaceMouse(x,y,z,pitch,yaw,roll);NotifyCameraChanged();}
bool D3D11ModelViewport::SetNavLibCameraState(const OrbitCamera::State& state){homeAnimationActive_=false;if(!camera_.SetFromNavLibState(state))return false;NotifyCameraChanged();return true;}
bool D3D11ModelViewport::SetNavLibCameraTarget(Float3 target){homeAnimationActive_=false;if(!camera_.SetCameraTargetFromNavLib(target))return false;NotifyCameraChanged();return true;}
bool D3D11ModelViewport::SetNavLibPivot(Float3 pivot){homeAnimationActive_=false;if(!camera_.SetPivotFromNavLib(pivot))return false;NotifyCameraChanged();return true;}
void D3D11ModelViewport::BeginOrbit(POINT point){homeAnimationActive_=false;drag_=Drag::Orbit;dragStart_=point;}
void D3D11ModelViewport::BeginPan(POINT point){homeAnimationActive_=false;drag_=Drag::Pan;dragStart_=point;}
void D3D11ModelViewport::ContinueDrag(POINT point,UINT width,UINT height){if(drag_==Drag::None)return;homeAnimationActive_=false;const float pixelDx=float(point.x-dragStart_.x),pixelDy=float(point.y-dragStart_.y);if(pixelDx==0.0f&&pixelDy==0.0f)return;if(drag_==Drag::Orbit)camera_.Orbit(pixelDx/std::max(1u,width)*4,pixelDy/std::max(1u,height)*4);else camera_.PanPixels(pixelDx,pixelDy,width,height);dragStart_=point;NotifyCameraChanged();}
void D3D11ModelViewport::EndDrag(){drag_=Drag::None;}
void D3D11ModelViewport::Dolly(float steps){homeAnimationActive_=false;camera_.Dolly(steps);NotifyCameraChanged();}
bool D3D11ModelViewport::SetSelectedSnapPlane(uint32_t plane){if(!document_||plane>=document_->snapPlanes.size())return false;selectedSnapPlane_=static_cast<int>(plane);return true;}
void D3D11ModelViewport::ClearSelectedSnapPlane(){selectedSnapPlane_=-1;}
bool D3D11ModelViewport::UploadSelectedSnapPlane(){if(!resources_||uploadedSnapPlane_==selectedSnapPlane_)return true;resources_->selectedIndices.Reset();resources_->selectedIndexCount=0;uploadedSnapPlane_=selectedSnapPlane_;if(selectedSnapPlane_<0||!document_||document_->geometries.empty())return true;const auto& mesh=document_->geometries.front();const auto& plane=document_->snapPlanes[static_cast<size_t>(selectedSnapPlane_)];std::vector<uint32_t> indices;indices.reserve(plane.triangles.size()*3);for(uint32_t triangle:plane.triangles){const size_t offset=size_t(triangle)*3;if(offset+2<mesh.indices.size()){indices.push_back(mesh.indices[offset]);indices.push_back(mesh.indices[offset+1]);indices.push_back(mesh.indices[offset+2]);}}if(indices.empty())return true;D3D11_BUFFER_DESC desc{};desc.ByteWidth=UINT(indices.size()*sizeof(uint32_t));desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_INDEX_BUFFER;D3D11_SUBRESOURCE_DATA data{indices.data()};ComPtr<ID3D11Device> device;resources_->vertices->GetDevice(&device);if(!device||FAILED(device->CreateBuffer(&desc,&data,&resources_->selectedIndices))){resources_->selectedIndices.Reset();return false;}resources_->selectedIndexCount=UINT(indices.size());return true;}
void D3D11ModelViewport::NotifyCameraChanged(){if(cameraChanged_)cameraChanged_(cameraContext_);}
