#include "model_camera.h"

#include <algorithm>
#include <cmath>
#include <windows.h>

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
float Lerp(float start, float target, float progress) { return start + (target - start) * progress; }
Float3 Lerp(Float3 start, Float3 target, float progress) { return Add(Mul(start, 1.0f - progress), Mul(target, progress)); }
Float3 CanonicalForward(Float3 upAxis) {
    if (std::fabs(upAxis.z) > .9f) return { 0.0f, 1.0f, 0.0f };
    return { 0.0f, 0.0f, -1.0f };
}
struct Quaternion { float x, y, z, w; };
Quaternion Normalize(Quaternion value) {
    const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w);
    return length > 1e-12f ? Quaternion{ value.x / length, value.y / length, value.z / length, value.w / length } : Quaternion{ 0, 0, 0, 1 };
}
Quaternion OrientationQuaternion(Float3 forward, Float3 up) {
    // The renderer's view matrix stores a reflected horizontal axis, so do not feed that
    // basis directly to quaternion math. Build the equivalent proper rotation from the
    // camera's forward/up axes; SetLocalOrientation restores the renderer's convention.
    const Float3 right = Normalize(Cross(forward, up));
    up = Normalize(Cross(right, forward));
    const float m00 = right.x, m01 = up.x, m02 = -forward.x;
    const float m10 = right.y, m11 = up.y, m12 = -forward.y;
    const float m20 = right.z, m21 = up.z, m22 = -forward.z;
    const float trace = m00 + m11 + m22;
    Quaternion result{};
    if (trace > 0.0f) { const float scale = std::sqrt(trace + 1.0f) * 2.0f; result = { (m21 - m12) / scale, (m02 - m20) / scale, (m10 - m01) / scale, 0.25f * scale }; }
    else if (m00 > m11 && m00 > m22) { const float scale = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f; result = { 0.25f * scale, (m01 + m10) / scale, (m02 + m20) / scale, (m21 - m12) / scale }; }
    else if (m11 > m22) { const float scale = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f; result = { (m01 + m10) / scale, 0.25f * scale, (m12 + m21) / scale, (m02 - m20) / scale }; }
    else { const float scale = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f; result = { (m02 + m20) / scale, (m12 + m21) / scale, 0.25f * scale, (m10 - m01) / scale }; }
    return Normalize(result);
}
Quaternion Slerp(Quaternion start, Quaternion target, float progress) {
    float dot = start.x * target.x + start.y * target.y + start.z * target.z + start.w * target.w;
    if (dot < 0.0f) { target = { -target.x, -target.y, -target.z, -target.w }; dot = -dot; }
    if (dot > 0.9995f) return Normalize({ Lerp(start.x, target.x, progress), Lerp(start.y, target.y, progress), Lerp(start.z, target.z, progress), Lerp(start.w, target.w, progress) });
    const float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
    const float sinAngle = std::sin(angle);
    return Normalize({ std::sin((1.0f - progress) * angle) / sinAngle * start.x + std::sin(progress * angle) / sinAngle * target.x,
        std::sin((1.0f - progress) * angle) / sinAngle * start.y + std::sin(progress * angle) / sinAngle * target.y,
        std::sin((1.0f - progress) * angle) / sinAngle * start.z + std::sin(progress * angle) / sinAngle * target.z,
        std::sin((1.0f - progress) * angle) / sinAngle * start.w + std::sin(progress * angle) / sinAngle * target.w });
}
void QuaternionOrientation(Quaternion rotation, Float3& forward, Float3& up) {
    rotation = Normalize(rotation);
    const float xx = rotation.x * rotation.x, yy = rotation.y * rotation.y, zz = rotation.z * rotation.z;
    const float xy = rotation.x * rotation.y, xz = rotation.x * rotation.z, yz = rotation.y * rotation.z;
    const float xw = rotation.x * rotation.w, yw = rotation.y * rotation.w, zw = rotation.z * rotation.w;
    up = Normalize(Float3{ 2.0f * (xy - zw), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + xw) });
    forward = Normalize(Float3{ -2.0f * (xz + yw), -2.0f * (yz - xw), -(1.0f - 2.0f * (xx + yy)) });
}
#if defined(_DEBUG)
Float3 AnimationEye(const OrbitCamera::AnimationState& state) {
    const Float3 right = Normalize(Cross(state.forward, state.up));
    const Float3 up = Normalize(Cross(right, state.forward));
    return Add(Add(Sub(state.pivot, Mul(state.forward, state.distance)), Mul(right, state.framingRight)), Mul(up, state.framingUp));
}
void TraceAnimationState(const wchar_t* label, const OrbitCamera::AnimationState& state, Quaternion orientation) {
    const Float3 eye = AnimationEye(state);
    wchar_t message[512]{};
    swprintf_s(message, L"Viewtrious animated Home %s eye=(%.5f,%.5f,%.5f) forward=(%.5f,%.5f,%.5f) up=(%.5f,%.5f,%.5f) pivot=(%.5f,%.5f,%.5f) frame=(%.5f,%.5f) distance=%.5f q=(%.5f,%.5f,%.5f,%.5f)\\n",
        label, eye.x, eye.y, eye.z, state.forward.x, state.forward.y, state.forward.z, state.up.x, state.up.y, state.up.z,
        state.pivot.x, state.pivot.y, state.pivot.z, state.framingRight, state.framingUp, state.distance, orientation.x, orientation.y, orientation.z, orientation.w);
    OutputDebugStringW(message);
}
void TraceHomeBasis(const OrbitCamera& camera, Float3 upAxis) {
    const OrbitCamera::State state = camera.NavLibState();
    const Float3 forward = Normalize(state.forward), right = Normalize(Cross(forward, state.up)), up = Normalize(Cross(right, forward));
    const Matrix4& matrix = camera.ViewProjection();
    const auto project = [&matrix](Float3 point) {
        const float x=point.x*matrix.m[0]+point.y*matrix.m[4]+point.z*matrix.m[8]+matrix.m[12];
        const float y=point.x*matrix.m[1]+point.y*matrix.m[5]+point.z*matrix.m[9]+matrix.m[13];
        const float w=point.x*matrix.m[3]+point.y*matrix.m[7]+point.z*matrix.m[11]+matrix.m[15];
        return Float3{ x/w, y/w, w };
    };
    const Float3 center = camera.Pivot(); const float probe = std::max(camera.Radius()*.1f, 1e-4f);
    const Float3 origin = project(center), plusX = project(Add(center,{probe,0,0})), plusZ = project(Add(center,{0,0,probe}));
    wchar_t message[1024]{};
    swprintf_s(message,L"Viewtrious Home basis: preferredUp=(%.0f,%.0f,%.0f) eye=(%.5f,%.5f,%.5f) forward=(%.5f,%.5f,%.5f) up=(%.5f,%.5f,%.5f) right=(%.5f,%.5f,%.5f) handedness=%.6f ndcCenter=(%.5f,%.5f) plusX=(%.5f,%.5f) plusZ=(%.5f,%.5f)\\n",upAxis.x,upAxis.y,upAxis.z,state.position.x,state.position.y,state.position.z,forward.x,forward.y,forward.z,up.x,up.y,up.z,right.x,right.y,right.z,Dot(Cross(right,up),Mul(forward,-1)),origin.x,origin.y,plusX.x,plusX.y,plusZ.x,plusZ.y);
    OutputDebugStringW(message);
}
#endif
}

