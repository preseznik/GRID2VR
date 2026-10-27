#pragma once
#include "chase_camera_behavior.h"
#include "guarded_hooks.h"

namespace grid2vr::chasecamera {
constexpr uintptr_t collisionTargetCallRva=0x1f84f1,collisionTargetReturnRva=0x1f84f6;
constexpr uintptr_t collisionTargetRoutineRva=0x18f8a0;
struct CollisionGuard { uintptr_t rva;std::array<uint8_t,64> bytes;size_t size; };
inline constexpr std::array<CollisionGuard,5> collisionGuards{{
    {0x18f8a0,{0x53,0x8b,0xdc,0x83,0xec,0x08,0x83,0xe4,0xf0,0x83,0xc4,0x04,0x55,0x8b,0x6b,0x04,
        0x89,0x6c,0x24,0x04,0x8b,0xec,0x83,0xec,0x6c,0x56,0x8b,0xf1,0x8b,0x4e,0x60,0x85,
        0xc9,0x0f,0x84,0x4d,0x01,0x00,0x00,0x8b,0x01,0x8b,0x40,0x14,0x8d,0x55,0xd0,0x52,
        0xff,0xd0,0x8b,0x4e,0x60,0x8b,0x11,0x8b,0x52,0x1c,0x8d,0x45,0xa0,0x50,0xff,0xd2},64},
    {0x18fa05,{0xc5,0xf8,0x29,0x40,0x30,0x5e,0x8b,0xe5,0x5d,0x8b,0xe3,0x5b,0xc2,0x08,0x00},15},
    {0x1f9a53,{0xc5,0xf8,0x29,0x4e,0x10,0x83,0xbf,0xd8,0x24,0x00,0x00,0x00,0x0f,0x85,0x14,0x01,0x00,0x00},18},
    {0x1f9b08,{0xc5,0xf8,0x28,0x4d,0xb0,0xc5,0xf8,0x58,0x45,0xd0},10},
    {0x1f9b81,{0xc5,0xf8,0x28,0x45,0xd0,0xc5,0xf8,0x5c,0xf9,0xc5,0xf8,0x28,0xcf,0xc5,0xf8,0x28,0xc7,0xe8,0xa9,0x52,0x62,0x00},22}
}};
inline bool CollisionGuardsMatch(uintptr_t base) {
    for(const auto& guard:collisionGuards)
        if(!MatchesInstructions(reinterpret_cast<void*>(base+guard.rva),{guard.bytes.data(),guard.size}))return false;
    return true;
}

// Apply the exact same transform to native aim target T and requested position
// C: (T'-C') = scale * rotation * (T-C). A collision may shorten C' without
// bringing back the old unrotated aim. Only caller-owned target xyz is changed;
// native collision displacement, target height smoothing and roll remain native.
struct CollisionTargetTransform {
    ChaseVector pivot;
    ChaseQuaternion rotation;
    float scale{1};
    bool Apply(std::array<float,4>& target) const {
        for(float value:target)if(!std::isfinite(value))return false;
        if(!std::isfinite(pivot.x) || !std::isfinite(pivot.y) || !std::isfinite(pivot.z) ||
           !std::isfinite(scale) || scale<.6f || scale>10)return false;
        const float norm=rotation.x*rotation.x+rotation.y*rotation.y+rotation.z*rotation.z+rotation.w*rotation.w;
        if(!std::isfinite(norm) || std::abs(norm-1.f)>.0001f)return false;
        ChaseVector relative{target[0]-pivot.x,target[1]-pivot.y,target[2]-pivot.z};
        const float radius=relative.x*relative.x+relative.y*relative.y+relative.z*relative.z;
        if(!std::isfinite(radius) || radius>3600)return false;
        if(rotation.x==0 && rotation.y==0 && rotation.z==0 && rotation.w==1 && scale==1)return true;
        relative=ChaseRotate(rotation,relative);
        const std::array<float,3> result{pivot.x+relative.x*scale,pivot.y+relative.y*scale,pivot.z+relative.z*scale};
        for(float value:result)if(!std::isfinite(value))return false;
        std::copy(result.begin(),result.end(),target.begin());return true;
    }
};
}
