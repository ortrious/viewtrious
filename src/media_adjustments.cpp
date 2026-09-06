#include "media_adjustments.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {


struct ShaderParameters { float brightness, contrast, shadows, highlights; };
struct ImageShaderParameters { float exposure, brightness, shadows, highlights, contrast, saturation, padding0, padding1; };

constexpr char kAdjustmentShader[] = R"(
Texture2D inputTexture : register(t0);
SamplerState inputSampler : register(s0);
cbuffer Parameters : register(b0) { float4 adjustments; };
struct VertexOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
VertexOutput VSMain(uint index : SV_VertexID) {
    float2 positions[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    float2 uvs[3] = { float2(0.0, 1.0), float2(0.0, -1.0), float2(2.0, 1.0) };
    VertexOutput output; output.position = float4(positions[index], 0.0, 1.0); output.uv = uvs[index]; return output;
}
float4 PSMain(VertexOutput input) : SV_TARGET {
    float4 sample = inputTexture.Sample(inputSampler, input.uv);
    float alpha = sample.a;
    float3 color = alpha > 0.001 ? sample.rgb / alpha : float3(0.0, 0.0, 0.0);
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    float adjusted = luminance;
    adjusted = adjusted * exp2(adjustments.x * 0.60) + max(adjustments.x, 0.0) * 0.035 * (1.0 - adjusted);
    adjusted = (adjusted - 0.5) * (1.0 + adjustments.y * 0.65) + 0.5;
    float shadowWeight = 1.0 - smoothstep(0.10, 0.62, saturate(adjusted));
    float highlightWeight = smoothstep(0.38, 0.92, saturate(adjusted));
    adjusted += adjustments.z * 0.34 * shadowWeight;
    adjusted += adjustments.w * 0.30 * highlightWeight;
    adjusted = saturate(adjusted);
    float3 chroma = luminance > 0.001 ? color / luminance : float3(1.0, 1.0, 1.0);
    return float4(saturate(chroma * adjusted) * alpha, alpha);
}
)";

constexpr char kImageAdjustmentShader[] = R"(
Texture2D inputTexture : register(t0); SamplerState inputSampler : register(s0);
cbuffer Parameters : register(b0) { float4 light; float4 color; };
struct VertexOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
VertexOutput VSMain(uint index : SV_VertexID) { float2 positions[3] = { float2(-1,-1), float2(-1,3), float2(3,-1) }; float2 uvs[3] = { float2(0,1), float2(0,-1), float2(2,1) }; VertexOutput output; output.position=float4(positions[index],0,1); output.uv=uvs[index]; return output; }
float4 PSMain(VertexOutput input) : SV_TARGET {
    float4 sample=inputTexture.Sample(inputSampler,input.uv); float alpha=sample.a; float3 source=alpha>.001 ? sample.rgb/alpha : float3(0,0,0); source*=exp2(light.x*2.0);
    float luminance=dot(source,float3(.2126,.7152,.0722)); float perceptual=pow(saturate(luminance),.82); perceptual+=light.y*(.18+.28*(1.0-perceptual));
    perceptual+=light.z*.58*(1.0-smoothstep(.04,.55,saturate(perceptual))); perceptual+=light.w*.42*smoothstep(.45,.96,saturate(perceptual)); perceptual=saturate((perceptual-.5)*(1.0+color.x*.72)+.5);
    float targetLuminance=pow(perceptual,1.0/.82); float3 chroma=luminance>.0001 ? source/luminance : float3(1,1,1); float3 adjusted=saturate(chroma*targetLuminance); float gray=dot(adjusted,float3(.2126,.7152,.0722)); return float4(saturate(lerp(gray.xxx,adjusted,1.0+color.y))*alpha,alpha);
}
)";

bool CreateTexture(ID3D11Device* device, UINT width, UINT height, UINT bindFlags, D3D11_USAGE usage, UINT cpuFlags, ComPtr<ID3D11Texture2D>& texture) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = usage; desc.BindFlags = bindFlags; desc.CPUAccessFlags = cpuFlags;
    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture));
}

} // namespace

bool MediaAdjustments::IsNeutral() const {
    return brightness == 0.0f && contrast == 0.0f && shadows == 0.0f && highlights == 0.0f;
}

bool MediaAdjustmentProcessor::Initialize(ID3D11Device* device) {
    Reset();
    if (!device) return false;
    device_ = device;
    device_->GetImmediateContext(&context_);
    return context_ != nullptr;
}

void MediaAdjustmentProcessor::Reset() {
    outputTarget_.Reset(); outputTexture_.Reset(); imageParameterBuffer_.Reset(); imagePixelShader_.Reset(); imageVertexShader_.Reset(); parameterBuffer_.Reset(); sampler_.Reset();
    pixelShader_.Reset(); vertexShader_.Reset(); context_.Reset(); device_.Reset();
    outputWidth_ = outputHeight_ = 0;
}

bool MediaAdjustmentProcessor::EnsureShaders() {
    if (vertexShader_ && pixelShader_ && sampler_ && parameterBuffer_) return true;
    if (!device_) return false;
    ComPtr<ID3DBlob> vertexBlob, pixelBlob;
    if (FAILED(D3DCompile(kAdjustmentShader, sizeof(kAdjustmentShader) - 1, nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vertexBlob, nullptr)) ||
        FAILED(D3DCompile(kAdjustmentShader, sizeof(kAdjustmentShader) - 1, nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &pixelBlob, nullptr)) ||
        FAILED(device_->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, &vertexShader_)) ||
        FAILED(device_->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr, &pixelShader_))) return false;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device_->CreateSamplerState(&sampler, &sampler_))) return false;
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(ShaderParameters); buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device_->CreateBuffer(&buffer, nullptr, &parameterBuffer_));
}

