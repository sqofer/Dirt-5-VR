#pragma once
#include <openxr/openxr.h>
#include <DirectXMath.h>
#include <cmath>
#include <stdexcept>
#include <initializer_list>

namespace vr_camera {
enum class Depth { Forward, Reverse };

// Row vectors, D3D clip depth [0,1], right-handed XR axes, -Z forward.
// Resolution and desktop aspect ratio deliberately do not enter the projection.
inline DirectX::XMMATRIX projection(const XrFovf& f, float nearZ, float farZ, Depth depth) {
    constexpr float halfPi=1.57079632679f;
    for(float angle:{f.angleLeft,f.angleRight,f.angleUp,f.angleDown})
        if(!std::isfinite(angle)||std::abs(angle)>=halfPi)throw std::invalid_argument("Invalid perspective FOV");
    if(!std::isfinite(nearZ)||nearZ<=0||!std::isfinite(farZ)||farZ<=nearZ)
        throw std::invalid_argument("Invalid near/far planes");
    float l=std::tan(f.angleLeft),r=std::tan(f.angleRight),b=std::tan(f.angleDown),t=std::tan(f.angleUp);
    if(!(r>l&&t>b))throw std::invalid_argument("Inverted perspective FOV");
    float a=depth==Depth::Reverse?nearZ/(farZ-nearZ):-farZ/(farZ-nearZ);
    float z=depth==Depth::Reverse?farZ*nearZ/(farZ-nearZ):-farZ*nearZ/(farZ-nearZ);
    return DirectX::XMMatrixSet(2/(r-l),0,0,0, 0,2/(t-b),0,0,
        (r+l)/(r-l),(t+b)/(t-b),a,-1, 0,0,z,0);
}

// Scale all XR translations in metres together, including runtime eye separation.
// No forced IPD, convergence distance or symmetric eye rotations.
// Coordinate/seat anchoring into DIRT5 remains the game adapter's responsibility.
inline DirectX::XMMATRIX world(const XrPosef& pose, float unitsPerMetre) {
    const auto& q=pose.orientation;const auto& p=pose.position;
    float norm=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    if(!std::isfinite(unitsPerMetre)||unitsPerMetre<=0||!std::isfinite(norm)||std::abs(norm-1)>0.01f||
       !std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z))
        throw std::invalid_argument("Invalid XR pose or metric scale");
    auto rotation=DirectX::XMQuaternionNormalize(DirectX::XMVectorSet(q.x,q.y,q.z,q.w));
    return DirectX::XMMatrixRotationQuaternion(rotation)*DirectX::XMMatrixTranslation(p.x*unitsPerMetre,p.y*unitsPerMetre,p.z*unitsPerMetre);
}
inline DirectX::XMMATRIX view(const XrPosef& pose, float unitsPerMetre) {
    return DirectX::XMMatrixInverse(nullptr,world(pose,unitsPerMetre));
}
} // namespace vr_camera
