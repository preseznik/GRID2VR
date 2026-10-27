#pragma once
#include "chase_camera_behavior.h"
#include "chase_observer_prequery.h"
#include <bit>
#include <cstring>

namespace grid2vr::chasecamera {
using Pose=std::array<float,8>;
// Keep a native baseline only while the same object still contains our result.
// Restore before native simulation, after the manager caches the prior displayed
// position for camera-region crossing. Foreign/native changes win. No FOV,
// provider output or recording buffer is saved.
struct PoseOverlay {
    uintptr_t camera{},provider{},entity{};
    Pose native{},displayed{};
    bool valid{};
    template<class Read,class Write>
    bool Restore(uintptr_t owner,uintptr_t source,uintptr_t target,Read&& read,Write&& write) {
        if(!valid || owner!=camera)return true;
        valid=false;
        Pose current{};
        if(source!=provider || target!=entity || !read(camera+0xb0,current.data(),sizeof(current)))return false;
        if(std::memcmp(current.data(),displayed.data(),sizeof(current)))return true;
        return write(camera+0xb0,native.data(),sizeof(native));
    }
};

inline bool VehicleOrbitPivot(const chaseobserver::PreQueryObservation& snapshot,ChaseVector& pivot) {
    // Provider slot 5 returns the vehicle transform's translation (+330),
    // already matched by CapturePreQuery to this owned entity. Camera +2450
    // is a forward look/obstruction target (about 1.8 m ahead in the retained
    // samples), not the centre of the vehicle. Keep that native target intact;
    // only our orbit and distance transform use the vehicle origin.
    const auto f=[](uint32_t bits){return std::bit_cast<float>(bits);};
    // Preserve the adapter's existing native-context sanity checks even though
    // these values no longer contribute to our pivot.
    for(unsigned column=0;column<3;++column) {
        const float offset=f(snapshot.localOffsetWords[8+column]);
        if(!std::isfinite(offset) || std::abs(offset)>20)return false;
        for(unsigned axis=0;axis<3;++axis) {
            const float basis=f(snapshot.entityBasisWords[column*4+axis]);
            if(!std::isfinite(basis) || std::abs(basis)>1.01f)return false;
        }
    }
    const ChaseVector result{f(snapshot.entityPositionWords[0]),f(snapshot.entityPositionWords[1]),f(snapshot.entityPositionWords[2])};
    if(!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z))return false;
    pivot=result;return true;
}

// Qualify displayed selection independently from observation/export stack
// provenance. Reject transitions and changed graphs; a valid graph alone is
// insufficient without the qualified stage, local race and provider snapshot.
template<class Read>
bool SelectedOwner(uintptr_t base,uintptr_t camera,Read&& read,uintptr_t& manager) {
    const auto snapshot=[&](std::array<uint32_t,10>& words)->bool {
        const auto pointer=[&](uintptr_t at,uint32_t& value) {
            return read(at,&value,4) && chaseobserver::OwnerNativePointer(value);
        };
        uint8_t first{},second{},alternate{};
        if(!pointer(base+0xdef810,words[0]) || !pointer(words[0]+0x10,words[1]) ||
           !pointer(words[1]+0x14fc,words[2]) || !read(words[2]+0x392,&first,1) ||
           !read(words[2]+0x393,&second,1) || first || second || !pointer(words[2]+0x33c,words[3]) ||
           !pointer(words[3]+0x748,words[4]) || !read(words[3]+0x759,&alternate,1) || alternate>1 ||
           words[4]!=camera || !pointer(camera,words[5]) || words[5]!=base+0xd09948 ||
           !pointer(camera+0x60,words[6]) || !pointer(words[6]+8,words[7]))return false;
        words[9]=alternate;
        // The two captured chase views have no alternate (+70==0). A native
        // Look Back selection flag can therefore still select this same camera.
        // A real alternate camera requires its own validated pose convention.
        if(alternate && (!read(words[4]+0x70,&words[8],4) || words[8]))return false;
        return true;
    };
    std::array<uint32_t,10> first{},second{};
    if(!snapshot(first) || !snapshot(second) || first!=second)return false;
    manager=first[3];return true;
}
}
