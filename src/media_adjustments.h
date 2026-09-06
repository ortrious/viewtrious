#pragma once

#include <d3d11.h>
#include <wrl/client.h>

struct MediaAdjustments {
    float brightness = 0.0f;
    float contrast = 0.0f;
    float shadows = 0.0f;
    float highlights = 0.0f;

    bool IsNeutral() const;
};

// Reusable GPU display adjustment path for any BGRA D3D11 media texture.
class MediaAdjustmentProcessor {
public:
    bool Initialize(ID3D11Device* device);
    void Reset();
    bool Process(ID3D11Texture2D* source, UINT width, UINT height, const MediaAdjustments& adjustments);
    bool Analyze(ID3D11Texture2D* source, MediaAdjustments& adjustments);
    ID3D11Texture2D* OutputTexture() const { return outputTexture_.Get(); }

private:
    bool EnsureShaders();
    bool EnsureOutput(UINT width, UINT height);
    bool EnsureAnalysisResources();
    bool Render(ID3D11Texture2D* source, ID3D11RenderTargetView* target, UINT width, UINT height, const MediaAdjustments& adjustments);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> parameterBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> outputTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outputTarget_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> analysisTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> analysisTarget_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> analysisStaging_;
    UINT outputWidth_ = 0;
    UINT outputHeight_ = 0;
};
