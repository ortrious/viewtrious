#include "media_adjustments.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {


struct ShaderParameters { float brightness, contrast, shadows, highlights; };
struct ImageShaderParameters { float exposure, brightness, shadows, highlights, contrast, saturation, sharpness, padding0, texelWidth, texelHeight, padding1, padding2; };

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
cbuffer Parameters : register(b0) { float4 light; float4 color; float4 source; };
struct VertexOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
VertexOutput VSMain(uint index : SV_VertexID) { float2 positions[3] = { float2(-1,-1), float2(-1,3), float2(3,-1) }; float2 uvs[3] = { float2(0,1), float2(0,-1), float2(2,1) }; VertexOutput output; output.position=float4(positions[index],0,1); output.uv=uvs[index]; return output; }

float3 SrgbToLinear(float3 value) {
    const float3 threshold = float3(0.04045, 0.04045, 0.04045);
    const float3 low = value / 12.92;
    const float3 high = pow(max((value + 0.055) / 1.055, 0.0), 2.4);
    return lerp(high, low, step(value, threshold));
}

float3 LinearToSrgb(float3 value) {
    const float3 threshold = float3(0.0031308, 0.0031308, 0.0031308);
    value = max(value, 0.0);
    const float3 low = value * 12.92;
    const float3 high = 1.055 * pow(value, 1.0 / 2.4) - 0.055;
    return lerp(high, low, step(value, threshold));
}

float ShadowMask(float tone) { return 1.0 - smoothstep(0.06, 0.60, tone); }
float HighlightMask(float tone) { return smoothstep(0.40, 0.96, tone); }

float3 AdjustLinearRgb(float3 linearRgb) {
    // WIC supplies premultiplied sRGB. Work on straight, linear RGB so tone
    // operations act on light rather than independently on encoded channels.
    linearRgb *= exp2(light.x * 2.0);

    const float3 lumaWeights = float3(0.2126, 0.7152, 0.0722);
    const float luminance = dot(linearRgb, lumaWeights);
    float tone = LinearToSrgb(luminance.xxx).x;

    // The explicit Image2D order is exposure, brightness, shadows/highlights,
    // contrast, then saturation. Every tonal control addresses this one tone.
    tone += light.y * (0.22 + 0.28 * (1.0 - tone));
    tone += light.z * 0.62 * ShadowMask(tone);
    tone += light.w * 0.46 * HighlightMask(tone);
    tone = saturate((tone - 0.5) * (1.0 + color.x * 0.72) + 0.5);

    const float targetLuminance = SrgbToLinear(tone.xxx).x;
    // Preserve chroma with a common gain. Capping only extreme near-black
    // amplification avoids colored noise from unstable luminance division.
    const float gain = min(targetLuminance / max(luminance, 0.0005), 12.0);
    float3 adjusted = linearRgb * gain;

    const float adjustedLuminance = dot(adjusted, lumaWeights);
    adjusted = lerp(adjustedLuminance.xxx, adjusted, 1.0 + color.y);

    // Compress gamut jointly rather than clipping individual channels, which
    // keeps bright saturated colors from abruptly shifting hue.
    const float peak = max(adjusted.r, max(adjusted.g, adjusted.b));
    adjusted /= max(1.0, peak);
    return adjusted;
}

float4 SampleAdjusted(float2 uv) {
    const float4 sample = inputTexture.Sample(inputSampler, uv);
    if (sample.a <= 0.0001) return float4(0.0, 0.0, 0.0, 0.0);
    const float3 linearRgb = SrgbToLinear(saturate(sample.rgb / sample.a));
    return float4(AdjustLinearRgb(linearRgb), sample.a);
}

float4 PSMain(VertexOutput input) : SV_TARGET {
    const float4 sample = SampleAdjusted(input.uv);
    if (sample.a <= 0.0001) return float4(0.0, 0.0, 0.0, 0.0);
    float3 adjusted = sample.rgb;

    // Use an alpha-weighted four-neighbor unsharp mask. Transparent neighbors
    // contribute no color, preserving straight alpha without edge fringes.
    if (color.z > 0.0) {
        const float2 texel = source.xy * source.z;
        const float4 north = SampleAdjusted(input.uv + float2(0.0, -texel.y));
        const float4 south = SampleAdjusted(input.uv + float2(0.0, texel.y));
        const float4 west = SampleAdjusted(input.uv + float2(-texel.x, 0.0));
        const float4 east = SampleAdjusted(input.uv + float2(texel.x, 0.0));
        float3 blurred = adjusted * sample.a;
        float weight = sample.a;
        blurred += 0.25 * north.rgb * north.a; weight += 0.25 * north.a;
        blurred += 0.25 * south.rgb * south.a; weight += 0.25 * south.a;
        blurred += 0.25 * west.rgb * west.a; weight += 0.25 * west.a;
        blurred += 0.25 * east.rgb * east.a; weight += 0.25 * east.a;
        blurred /= max(weight, 0.0001);
        adjusted += color.z * 3.00 * (adjusted - blurred);
        const float peak = max(adjusted.r, max(adjusted.g, adjusted.b));
        adjusted = max(adjusted, 0.0) / max(1.0, peak);
    }
    return float4(LinearToSrgb(adjusted) * sample.a, sample.a);
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

bool MediaAdjustmentProcessor::RenderImage(ID3D11Texture2D* source, ID3D11RenderTargetView* target, UINT width, UINT height, const ImageAdjustments& adjustments, float sharpnessTexelRadius) {
    if (!source || !target || !context_ || !EnsureImageShaders()) return false;
    ComPtr<ID3D11ShaderResourceView> sourceView;
    if (FAILED(device_->CreateShaderResourceView(source, nullptr, &sourceView))) return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(imageParameterBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    *static_cast<ImageShaderParameters*>(mapped.pData) = { adjustments.exposure, adjustments.brightness, adjustments.shadows, adjustments.highlights, adjustments.contrast, adjustments.saturation, adjustments.sharpness, 0.0f, 1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height), sharpnessTexelRadius, 0.0f };
    context_->Unmap(imageParameterBuffer_.Get(), 0);
    const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    ID3D11RenderTargetView* targets[] = { target }; ID3D11ShaderResourceView* views[] = { sourceView.Get() }; ID3D11SamplerState* samplers[] = { sampler_.Get() }; ID3D11Buffer* buffers[] = { imageParameterBuffer_.Get() };
    context_->OMSetRenderTargets(1, targets, nullptr); context_->RSSetViewports(1, &viewport); context_->IASetInputLayout(nullptr); context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(imageVertexShader_.Get(), nullptr, 0); context_->PSSetShader(imagePixelShader_.Get(), nullptr, 0); context_->PSSetShaderResources(0, 1, views); context_->PSSetSamplers(0, 1, samplers); context_->PSSetConstantBuffers(0, 1, buffers); context_->Draw(3, 0);
    ID3D11ShaderResourceView* nullViews[] = { nullptr }; context_->PSSetShaderResources(0, 1, nullViews); context_->ClearState(); return true;
}

bool MediaAdjustmentProcessor::ProcessImage(ID3D11Texture2D* source, UINT width, UINT height, const ImageAdjustments& adjustments, float sharpnessTexelRadius) {
    if (adjustments.IsNeutral()) return true;
    return device_ && EnsureOutput(width, height) && RenderImage(source, outputTarget_.Get(), width, height, adjustments, sharpnessTexelRadius);
}

bool ImageAdjustments::IsNeutral() const {
    return exposure == 0.0f && brightness == 0.0f && contrast == 0.0f && shadows == 0.0f && highlights == 0.0f && saturation == 0.0f && sharpness == 0.0f;
}
