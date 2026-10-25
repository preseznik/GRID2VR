#pragma once
#include "replay_camera.h"

namespace grid2vr {
struct EyeFrustum { float left,right,down,up; }; // Tangents, in eye coordinates.
constexpr EyeFrustum diagnosticFrustum{-1.f,.8f,-.7f,.6f};
inline bool ApplyEyeFrustum(CameraContext& camera,const EyeFrustum& fov) {
    for (float value:{fov.left,fov.right,fov.down,fov.up})
        if (!std::isfinite(value) || std::abs(value)>4) return false;
    if (fov.left>=0 || fov.right<=0 || fov.down>=0 || fov.up<=0 ||
        fov.right-fov.left<.1f || fov.up-fov.down<.1f) return false;
    std::array<float,16> p{},v{},vp{};
    memcpy(p.data(),camera.data()+0x110,64); memcpy(v.data(),camera.data()+0x150,64);
    memcpy(vp.data(),camera.data()+0x190,64);
    for (size_t i=0;i<16;++i)
        if (!std::isfinite(p[i]) || !std::isfinite(v[i]) || !std::isfinite(vp[i])) return false;
    if (p[0]<=0 || p[5]<=0 || p[11]!=-1 || p[15]!=0 || p[10]>=-1 || p[14]>=0) return false;
    for (unsigned i:{1u,2u,3u,4u,6u,7u,12u,13u}) if (p[i]!=0) return false;
    p[0]=2/(fov.right-fov.left); p[5]=2/(fov.up-fov.down);
    p[8]=(fov.right+fov.left)/(fov.right-fov.left);
    p[9]=(fov.up+fov.down)/(fov.up-fov.down);
    // A lateral FOV change affects only X/Y. Keep native clip Z/W bit-exact:
    // recomputing those unchanged columns in double rounds differently from
    // the native float builder (including retained practice camera fixtures).
    for (size_t row=0;row<4;++row) for (size_t col=0;col<2;++col) {
        double value{};
        for (size_t k=0;k<4;++k) value+=double(v[4*row+k])*p[4*k+col];
        vp[4*row+col]=static_cast<float>(value);
        if (!std::isfinite(vp[4*row+col])) return false;
    }
    memcpy(camera.data()+0x110,p.data(),64); memcpy(camera.data()+0x190,vp.data(),64);
    return true;
}
}
