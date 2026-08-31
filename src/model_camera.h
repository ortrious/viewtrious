#pragma once

#include "model_document.h"

enum class ModelProjectionMode { Perspective, Orthographic };

class OrbitCamera {
public:
    struct State { Float3 position; Float3 forward; Float3 up; };
    struct AnimationState {
        Float3 pivot;
        Float3 forward;
        Float3 up;
        float framingRight;
        float framingUp;
        float distance;
        float radius;
        float aspect;
        float fieldOfView;
        float orthographicHalfHeight;
        ModelProjectionMode projectionMode;
    };
    void Fit(const ModelBounds& bounds, float aspectRatio, Float3 upAxis);
    void SetAspectRatio(float aspectRatio);
    void Orbit(float deltaX, float deltaY);
    void Pan(float deltaX, float deltaY);
    void PanPixels(float deltaX, float deltaY, unsigned int viewportWidth, unsigned int viewportHeight);
    void Dolly(float wheelUnits);
    void ApplySpaceMouse(float x, float y, float z, float pitch, float yaw, float roll);
    bool SetFromNavLibState(const State& state);
    bool SetCameraTargetFromNavLib(Float3 target);
    bool SetPivotFromNavLib(Float3 pivot);
    AnimationState CaptureAnimationState();
    void ApplyInterpolatedAnimationState(const AnimationState& start, const AnimationState& target, float progress);
    State NavLibState() const;
    void SetPivot(Float3 pivot);
    void SetFieldOfView(float radians);
    void SetProjectionMode(ModelProjectionMode mode);
    void SetOrthographicHalfHeight(float halfHeight);
    const Matrix4& ViewProjection() const;
    Float3 Position() const;
    Float3 Pivot() const;
    Float3 CameraTarget() const;
    float FieldOfView() const { return fieldOfView_; }
    float Distance() const;
    struct ClipPlanes { float nearPlane; float farPlane; };
    ClipPlanes CurrentClipPlanes() const;
    float Radius() const { return radius_; }
    float AspectRatio() const { return aspect_; }
    ModelProjectionMode ProjectionMode() const { return projectionMode_; }
    float ViewHalfHeight() const;

private:
    void MaterializeNavLibState();
    void SetLocalOrientation(Float3 forward, Float3 up);
    Float3 LocalFramingOffset() const;
    void Update();
    Float3 pivot_{};
    Float3 forward_{ 0.0f, 0.0f, -1.0f };
    Float3 up_{ 0.0f, 1.0f, 0.0f };
    float framingRight_ = 0.0f, framingUp_ = 0.0f;
    float distance_ = 5.0f, radius_ = 1.0f, aspect_ = 1.0f;
    float fieldOfView_ = 0.785398163f;
    float orthographicHalfHeight_ = 1.0f;
    ModelProjectionMode projectionMode_ = ModelProjectionMode::Perspective;
    State navLibState_{};
    bool navLibStateActive_ = false;
    Matrix4 viewProjection_ = Matrix4::Identity();
};
