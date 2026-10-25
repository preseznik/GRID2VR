#pragma once
#include "replay_camera.h"
#include "replay_state.h"
#include "eye_projection.h"
#include "scene_pose.h"
#include <algorithm>
#include <limits>
#include <span>

namespace grid2vr {
using VisibilityVolume=std::array<uint8_t,224>;
struct VisibilityBox {
    std::array<float,3> center{};
    float radius{};
    alignas(16) std::array<float,16> matrix{};
};
// Enclose the native volume under any rotation about this camera. The extra
// eight native units allow head/eye translation; these units are not metres.
inline bool MakeVisibilityBox(const VisibilityVolume& volume,const SceneView& view,VisibilityBox& output) {
    std::array<float,56> data{}; memcpy(data.data(),volume.data(),volume.size());
    VisibilityBox candidate;
    memcpy(candidate.center.data(),view.data()+0x40,12);
    for (float value:candidate.center) if (!std::isfinite(value) || std::abs(value)>1e6f) return false;
    for (size_t i=0;i<6;++i) {
        double norm{};
        for (size_t j=0;j<4;++j) {
            const auto value=data[4*i+j];
            if (!std::isfinite(value)) return false;
            if (j<3) norm+=double(value)*value;
        }
        if (std::abs(norm-1)>1e-3) return false;
    }
    double distance{};
    for (size_t i=6;i<14;++i) {
        double squared{};
        for (size_t axis=0;axis<3;++axis) {
            const float value=data[4*i+axis];
            if (!std::isfinite(value) || std::abs(value)>1e6f) return false;
            const double delta=double(value)-candidate.center[axis]; squared+=delta*delta;
        }
        // Native corner W is unused and may retain previous stack bytes.
        for (size_t plane=0;plane<6;++plane) {
            double side=data[4*plane+3];
            for (size_t axis=0;axis<3;++axis) side+=double(data[4*plane+axis])*data[4*i+axis];
            if (side < -1) return false;
        }
        distance=(std::max)(distance,std::sqrt(squared));
    }
    if (distance<25 || distance>25000) return false;
    candidate.radius=std::nextafter(static_cast<float>(distance+8),std::numeric_limits<float>::infinity());
    for (size_t axis=0;axis<3;++axis) {
        const float scale=(axis==2?-1.f:1.f)/candidate.radius;
        candidate.matrix[axis*5]=scale;
        candidate.matrix[12+axis]=-candidate.center[axis]*scale;
    }
    candidate.matrix[15]=1;
    output=candidate; return true;
}
inline bool MakeEyeVisibilityBox(const VisibilityVolume& volume,const SceneView& visibilityView,
                                 const SceneView& renderView,std::span<const EyeFrustum> frusta,VisibilityBox& output) {
    VisibilityBox candidate;
    if (frusta.empty() || frusta.size()>2 || !MakeVisibilityBox(volume,visibilityView,candidate)) return false;
    std::array<float,56> data{};memcpy(data.data(),volume.data(),volume.size());
    double farDistance{},centerDistanceSquared{};
    for (size_t plane=0;plane<6;++plane) {
        double side=data[4*plane+3];
        for (size_t axis=0;axis<3;++axis) side+=double(data[4*plane+axis])*candidate.center[axis];
        farDistance=(std::max)(farDistance,side);
    }
    std::array<std::array<float,3>,3> basis{};
    constexpr size_t offsets[]{0x20,0x10,0x30};
    for (size_t row=0;row<3;++row) {
        memcpy(basis[row].data(),renderView.data()+offsets[row],12);
        for (float value:basis[row]) if (!std::isfinite(value)) return false;
        float position{};memcpy(&position,renderView.data()+0x40+4*row,4);
        if (!std::isfinite(position)) return false;
        const double delta=double(position)-candidate.center[row];centerDistanceSquared+=delta*delta;
    }
    // Bound the near-rigid native basis under any unit head rotation.
    if (!NativeBasisScaleSupported(basis)) return false;
    double maxRay{};
    for (const auto& eye:frusta) {
        for (float value:{eye.left,eye.right,eye.down,eye.up})
            if (!std::isfinite(value) || std::abs(value)>4) return false;
        if (eye.left>=0 || eye.right<=0 || eye.down>=0 || eye.up<=0) return false;
        const double x=(std::max)(std::abs(eye.left),std::abs(eye.right));
        const double y=(std::max)(std::abs(eye.down),std::abs(eye.up));
        maxRay=(std::max)(maxRay,std::sqrt(1+x*x+y*y));
    }
    // Use the same translation bound as PoseAxes, including the native basis
    // distortion bound rather than assuming a perfectly rigid camera.
    // Include differing visibility/render origins and the existing 8-unit margin.
    const double radius=std::sqrt(centerDistanceSquared)+(farDistance*maxRay+maxSceneTranslation)*std::sqrt(maxNativeBasisSquaredScale)+8;
    if (!std::isfinite(radius) || radius>25008 || farDistance<=0) return false;
    candidate.radius=(std::max)(candidate.radius,std::nextafter(static_cast<float>(radius),std::numeric_limits<float>::infinity()));
    for (size_t axis=0;axis<3;++axis) {
        const float scale=(axis==2?-1.f:1.f)/candidate.radius;
        candidate.matrix[axis*5]=scale;candidate.matrix[12+axis]=-candidate.center[axis]*scale;
    }
    output=candidate;return true;
}
inline bool ValidateVisibilityBox(const VisibilityVolume& volume,const VisibilityBox& box) {
    std::array<float,56> data{}; memcpy(data.data(),volume.data(),volume.size());
    for (float value:data) if (!std::isfinite(value)) return false;
    if (!std::isfinite(box.radius) || box.radius<25 || box.radius>25009) return false;
    // Check all eight distinct corners and six distinct inward axis planes.
    unsigned cornerMask{},planeMask{};
    for (size_t i=6;i<14;++i) {
        unsigned key{};
        for (size_t axis=0;axis<3;++axis) {
            const double delta=double(data[4*i+axis])-box.center[axis];
            if (std::abs(std::abs(delta)-box.radius)>.02) return false;
            if (delta>0) key|=1u<<axis;
        }
        if (cornerMask&(1u<<key)) return false;
        cornerMask|=1u<<key;
    }
    for (size_t i=0;i<6;++i) {
        unsigned key=6;
        for (unsigned axis=0;axis<3;++axis) {
            const float n=data[4*i+axis];
            if (std::abs(std::abs(n)-1)<1e-4) {
                if (key!=6) return false;
                key=2*axis+(n>0?1:0);
            } else if (std::abs(n)>1e-4) return false;
        }
        if (key==6 || (planeMask&(1u<<key))) return false;
        double side=data[4*i+3];
        for (size_t axis=0;axis<3;++axis) side+=double(data[4*i+axis])*box.center[axis];
        if (std::abs(side-box.radius)>.02) return false;
        planeMask|=1u<<key;
    }
    return cornerMask==255 && planeMask==63;
}
}
