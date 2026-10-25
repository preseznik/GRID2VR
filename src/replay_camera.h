#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <cmath>

namespace grid2vr {
using CameraContext=std::array<uint8_t,0x200>;
struct ClipTransitionTrigger {
    uint64_t frame{};
    uintptr_t context{};
    bool ready{};
    void Observe(uint64_t nextFrame,uintptr_t nextContext,uint32_t nearPlane,uint32_t farPlane) {
        if (ready || !nextContext) return;
        if ((nearPlane==0x3dcccccdu || nearPlane==0x3e4ccccdu) && farPlane==0x466a6000u) {
            frame=nextFrame; context=nextContext;
        } else {
            ready=context==nextContext && frame==nextFrame && nearPlane==0x3e4ccccdu && farPlane==0x42c80000u;
            context=0;
        }
    }
};
// Native 0x25bf00 rebuilds projection depth and VP after the 0x8c051
// near-object subpass changes main+0x40 to .2 and far to 100.
constexpr size_t clipWords[]{0xe8,0xec,0x138,0x148,0x198,0x1a8,0x1b8,0x1c8};
inline uint32_t CameraWord(const CameraContext& c,size_t offset) {
    uint32_t v{}; memcpy(&v,c.data()+offset,4); return v;
}
// Native world far planes vary by route.
// Only the fixed destination clip identifies the guarded short/menu pass.
inline bool MenuSourceFar(uint32_t bits) {
    float farPlane{};memcpy(&farPlane,&bits,4);
    return std::isfinite(farPlane) && farPlane>100.f;
}
// Presentation classification only: this permits cancellation before either
// eye starts, never replaying the short pass or relaxing world camera guards.
inline bool MenuClipTransition(const CameraContext& entry,const CameraContext& complete,
                               uint32_t entryNear,uint32_t completeNear,uint32_t farPlane) {
    if (entry==complete || entryNear!=CameraWord(entry,0xe8) || completeNear!=CameraWord(complete,0xe8) ||
        farPlane!=CameraWord(entry,0xec) ||
        // Native replay navigation can enter one float ULP above .2 before
        // the guarded short pass stores its exact .2/100 constants. This is
        // presentation-only; world replay still requires unchanged clips.
        // Launcher cockpit assets use exactly .02 m to avoid roof clipping.
        // Accept that source only for the same fixed .2/100 menu destination.
        (entryNear!=0x3ca3d70au && entryNear!=0x3dcccccdu && entryNear!=0x3e4ccccdu && entryNear!=0x3e4cccceu) ||
        !MenuSourceFar(farPlane) || completeNear!=0x3e4ccccdu ||
        CameraWord(complete,0xec)!=0x42c80000u) return false;
    for (auto offset:clipWords) {
        float a{},b{};memcpy(&a,entry.data()+offset,4);memcpy(&b,complete.data()+offset,4);
        if (!std::isfinite(a) || !std::isfinite(b)) return false;
    }
    for (size_t i=0;i<entry.size();++i) {
        bool clip=false;
        for (auto offset:clipWords) if(i>=offset && i<offset+4)clip=true;
        if (!clip && entry[i]!=complete[i]) return false;
    }
    return true;
}
inline bool MenuCamera(const CameraContext& entry,const CameraContext& complete,
                       uint32_t entryNear,uint32_t completeNear,uint32_t farPlane) {
    if (MenuClipTransition(entry,complete,entryNear,completeNear,farPlane)) return true;
    // The native short pass leaves the route's main far scalar unchanged.
    // A subsequent paused frame may therefore retain .2/100 in the camera
    // while the main object still reports its world far plane.
    if (entry!=complete || entryNear!=0x3e4ccccdu || completeNear!=entryNear ||
        (farPlane!=0x42c80000u && !MenuSourceFar(farPlane)) ||
        CameraWord(complete,0xe8)!=entryNear || CameraWord(complete,0xec)!=0x42c80000u) return false;
    for (auto offset:clipWords) {
        float value{};memcpy(&value,complete.data()+offset,4);
        if (!std::isfinite(value)) return false;
    }
    return true;
}
inline bool ReplayCameraSupported(const CameraContext& entry,const CameraContext& complete,
                                  uint32_t entryNear,uint32_t completeNear,uint32_t farPlane,bool allowClipTransition=true) {
    if (entryNear!=CameraWord(entry,0xe8) || completeNear!=CameraWord(complete,0xe8) ||
        farPlane!=CameraWord(entry,0xec)) return false;
    if (entry==complete) return entryNear==completeNear;
    // The changed-clip replay is fixture-tested but still awaiting a new live
    // desktop witness. The initial headset probe accepts stable clips only.
    if (!allowClipTransition) return false;
    // The guarded native near-object pass sets .2/100. Subsequent frames can
    // already enter at near .2; both observed paths keep every non-clip byte.
    if ((entryNear!=0x3dcccccdu && entryNear!=0x3e4ccccdu) || farPlane!=0x466a6000u ||
        completeNear!=0x3e4ccccdu || CameraWord(complete,0xec)!=0x42c80000u) return false;
    for (auto offset:clipWords) {
        float a{},b{};
        memcpy(&a,entry.data()+offset,4); memcpy(&b,complete.data()+offset,4);
        if (!std::isfinite(a) || !std::isfinite(b)) return false;
    }
    for (size_t i=0;i<entry.size();++i) {
        bool clip=false;
        for (auto offset:clipWords) if (i>=offset && i<offset+4) clip=true;
        if (!clip && entry[i]!=complete[i]) return false;
    }
    return true;
}
inline bool CameraClipEqual(const CameraContext& a,const CameraContext& b) {
    // VP depth also depends on pose, so compare the native scalar inputs and
    // projection depth only when checking different eyes at the same phase.
    for (size_t offset:{0xe8u,0xecu,0x138u,0x148u})
        if (CameraWord(a,offset)!=CameraWord(b,offset)) return false;
    return true;
}
inline void RestoreClipWords(void* target,const CameraContext& saved) {
    auto* bytes=static_cast<uint8_t*>(target);
    for (auto offset:clipWords) memcpy(bytes+offset,saved.data()+offset,4);
}
}
