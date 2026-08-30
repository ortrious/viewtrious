#pragma once

#include "model_document.h"

class OrbitCamera {
public:
    struct State { Float3 position; Float3 forward; Float3 up; };
    void Fit(const ModelBounds& bounds, float aspectRatio);
    void SetAspectRatio(float aspectRatio);
    void Orbit(float deltaX, float deltaY);
    void Pan(float deltaX, float deltaY);
    void Dolly(float wheelUnits);
    void ApplySpaceMouse(float x, float y, float z, float pitch, float yaw, float roll);
    bool SetFromNavLibState(const State& state);
    State NavLibState() const;
    void SetPivot(Float3 pivot);
    void SetFieldOfView(float radians);
    const Matrix4& ViewProjection() const;
    Float3 Position() const;
    Float3 Pivot() const { return pivot_; }
    float FieldOfView() const { return fieldOfView_; }
    float Distance() const { return distance_; }
    float Radius() const { return radius_; }
    float AspectRatio() const { return aspect_; }

private:
    void Update();
    Float3 pivot_{};
    float yaw_ = 0.62f, pitch_ = -0.42f, roll_ = 0.0f;
    float distance_ = 5.0f, radius_ = 1.0f, aspect_ = 1.0f;
    float fieldOfView_ = 0.785398163f;
    Matrix4 viewProjection_ = Matrix4::Identity();
};
