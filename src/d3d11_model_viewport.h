#pragma once

#include <windows.h>
#include "model_camera.h"
#include "graphics_host.h"

#include <memory>
#include <string>

class D3D11ModelViewport {
public:
    using CameraChanged = void (*) (void* context);
    D3D11ModelViewport();
    ~D3D11ModelViewport();
    D3D11ModelViewport(const D3D11ModelViewport&) = delete;
    D3D11ModelViewport& operator=(const D3D11ModelViewport&) = delete;

    bool Create(GraphicsHost& host, std::shared_ptr<ModelDocument> document, std::wstring& error);
    void Destroy();
    void Resize(GraphicsHost& host, const RECT& bounds);
    void Render(GraphicsHost& host, const RECT& bounds);
    void Fit();
    bool BeginAnimatedHome();
    bool BeginAnimatedOrientation(Float3 forward, Float3 up);
    bool BeginAnimatedSnapView(Float3 forward, Float3 up, Float3 hitPoint);
    bool BeginAnimatedFramingRecovery();
    bool AdvanceAnimatedHome(float progress);
    void CancelAnimatedHome() { homeAnimationActive_ = false; }
    void ApplySpaceMouse(float x, float y, float z, float pitch, float yaw, float roll);
    bool SetNavLibCameraState(const OrbitCamera::State& state);
    bool SetNavLibCameraTarget(Float3 target);
    OrbitCamera::State NavLibCameraState() const { return camera_.NavLibState(); }
    ModelBounds ModelBoundsForNavLib() const { return document_ ? document_->bounds : ModelBounds{}; }
    bool SetNavLibPivot(Float3 pivot);
    void SetNavLibFieldOfView(float radians) { camera_.SetFieldOfView(radians); NotifyCameraChanged(); }
    void SetProjectionMode(ModelProjectionMode mode) { camera_.SetProjectionMode(mode); }
    void SetVisualStyle(ModelVisualStyle style) { visualStyle_ = style; }
    void SetAntiAliasing(ModelAntiAliasing mode) { if (antiAliasing_ != mode) { antiAliasing_ = mode; width_ = height_ = 0; } }
    ModelAntiAliasing EffectiveAntiAliasing() const { return effectiveAntiAliasing_; }
    void SetOrthographicHalfHeight(float halfHeight) { camera_.SetOrthographicHalfHeight(halfHeight); }
    bool Active() const { return resources_ != nullptr; }
    void BeginOrbit(POINT point);
    void BeginPan(POINT point);
    void ContinueDrag(POINT point, UINT width, UINT height);
    void EndDrag();
    void Dolly(float steps);
    bool SetSelectedSnapPlane(uint32_t plane);
    void ClearSelectedSnapPlane();
    OrbitCamera& Camera() { return camera_; }
    const OrbitCamera& Camera() const { return camera_; }
    const ModelDocument* Document() const { return document_.get(); }
    D3D11_VIEWPORT LogicalViewport() const { return logicalViewport_; }
    D3D11_VIEWPORT RenderViewport() const { return renderViewport_; }
    void SetCameraChangedCallback(CameraChanged callback, void* context) { cameraChanged_ = callback; cameraContext_ = context; }

private:
    bool InitializeD3D(GraphicsHost& host, std::wstring& error);
    bool UploadModel(std::wstring& error);
    bool CreateTargets();
    void DiscardTargets();
    void DiscardD3D();
    void ResizeDepth(GraphicsHost& host, UINT width, UINT height);
    void NotifyCameraChanged();
    bool UploadSelectedSnapPlane();

    std::shared_ptr<ModelDocument> document_;
    OrbitCamera camera_;
    OrbitCamera::AnimationState homeAnimationStart_{};
    OrbitCamera::AnimationState homeAnimationTarget_{};
    bool homeAnimationActive_ = false;
    CameraChanged cameraChanged_ = nullptr;
    void* cameraContext_ = nullptr;
    POINT dragStart_{};
    enum class Drag { None, Orbit, Pan } drag_ = Drag::None;
    UINT width_ = 0, height_ = 0;
    D3D11_VIEWPORT logicalViewport_{};
    D3D11_VIEWPORT renderViewport_{};
    int selectedSnapPlane_ = -1;
    ModelVisualStyle visualStyle_ = ModelVisualStyle::Shaded;
    ModelAntiAliasing antiAliasing_ = ModelAntiAliasing::Msaa4x;
    ModelAntiAliasing effectiveAntiAliasing_ = ModelAntiAliasing::Off;
    int uploadedSnapPlane_ = -2;
    struct Resources;
    std::unique_ptr<Resources> resources_;
};