void OrbitCamera::Fit(const ModelBounds& bounds, float aspectRatio, Float3 upAxis) {
    navLibStateActive_ = false;
    pivot_ = Mul(Add(bounds.minimum, bounds.maximum), 0.5f);
    framingRight_ = framingUp_ = 0.0f;
    const Float3 diagonal = Sub(bounds.maximum, bounds.minimum);
    radius_ = std::max(1e-5f, 0.5f * std::sqrt(Dot(diagonal, diagonal)));
    aspect_ = std::max(0.01f, aspectRatio);
    distance_ = std::max(radius_ * 2.8f, radius_ / std::tan(fieldOfView_ * 0.5f));
    orthographicHalfHeight_ = std::max(radius_ * 1.2f, 1e-5f);
    upAxis = Normalize(upAxis);
    const Float3 forwardBasis = CanonicalForward(upAxis);
    const Float3 right = Normalize(Cross(forwardBasis, upAxis));
    const float yaw = 0.62f, pitch = -0.42f, cosPitch = std::cos(pitch);
    const Float3 eye = Add(pivot_, Add(Mul(right, distance_ * std::sin(yaw) * cosPitch), Add(Mul(upAxis, -distance_ * std::sin(pitch)), Mul(forwardBasis, -distance_ * std::cos(yaw) * cosPitch))));
    SetLocalOrientation(Normalize(Sub(pivot_, eye)), upAxis);
    Update();
#if defined(_DEBUG)
    TraceHomeBasis(*this, upAxis);
#endif
}
void OrbitCamera::SetAspectRatio(float aspectRatio) { aspect_ = std::max(0.01f, aspectRatio); Update(); }
void OrbitCamera::Orbit(float dx, float dy) {
    MaterializeNavLibState();
    // Rotate about the live view axes, rather than a bounded world-up Euler pitch.
    // This remains continuous through every pole and when returning from SpaceMouse.
    forward_ = Normalize(RotateAroundAxis(forward_, up_, dx));
    const Float3 right = Normalize(Cross(forward_, up_));
    forward_ = Normalize(RotateAroundAxis(forward_, right, dy));
    up_ = Normalize(RotateAroundAxis(up_, right, dy));
    SetLocalOrientation(forward_, up_);
    Update();
}
void OrbitCamera::Pan(float dx, float dy) {
    MaterializeNavLibState();
    framingRight_ += dx * distance_;
    framingUp_ += dy * distance_;
    Update();
}
void OrbitCamera::PanPixels(float dx, float dy, unsigned int viewportWidth, unsigned int viewportHeight) {
    MaterializeNavLibState();
    const float halfHeight = ViewHalfHeight();
    framingRight_ += dx * (2.0f * halfHeight * aspect_ / std::max(1u, viewportWidth));
    framingUp_ += dy * (2.0f * halfHeight / std::max(1u, viewportHeight));
    Update();
}
void OrbitCamera::Dolly(float wheelUnits) { MaterializeNavLibState(); if(projectionMode_==ModelProjectionMode::Orthographic) orthographicHalfHeight_=std::clamp(orthographicHalfHeight_*std::exp(-wheelUnits*.14f),radius_*.0002f,radius_*10000.0f); else distance_=std::clamp(distance_*std::exp(-wheelUnits*.14f),radius_*.02f,radius_*10000.0f); Update(); }
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
void OrbitCamera::SetProjectionMode(ModelProjectionMode mode) { if(projectionMode_==mode)return; MaterializeNavLibState(); const float halfHeight=ViewHalfHeight(); projectionMode_=mode; if(mode==ModelProjectionMode::Orthographic) orthographicHalfHeight_=halfHeight; else distance_=std::clamp(halfHeight/std::tan(fieldOfView_*.5f),radius_*.02f,radius_*10000.0f); Update(); }
void OrbitCamera::SetOrthographicHalfHeight(float halfHeight) { if(projectionMode_!=ModelProjectionMode::Orthographic||!std::isfinite(halfHeight))return; orthographicHalfHeight_=std::clamp(halfHeight,radius_*.0002f,radius_*10000.0f); Update(); }
bool OrbitCamera::SetFromNavLibState(const State& state) {
    if (!std::isfinite(state.position.x) || !std::isfinite(state.position.y) || !std::isfinite(state.position.z) ||
        !std::isfinite(state.forward.x) || !std::isfinite(state.forward.y) || !std::isfinite(state.forward.z) ||
        !std::isfinite(state.up.x) || !std::isfinite(state.up.y) || !std::isfinite(state.up.z) ||
        std::sqrt(Dot(state.forward, state.forward)) <= 1e-8f || std::sqrt(Dot(state.up, state.up)) <= 1e-8f) return false;
    // NavLib camera matrices are absolute. Perspective retains the validated exact-pose
    // contract. Orthographic retains its authoritative extent: forward eye travel in a
    // NavLib matrix is an internal bridge detail, while explicit view-extents updates own zoom.
    if (projectionMode_ == ModelProjectionMode::Orthographic) {
        const State current = NavLibState(); const Float3 forward = Normalize(state.forward);
        const Float3 right = Normalize(Cross(forward, state.up)), up = Normalize(Cross(right, forward));
        const Float3 delta = Sub(state.position, current.position);
#if defined(_DEBUG)
        const float previousExtent = orthographicHalfHeight_;
        const float forwardTravel = Dot(delta, forward);
#endif
        navLibState_ = { Add(current.position, Add(Mul(right, Dot(delta, right)), Mul(up, Dot(delta, up)))), forward, up };
#if defined(_DEBUG)
        wchar_t message[448]{};
        swprintf_s(message, L"Viewtrious orthographic NavLib matrix: eye=(%.4f,%.4f,%.4f) forwardTravel=%.6f lateral=(%.6f,%.6f) rotation=%d extent=%.6f->%.6f\\n", state.position.x, state.position.y, state.position.z, forwardTravel, Dot(delta, right), Dot(delta, up), Dot(current.forward, forward) < 0.999999f || Dot(current.up, up) < 0.999999f, previousExtent, orthographicHalfHeight_);
        OutputDebugStringW(message);
#endif
    } else navLibState_ = { state.position, Normalize(state.forward), Normalize(state.up) };
    navLibStateActive_ = true;
    Update();
    return true;
}
bool OrbitCamera::SetCameraTargetFromNavLib(Float3 target) {
    if (!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z)) return false;
    const State current = NavLibState();
    const Float3 forward = Normalize(current.forward);
    const Float3 right = Normalize(Cross(forward, current.up));
    const Float3 up = Normalize(Cross(right, forward));
    const Float3 targetOffset = Sub(target, CameraTarget());
    const Float3 pan = Add(Mul(right, Dot(targetOffset, right)), Mul(up, Dot(targetOffset, up)));
    if (!std::isfinite(pan.x) || !std::isfinite(pan.y) || !std::isfinite(pan.z)) return false;

    // Camera-target requests carry framing translation. The orbit pivot remains attached
    // to the model, while the camera/view frame moves in its current right/up plane.
    if (navLibStateActive_) navLibState_.position = Add(navLibState_.position, pan);
    else { framingRight_ += Dot(pan, right); framingUp_ += Dot(pan, up); }
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
OrbitCamera::AnimationState OrbitCamera::CaptureAnimationState() {
    MaterializeNavLibState();
    return { pivot_, forward_, up_, framingRight_, framingUp_, distance_, radius_, aspect_, fieldOfView_, orthographicHalfHeight_, projectionMode_ };
}
void OrbitCamera::ApplyInterpolatedAnimationState(const AnimationState& start, const AnimationState& target, float progress) {
    progress = std::clamp(progress, 0.0f, 1.0f);
    const Quaternion startOrientation = OrientationQuaternion(start.forward, start.up);
    const Quaternion targetOrientation = OrientationQuaternion(target.forward, target.up);
    Float3 forward{}, up{};
    const Quaternion orientation = Slerp(startOrientation, targetOrientation, progress);
    QuaternionOrientation(orientation, forward, up);
    navLibStateActive_ = false;
    pivot_ = Lerp(start.pivot, target.pivot, progress);
    framingRight_ = Lerp(start.framingRight, target.framingRight, progress);
    framingUp_ = Lerp(start.framingUp, target.framingUp, progress);
    distance_ = Lerp(start.distance, target.distance, progress);
    radius_ = Lerp(start.radius, target.radius, progress);
    aspect_ = Lerp(start.aspect, target.aspect, progress);
    fieldOfView_ = Lerp(start.fieldOfView, target.fieldOfView, progress);
    orthographicHalfHeight_ = Lerp(start.orthographicHalfHeight, target.orthographicHalfHeight, progress);
    projectionMode_ = start.projectionMode;
    SetLocalOrientation(forward, up);
    Update();
#if defined(_DEBUG)
    if (progress <= 0.1f) {
        TraceAnimationState(L"start", start, startOrientation);
        TraceAnimationState(L"target", target, targetOrientation);
        AnimationState atZero = start;
        QuaternionOrientation(Slerp(startOrientation, targetOrientation, 0.0f), atZero.forward, atZero.up);
        TraceAnimationState(L"applied t=0", atZero, startOrientation);
        TraceAnimationState(L"first nonzero frame", CaptureAnimationState(), orientation);
    }
#endif
}
Float3 OrbitCamera::Position() const {
    if (navLibStateActive_) return navLibState_.position;
    return Add(Sub(pivot_, Mul(forward_, distance_)), LocalFramingOffset());
}
Float3 OrbitCamera::Pivot() const {
    return pivot_;
}
Float3 OrbitCamera::CameraTarget() const {
    if (navLibStateActive_) return Add(navLibState_.position, Mul(Normalize(navLibState_.forward), Distance()));
    return Add(pivot_, LocalFramingOffset());
}
float OrbitCamera::Distance() const {
    if (!navLibStateActive_) return distance_;
    const Float3 eyeToPivot = Sub(pivot_, navLibState_.position);
    const float distance = Dot(eyeToPivot, Normalize(navLibState_.forward));
    return std::isfinite(distance) && distance > 1e-8f ? distance : distance_;
}
float OrbitCamera::ViewHalfHeight() const { return projectionMode_==ModelProjectionMode::Orthographic ? orthographicHalfHeight_ : Distance()*std::tan(fieldOfView_*.5f); }
OrbitCamera::ClipPlanes OrbitCamera::CurrentClipPlanes() const {
    const float distance = Distance();
    const float nearPlane = std::max(radius_ * 0.001f, distance * 0.001f);
    return { nearPlane, std::max(nearPlane * 2.0f, distance + radius_ * 8.0f) };
}
void OrbitCamera::MaterializeNavLibState() {
    if (!navLibStateActive_) return;
    // Preserve the established live rotation centre while returning to local orbit
    // controls. It must never be reconstructed from the last camera eye.
    const Float3 forward = Normalize(navLibState_.forward);
    SetLocalOrientation(forward, navLibState_.up);
    const float focusDistance = Dot(Sub(pivot_, navLibState_.position), forward_);
    if (std::isfinite(focusDistance) && focusDistance > 1e-8f) distance_ = focusDistance;
    const Float3 framing = Sub(navLibState_.position, Sub(pivot_, Mul(forward_, distance_)));
    const Float3 right = Normalize(Cross(forward_, up_));
    const Float3 up = Normalize(Cross(right, forward_));
    framingRight_ = Dot(framing, right);
    framingUp_ = Dot(framing, up);
    navLibStateActive_ = false;
}
void OrbitCamera::SetLocalOrientation(Float3 forward, Float3 up) {
    forward_ = Normalize(forward);
    Float3 right = Cross(forward_, up);
    if (Dot(right, right) <= 1e-12f) {
        const Float3 fallback = std::fabs(forward_.y) < 0.9f ? Float3{ 0.0f, 1.0f, 0.0f } : Float3{ 1.0f, 0.0f, 0.0f };
        right = Cross(forward_, fallback);
    }
    right = Normalize(right);
    up_ = Normalize(Cross(right, forward_));
}
Float3 OrbitCamera::LocalFramingOffset() const {
    const Float3 right = Normalize(Cross(forward_, up_));
    const Float3 up = Normalize(Cross(right, forward_));
    return Add(Mul(right, framingRight_), Mul(up, framingUp_));
}
const Matrix4& OrbitCamera::ViewProjection() const { return viewProjection_; }
void OrbitCamera::Update() {
    const Float3 eye = Position();
    const Float3 forward = navLibStateActive_ ? Normalize(navLibState_.forward) : forward_;
    Float3 right{}; Float3 up{};
    if (navLibStateActive_) {
        right = Normalize(Cross(forward, navLibState_.up));
        up = Normalize(Cross(right, forward));
    } else {
        right = Normalize(Cross(forward, up_));
        up = Normalize(Cross(right, forward));
    }
    Matrix4 view{}; view.m[0]=right.x; view.m[4]=right.y; view.m[8]=right.z; view.m[1]=up.x; view.m[5]=up.y; view.m[9]=up.z; view.m[2]=-forward.x; view.m[6]=-forward.y; view.m[10]=-forward.z; view.m[12]=-Dot(right,eye); view.m[13]=-Dot(up,eye); view.m[14]=Dot(forward,eye); view.m[15]=1;
    const ClipPlanes clips = CurrentClipPlanes(); const float nearPlane = clips.nearPlane, farPlane = clips.farPlane; Matrix4 projection{};
    if(projectionMode_==ModelProjectionMode::Orthographic){const float halfHeight=std::max(orthographicHalfHeight_,1e-5f),halfWidth=halfHeight*aspect_;projection.m[0]=1.0f/halfWidth;projection.m[5]=1.0f/halfHeight;projection.m[10]=1.0f/(nearPlane-farPlane);projection.m[14]=nearPlane/(nearPlane-farPlane);projection.m[15]=1.0f;}
    else {const float f=1.0f/std::tan(fieldOfView_*.5f);projection.m[0]=f/aspect_;projection.m[5]=f;projection.m[10]=farPlane/(nearPlane-farPlane);projection.m[11]=-1;projection.m[14]=(nearPlane*farPlane)/(nearPlane-farPlane);}
    Matrix4 result{}; for (int r=0;r<4;++r) for (int col=0;col<4;++col) for (int k=0;k<4;++k) result.m[r*4+col] += view.m[r*4+k]*projection.m[k*4+col]; viewProjection_=result;
}
