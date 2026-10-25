#pragma once
#include "replay_camera.h"
#include "replay_state.h"
#include "eye_projection.h"

namespace grid2vr {
// Diagnostic separation in native world units. Metre scale is not yet proven.
constexpr float diagnosticEyeOffset=0.064f;
inline bool OffsetSceneView(const CameraContext& camera,const SceneView& source,
                            float distance,SceneView& result,float yaw=0) {
    if (!std::isfinite(distance) || std::abs(distance)>.1f || !std::isfinite(yaw) || std::abs(yaw)>1.571f) return false;
    std::array<float,3> right{},position{},nativeRight{},nativePosition{};
    memcpy(right.data(),camera.data()+0x70,12);
    memcpy(position.data(),camera.data()+0xa0,12);
    memcpy(nativeRight.data(),source.data()+0x20,12);
    memcpy(nativePosition.data(),source.data()+0x40,12);
    float length{};
    for (size_t i=0;i<3;++i) {
        if (!std::isfinite(right[i]) || !std::isfinite(position[i]) ||
            !std::isfinite(nativeRight[i]) || !std::isfinite(nativePosition[i]) ||
            std::abs(position[i])>100000 || std::abs(right[i]+nativeRight[i])>0.0001f ||
            std::abs(position[i]-nativePosition[i])>0.001f) return false;
        length+=right[i]*right[i];
    }
    if (std::abs(length-1.f)>0.0001f) return false;
    auto candidate=source;
    if (yaw!=0) {
        std::array<float,3> up{},forward{},nativeUp{},nativeBack{};
        memcpy(up.data(),source.data()+0x10,12); memcpy(forward.data(),source.data()+0x30,12);
        memcpy(nativeUp.data(),camera.data()+0x80,12); memcpy(nativeBack.data(),camera.data()+0x90,12);
        std::array<std::array<float,3>,3> basis{right,up,nativeBack};
        for (size_t row=0;row<3;++row) for (size_t col=0;col<3;++col) {
            float dot{};
            for (size_t i=0;i<3;++i) {
                if (!std::isfinite(up[i]) || !std::isfinite(forward[i]) || !std::isfinite(nativeUp[i]) || !std::isfinite(nativeBack[i]) ||
                    std::abs(up[i]-nativeUp[i])>.0001f || std::abs(forward[i]+nativeBack[i])>.0001f) return false;
                dot+=basis[row][i]*basis[col][i];
            }
            if (std::abs(dot-(row==col?1.f:0.f))>.0001f) return false;
        }
        const float c=static_cast<float>(std::cos(double(yaw))),s=static_cast<float>(std::sin(double(yaw)));
        for (size_t i=0;i<3;++i) {
            const float negativeRight=c*nativeRight[i]-s*forward[i];
            forward[i]=s*nativeRight[i]+c*forward[i];
            nativeRight[i]=negativeRight; right[i]=-negativeRight;
        }
        memcpy(candidate.data()+0x20,nativeRight.data(),12);
        memcpy(candidate.data()+0x30,forward.data(),12);
    }
    for (size_t i=0;i<3;++i) nativePosition[i]+=right[i]*distance;
    memcpy(candidate.data()+0x40,nativePosition.data(),12);
    result=candidate;
    return true;
}
// Translate the recorded previous camera, preserving its own rotation and FOV.
// Row-vector convention: eyeVP = T(-worldDelta) * centerVP. Use the rounded
// native position delta, just as OffsetSceneView does for the current view.
inline bool PrepareEyeHistory(const CameraContext& previous,const SceneView& previousView,
                              const HistoryState& entry,float distance,HistoryState& prepared,float yaw=0,const EyeFrustum* frustum=nullptr) {
    SceneView shifted{};
    if (memcmp(previous.data()+0x190,entry.data()+0x10,64) ||
        !OffsetSceneView(previous,previousView,distance,shifted,yaw)) return false;
    std::array<float,16> vp{};
    std::array<float,3> source{},target{};
    auto projected=previous;
    if (frustum && !ApplyEyeFrustum(projected,*frustum)) return false;
    memcpy(vp.data(),projected.data()+0x190,64);
    memcpy(source.data(),previousView.data()+0x40,12);
    memcpy(target.data(),shifted.data()+0x40,12);
    for (float value:vp) if (!std::isfinite(value)) return false;
    if (yaw!=0) {
        // Bootstrap only: move the preceding center V by the rounded world delta,
        // then rotate into the local eye axes. Subsequent pairs use the actual
        // preceding rendered VP through RenderedEyeHistory below.
        std::array<float,16> v{},p{};
        memcpy(v.data(),projected.data()+0x150,64); memcpy(p.data(),projected.data()+0x110,64);
        std::array<double,16> moved{},rotated{};
        for (size_t i=0;i<16;++i) {
            if (!std::isfinite(v[i]) || !std::isfinite(p[i])) return false;
            moved[i]=v[i];
        }
        for (size_t column=0;column<4;++column)
            for (size_t row=0;row<3;++row) moved[12+column]-=(double(target[row])-source[row])*v[row*4+column];
        const double c=std::cos(double(yaw)),s=std::sin(double(yaw));
        for (size_t row=0;row<4;++row) {
            rotated[row*4]=c*moved[row*4]-s*moved[row*4+2];
            rotated[row*4+1]=moved[row*4+1];
            rotated[row*4+2]=s*moved[row*4]+c*moved[row*4+2];
            rotated[row*4+3]=moved[row*4+3];
        }
        for (size_t row=0;row<4;++row) for (size_t column=0;column<4;++column) {
            double value{};
            for (size_t k=0;k<4;++k) value+=rotated[row*4+k]*p[k*4+column];
            vp[row*4+column]=static_cast<float>(value);
            if (!std::isfinite(vp[row*4+column])) return false;
        }
    } else for (size_t column=0;column<4;++column) {
        double translated=vp[12+column];
        for (size_t row=0;row<3;++row)
            translated-=(double(target[row])-source[row])*vp[row*4+column];
        vp[12+column]=static_cast<float>(translated);
        if (!std::isfinite(vp[12+column])) return false;
    }
    memcpy(prepared.data()+0x50,vp.data(),64);
    return true;
}
struct RenderedEyeHistory {
    uint64_t frame{};
    uintptr_t context{},manager{};
    CameraContext camera{};
    bool Store(uint64_t nextFrame,uintptr_t nextContext,uintptr_t nextManager,const CameraContext& next) {
        if (!nextFrame || !nextContext || !nextManager || (frame && (nextFrame!=frame+1 || nextContext!=context || nextManager!=manager))) return false;
        std::array<float,16> vp{}; memcpy(vp.data(),next.data()+0x190,64);
        for (float value:vp) if (!std::isfinite(value)) return false;
        frame=nextFrame; context=nextContext; manager=nextManager; camera=next; return true;
    }
    bool Seed(uint64_t nextFrame,uintptr_t nextContext,uintptr_t nextManager,HistoryState& prepared) const {
        if (!frame || nextFrame!=frame+1 || nextContext!=context || nextManager!=manager) return false;
        memcpy(prepared.data()+0x50,camera.data()+0x190,64); return true;
    }
};
using NativeCameraFn=void (__thiscall*)(void*,uintptr_t,uintptr_t,float,uint32_t);
inline void BuildNativeCamera(NativeCameraFn function,void* main,uintptr_t context,
                              const SceneView& view,float farPlane) {
    function(main,context,reinterpret_cast<uintptr_t>(view.data()),farPlane,3);
}
}
