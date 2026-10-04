#pragma once
#include "vr_camera.h"
#include <array>
#include <cstddef>

namespace dirt5_camera {
using namespace DirectX;

// The inspected engine stores rigid world poses with _44 == 0. DirectXMath
// needs the homogeneous affine form. This helper never changes game memory.
inline XMMATRIX affineSeat(const XMFLOAT4X4& native) {
    for(int r=0;r<4;++r)for(int c=0;c<4;++c)
        if(!std::isfinite(native.m[r][c]))throw std::invalid_argument("Invalid native seat matrix");
    for(int r=0;r<3;++r){
        if(std::abs(native.m[r][3])>0.0001f)throw std::invalid_argument("Seat matrix contains perspective");
        for(int s=0;s<3;++s){
            float dot=0;for(int c=0;c<3;++c)dot+=native.m[r][c]*native.m[s][c];
            if(std::abs(dot-(r==s?1.f:0.f))>0.001f)throw std::invalid_argument("Seat matrix is not rigid");
        }
    }
    if(std::abs(native._44)>0.0001f&&std::abs(native._44-1)>0.0001f)
        throw std::invalid_argument("Unknown native homogeneous convention");
    auto copy=native;copy._44=1;
    auto matrix=XMLoadFloat4x4(&copy);
    if(std::abs(XMVectorGetX(XMMatrixDeterminant(matrix))-1)>0.002f)
        throw std::invalid_argument("Native seat axes are reflected");
    return matrix;
}

struct EyeMatrices {
    XMFLOAT4X4 worldAffine;
    XMFLOAT4X4 worldNative;
    XMFLOAT4X4 view;
    XMFLOAT4X4 projection;
};

// Verified copy at ViewImpl vtable slot 4 (RVA 1b1760 -> 183f30):
// four matrices are copied to ViewImpl+0x200, then derived data are rebuilt.
// The layout does not establish a valid engine invocation point.
// Renderer thread/phase, lifetime and repeatability remain
// unverified. No engine functions or addresses are invoked by this header.
struct alignas(16) NativeViewMatrices {
    XMFLOAT4X4 currentWorld;
    XMFLOAT4X4 currentProjection;
    XMFLOAT4X4 previousWorld;
    XMFLOAT4X4 previousProjection;
};
static_assert(sizeof(NativeViewMatrices)==0x100);
static_assert(offsetof(NativeViewMatrices,currentProjection)==0x40);
static_assert(offsetof(NativeViewMatrices,previousWorld)==0x80);
static_assert(offsetof(NativeViewMatrices,previousProjection)==0xc0);

// Previous matrices belong to the SAME eye. A failed/incomplete pair does not
// advance either history. The adapter must call commit only after completing
// both renders and their GPU work and successfully submitting the XR pair.
class StereoHistory {
    std::array<EyeMatrices,2> eyes_{};
    bool valid_=false;
public:
    NativeViewMatrices prepare(unsigned eye,const EyeMatrices& current) const {
        if(eye>=eyes_.size())throw std::invalid_argument("Invalid stereo eye");
        const auto& previous=valid_?eyes_[eye]:current;
        return {current.worldNative,current.projection,previous.worldNative,previous.projection};
    }
    void commit(const std::array<EyeMatrices,2>& completedPair) {eyes_=completedPair;valid_=true;}
    void reset() {valid_=false;}
};

// locatedEye and referenceHead are poses in the SAME OpenXR reference space.
// referenceHead is the centre-head pose at recenter, not an eye pose.
// Per-eye orientation/translation/FOV come directly from xrLocateViews.
// unitsPerMetre must be validated by the game adapter; no headset-specific
// IPD, desktop aspect, default world scale, or convergence adjustment is used.
inline EyeMatrices build(const XMFLOAT4X4& nativeSeat, const XrPosef& referenceHead,
                         const XrView& locatedEye, float unitsPerMetre,
                         float nativeNearZ, float nativeFarZ) {
    auto seat=affineSeat(nativeSeat);
    auto relativeEye=vr_camera::world(locatedEye.pose,unitsPerMetre)*
        vr_camera::view(referenceHead,unitsPerMetre);
    auto eyeWorld=relativeEye*seat;
    EyeMatrices out{};
    XMStoreFloat4x4(&out.worldAffine,eyeWorld);
    out.worldNative=out.worldAffine;out.worldNative._44=0;
    XMStoreFloat4x4(&out.view,XMMatrixInverse(nullptr,eyeWorld));
    XMStoreFloat4x4(&out.projection,vr_camera::projection(locatedEye.fov,nativeNearZ,
        nativeFarZ,vr_camera::Depth::Reverse));
    return out;
}
} // namespace dirt5_camera
