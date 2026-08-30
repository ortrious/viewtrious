#pragma once

#include <windows.h>
#include "model_camera.h"

#include <memory>
#include <string>

class D3D11ModelViewport {
public:
    using CameraChanged = void (*) (void* context);
    D3D11ModelViewport();
    ~D3D11ModelViewport();
    D3D11ModelViewport(const D3D11ModelViewport&) = delete;
    D3D11ModelViewport& operator=(const D3D11ModelViewport&) = delete;

    bool Create(HWND parent, const RECT& bounds, std::shared_ptr<ModelDocument> document, std::wstring& error);
    void Destroy();
    void SetBounds(const RECT& bounds);
    void SetVisible(bool visible);
    void Render();
    void Fit();
    void ApplySpaceMouse(float x, float y, float z, float pitch, float yaw, float roll);
    bool SetNavLibCameraState(const OrbitCamera::State& state);
    OrbitCamera::State NavLibCameraState() const { return camera_.NavLibState(); }
    bool IsVisible() const;
    HWND Window() const { return window_; }
    OrbitCamera& Camera() { return camera_; }
    const OrbitCamera& Camera() const { return camera_; }
    const ModelDocument* Document() const { return document_.get(); }
    void SetCameraChangedCallback(CameraChanged callback, void* context) { cameraChanged_ = callback; cameraContext_ = context; }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    bool InitializeD3D(std::wstring& error);
    bool UploadModel(std::wstring& error);
    bool CreateTargets();
    void DiscardTargets();
    void DiscardD3D();
    void Resize();
    void NotifyCameraChanged();

    HWND window_ = nullptr;
    std::shared_ptr<ModelDocument> document_;
    OrbitCamera camera_;
    CameraChanged cameraChanged_ = nullptr;
    void* cameraContext_ = nullptr;
    POINT dragStart_{};
    enum class Drag { None, Orbit, Pan } drag_ = Drag::None;
    UINT width_ = 0, height_ = 0;
    struct Resources;
    std::unique_ptr<Resources> resources_;
};
