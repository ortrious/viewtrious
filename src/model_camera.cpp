#include "model_camera.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kPi = 3.1415926535f;
Float3 Add(Float3 a, Float3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Float3 Sub(Float3 a, Float3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Float3 Mul(Float3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float Dot(Float3 a, Float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 Cross(Float3 a, Float3 b) { return { a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x }; }
Float3 Normalize(Float3 value) { const float length = std::sqrt(Dot(value, value)); return length > 1e-12f ? Mul(value, 1.0f / length) : Float3{ 0, 0, 1 }; }
Float3 RotateAroundAxis(Float3 value, Float3 axis, float radians) {
    axis = Normalize(axis);
    const float c = std::cos(radians), s = std::sin(radians);
    return Add(Add(Mul(value, c), Mul(Cross(axis, value), s)), Mul(axis, Dot(axis, value) * (1.0f - c)));
}
}

void OrbitCamera::Fit(const ModelBounds& bounds, float aspectRatio) {
    navLibStateActive_ = false;
    pivot_ = Mul(Add(bounds.minimum, bounds.maximum), 0.5f);
    const Float3 diagonal = Sub(bounds.maximum, bounds.minimum);
    radius_ = std::max(1e-5f, 0.5f * std::sqrt(Dot(diagonal, diagonal)));
    aspect_ = std::max(0.01f, aspectRatio);
    distance_ = std::max(radius_ * 2.8f, radius_ / std::tan(fieldOfView_ * 0.5f));
    const float yaw = 0.62f, pitch = -0.42f, cosPitch = std::cos(pitch);
    const Float3 eye = Add(pivot_, { distance_ * std::sin(yaw) * cosPitch, distance_ * std::sin(pitch), distance_ * std::cos(yaw) * cosPitch });
    SetLocalOrientation(Normalize(Sub(pivot_, eye)), { 0.0f, 1.0f, 0.0f });
    Update();
}
void OrbitCamera::SetAspectRatio(float aspectRatio) { aspect_ = std::max(0.01f, aspectRatio); Update(); }
void OrbitCamera::Orbit(float dx, float dy) {
    MaterializeNavLibState();
    // Rotate about the live view axes, rather than a bounded world-up Euler pitch.
    // This remains continuous through every pole and when returning from SpaceMouse.
    forward_ = Normalize(RotateAroundAxis(forward_, up_, dx));
    const Float3 right = Normalize(Cross(up_, forward_));
    forward_ = Normalize(RotateAroundAxis(forward_, right, dy));
    up_ = Normalize(RotateAroundAxis(up_, right, dy));
    SetLocalOrientation(forward_, up_);
    Update();
}
void OrbitCamera::Pan(float dx, float dy) {
    MaterializeNavLibState();
    const Float3 forward = forward_;
    const Float3 right = Normalize(Cross(forward, up_)); const Float3 up = Normalize(Cross(right, forward));
    pivot_ = Add(pivot_, Add(Mul(right, dx * distance_), Mul(up, dy * distance_))); Update();
}
void OrbitCamera::Dolly(float wheelUnits) { MaterializeNavLibState(); distance_ = std::clamp(distance_ * std::exp(-wheelUnits * 0.14f), radius_ * 0.02f, radius_ * 10000.0f); Update(); }
void OrbitCamera::ApplySpaceMouse(float x, float y, float z, float pitch, float yaw, float roll) {
    MaterializeNavLibState();
    Pan(x * 0.06f, y * 0.06f); Dolly(-z * 0.45f); Orbit(yaw * 0.025f, pitch * 0.025f);
    up_ = Normalize(RotateAroundAxis(up_, forward_, roll * 0.025f));
    SetLocalOrientation(forward_, up_); Update();
}
OrbitCamera::State OrbitCamera::NavLibState() const {
    if (navLibStateActive_) return navLibState_;
    return { Position(), forward_, up_ };
}
void OrbitCamera::SetPivot(Float3 pivot) { if (std::isfinite(pivot.x) && std::isfinite(pivot.y) && std::isfinite(pivot.z)) { MaterializeNavLibState(); pivot_ = pivot; Update(); } }
void OrbitCamera::SetFieldOfView(float radians) { if (std::isfinite(radians)) { fieldOfView_ = std::clamp(radians, 0.17f, 2.6f); Update(); } }
bool OrbitCamera::SetFromNavLibState(const State& state) {
    if (!std::isfinite(state.position.x) || !std::isfinite(state.position.y) || !std::isfinite(state.position.z) ||
        !std::isfinite(state.forward.x) || !std::isfinite(state.forward.y) || !std::isfinite(state.forward.z) ||
        !std::isfinite(state.up.x) || !std::isfinite(state.up.y) || !std::isfinite(state.up.z) ||
        std::sqrt(Dot(state.forward, state.forward)) <= 1e-8f || std::sqrt(Dot(state.up, state.up)) <= 1e-8f) return false;
    const Float3 forward = Normalize(state.forward);
    const Float3 right = Normalize(Cross(state.up, forward));
    const Float3 up = Normalize(Cross(forward, right));
    // NavLib camera matrices are absolute. Their lateral camera-target displacement is
    // the Model3D pan request. Move the live orbit pivot by that view-plane component so
    // the camera and rotation centre remain one coherent panned state. The forward-axis
    // component is deliberately excluded: push/pull continues to be camera dolly only.
    const Float3 requestedTarget = Add(state.position, Mul(forward, Distance()));
    const Float3 targetOffset = Sub(requestedTarget, pivot_);
    const Float3 pan = Add(Mul(right, Dot(targetOffset, right)), Mul(up, Dot(targetOffset, up)));
    if (std::isfinite(pan.x) && std::isfinite(pan.y) && std::isfinite(pan.z)) pivot_ = Add(pivot_, pan);
    // Keep the exact accepted pose active so the next GetCameraMatrix returns the same
    // baseline at the next motion session.
    navLibState_ = { state.position, forward, up };
    navLibStateActive_ = true;
    Update();
    return true;
}
bool OrbitCamera::SetPivotFromNavLib(Float3 pivot) {
    if (!std::isfinite(pivot.x) || !std::isfinite(pivot.y) || !std::isfinite(pivot.z)) return false;
    MaterializeNavLibState();
    const State current = NavLibState();
    const Float3 eyeToPivot = Sub(pivot, current.position);
    const float distance = std::sqrt(Dot(eyeToPivot, eyeToPivot));
    if (!std::isfinite(distance) || distance <= std::max(radius_ * 1e-5f, 1e-8f) || distance > radius_ * 10000.0f) return false;

    // NavLib defines the pivot as a world-space rotation centre.  Preserve the eye and rebuild
    // the orbit relation so eye, forward, pivot, and distance remain one coherent state.
    pivot_ = pivot;
    distance_ = distance;
    SetLocalOrientation(Normalize(eyeToPivot), current.up);
    Update();
    return true;
}
Float3 OrbitCamera::Position() const {
    if (navLibStateActive_) return navLibState_.position;
    return Sub(pivot_, Mul(forward_, distance_));
}
Float3 OrbitCamera::Pivot() const {
    return pivot_;
}
float OrbitCamera::Distance() const {
    if (!navLibStateActive_) return distance_;
    const Float3 eyeToPivot = Sub(pivot_, navLibState_.position);
    const float distance = std::sqrt(Dot(eyeToPivot, eyeToPivot));
    return std::isfinite(distance) && distance > 1e-8f ? distance : distance_;
}
OrbitCamera::ClipPlanes OrbitCamera::CurrentClipPlanes() const {
    const float distance = Distance();
    const float nearPlane = std::max(radius_ * 0.001f, distance * 0.001f);
    return { nearPlane, std::max(nearPlane * 2.0f, distance + radius_ * 8.0f) };
}
void OrbitCamera::MaterializeNavLibState() {
    if (!navLibStateActive_) return;
    // Preserve the established live rotation centre while returning to local orbit
    // controls. It must never be reconstructed from the last camera eye.
    const Float3 eyeToPivot = Sub(pivot_, navLibState_.position);
    const float pivotDistance = std::sqrt(Dot(eyeToPivot, eyeToPivot));
    if (std::isfinite(pivotDistance) && pivotDistance > 1e-8f) distance_ = pivotDistance;
    const Float3 forward = pivotDistance > 1e-8f ? Mul(eyeToPivot, 1.0f / pivotDistance) : Normalize(navLibState_.forward);
    // Match the active NavLib rendering basis exactly before local mouse orbit starts.
    SetLocalOrientation(forward, navLibState_.up);
    navLibStateActive_ = false;
}
void OrbitCamera::SetLocalOrientation(Float3 forward, Float3 up) {
    forward_ = Normalize(forward);
    Float3 right = Cross(up, forward_);
    if (Dot(right, right) <= 1e-12f) {
        const Float3 fallback = std::fabs(forward_.y) < 0.9f ? Float3{ 0.0f, 1.0f, 0.0f } : Float3{ 1.0f, 0.0f, 0.0f };
        right = Cross(fallback, forward_);
    }
    right = Normalize(right);
    up_ = Normalize(Cross(forward_, right));
}
const Matrix4& OrbitCamera::ViewProjection() const { return viewProjection_; }
void OrbitCamera::Update() {
    const Float3 eye = Position();
    const Float3 forward = navLibStateActive_ ? Normalize(navLibState_.forward) : forward_;
    Float3 right{}; Float3 up{};
    if (navLibStateActive_) {
        right = Normalize(Cross(navLibState_.up, forward));
        up = Normalize(Cross(forward, right));
    } else {
        right = Normalize(Cross(up_, forward));
        up = Normalize(Cross(forward, right));
    }
    Matrix4 view{}; view.m[0]=right.x; view.m[4]=right.y; view.m[8]=right.z; view.m[1]=up.x; view.m[5]=up.y; view.m[9]=up.z; view.m[2]=-forward.x; view.m[6]=-forward.y; view.m[10]=-forward.z; view.m[12]=-Dot(right,eye); view.m[13]=-Dot(up,eye); view.m[14]=Dot(forward,eye); view.m[15]=1;
    const ClipPlanes clips = CurrentClipPlanes(); const float nearPlane = clips.nearPlane, farPlane = clips.farPlane; const float f = 1.0f / std::tan(fieldOfView_ * 0.5f);
    Matrix4 projection{}; projection.m[0]=f/aspect_; projection.m[5]=f; projection.m[10]=farPlane/(nearPlane-farPlane); projection.m[11]=-1; projection.m[14]=(nearPlane*farPlane)/(nearPlane-farPlane);
    Matrix4 result{}; for (int r=0;r<4;++r) for (int col=0;col<4;++col) for (int k=0;k<4;++k) result.m[r*4+col] += view.m[r*4+k]*projection.m[k*4+col]; viewProjection_=result;
}