bool MediaAdjustmentProcessor::EnsureOutput(UINT width, UINT height) {
    if (outputTexture_ && outputWidth_ == width && outputHeight_ == height) return true;
    outputTarget_.Reset(); outputTexture_.Reset(); outputWidth_ = outputHeight_ = 0;
    if (!CreateTexture(device_.Get(), width, height, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, outputTexture_) ||
        FAILED(device_->CreateRenderTargetView(outputTexture_.Get(), nullptr, &outputTarget_))) { outputTexture_.Reset(); return false; }
    outputWidth_ = width; outputHeight_ = height;
    return true;
}


bool MediaAdjustmentProcessor::EnsureImageShaders() {
    if (imageVertexShader_ && imagePixelShader_ && imageParameterBuffer_) return true;
    if (!device_ || !EnsureShaders()) return false;
    ComPtr<ID3DBlob> vertexBlob, pixelBlob;
    if (FAILED(D3DCompile(kImageAdjustmentShader, sizeof(kImageAdjustmentShader) - 1, nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vertexBlob, nullptr)) ||
        FAILED(D3DCompile(kImageAdjustmentShader, sizeof(kImageAdjustmentShader) - 1, nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &pixelBlob, nullptr)) ||
        FAILED(device_->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, &imageVertexShader_)) ||
        FAILED(device_->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr, &imagePixelShader_))) return false;
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(ImageShaderParameters); buffer.Usage = D3D11_USAGE_DYNAMIC; buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device_->CreateBuffer(&buffer, nullptr, &imageParameterBuffer_));
}

bool MediaAdjustmentProcessor::Render(ID3D11Texture2D* source, ID3D11RenderTargetView* target, UINT width, UINT height, const MediaAdjustments& adjustments) {
    if (!source || !target || !context_ || !EnsureShaders()) return false;
    ComPtr<ID3D11ShaderResourceView> sourceView;
    if (FAILED(device_->CreateShaderResourceView(source, nullptr, &sourceView))) return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(parameterBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    *static_cast<ShaderParameters*>(mapped.pData) = { adjustments.brightness, adjustments.contrast, adjustments.shadows, adjustments.highlights };
    context_->Unmap(parameterBuffer_.Get(), 0);
    const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    ID3D11RenderTargetView* targets[] = { target };
    ID3D11ShaderResourceView* views[] = { sourceView.Get() };
    ID3D11SamplerState* samplers[] = { sampler_.Get() };
    ID3D11Buffer* buffers[] = { parameterBuffer_.Get() };
    context_->OMSetRenderTargets(1, targets, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, views);
    context_->PSSetSamplers(0, 1, samplers);
    context_->PSSetConstantBuffers(0, 1, buffers);
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* nullViews[] = { nullptr };
    context_->PSSetShaderResources(0, 1, nullViews);
    context_->ClearState();
    return true;
}

bool MediaAdjustmentProcessor::Process(ID3D11Texture2D* source, UINT width, UINT height, const MediaAdjustments& adjustments) {
    if (adjustments.IsNeutral()) return true;
    return device_ && EnsureOutput(width, height) && Render(source, outputTarget_.Get(), width, height, adjustments);
}

bool MediaAdjustmentProcessor::RenderImage(ID3D11Texture2D* source, ID3D11RenderTargetView* target, UINT width, UINT height, const ImageAdjustments& adjustments) {
    if (!source || !target || !context_ || !EnsureImageShaders()) return false;
    ComPtr<ID3D11ShaderResourceView> sourceView;
    if (FAILED(device_->CreateShaderResourceView(source, nullptr, &sourceView))) return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(imageParameterBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    *static_cast<ImageShaderParameters*>(mapped.pData) = { adjustments.exposure, adjustments.brightness, adjustments.shadows, adjustments.highlights, adjustments.contrast, adjustments.saturation, 0.0f, 0.0f };
    context_->Unmap(imageParameterBuffer_.Get(), 0);
    const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    ID3D11RenderTargetView* targets[] = { target }; ID3D11ShaderResourceView* views[] = { sourceView.Get() }; ID3D11SamplerState* samplers[] = { sampler_.Get() }; ID3D11Buffer* buffers[] = { imageParameterBuffer_.Get() };
    context_->OMSetRenderTargets(1, targets, nullptr); context_->RSSetViewports(1, &viewport); context_->IASetInputLayout(nullptr); context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(imageVertexShader_.Get(), nullptr, 0); context_->PSSetShader(imagePixelShader_.Get(), nullptr, 0); context_->PSSetShaderResources(0, 1, views); context_->PSSetSamplers(0, 1, samplers); context_->PSSetConstantBuffers(0, 1, buffers); context_->Draw(3, 0);
    ID3D11ShaderResourceView* nullViews[] = { nullptr }; context_->PSSetShaderResources(0, 1, nullViews); context_->ClearState(); return true;
}

bool MediaAdjustmentProcessor::ProcessImage(ID3D11Texture2D* source, UINT width, UINT height, const ImageAdjustments& adjustments) {
    if (adjustments.IsNeutral()) return true;
    return device_ && EnsureOutput(width, height) && RenderImage(source, outputTarget_.Get(), width, height, adjustments);
}

bool ImageAdjustments::IsNeutral() const {
    return exposure == 0.0f && brightness == 0.0f && contrast == 0.0f && shadows == 0.0f && highlights == 0.0f && saturation == 0.0f;
}
