#pragma once
#include "chase_camera_adapter.h"
#include "chase_camera_playback_state.h"
#include "chase_camera_replay_clock.h"
#include "chase_camera_replay_clock_guard.h"
#include "chase_camera_replay_owner_guard.h"
#include "chase_camera_replay_director_guard.h"
#include "chase_camera_replay_shuttle_guard.h"
#include "chase_camera_replay_menu.h"
#include "chase_camera_replay_menu_guard.h"

namespace grid2vr::chasecamera {
inline bool RecordedReplayGuardsMatch(uintptr_t base) {
    for(const auto& guard:PlaybackClockGuards(base))
        if(!MatchesInstructions(reinterpret_cast<void*>(base+guard.rva),{guard.bytes.data(),guard.size}))return false;
    for(const auto& guard:ReplayOwnerInputGuards(base))
        for(size_t offset=0;offset<guard.size;offset+=64)
            if(!MatchesInstructions(reinterpret_cast<void*>(base+guard.rva+offset),
                {guard.bytes.data()+offset,std::min(size_t{64},guard.size-offset)}))return false;
    for(const auto& guard:ReplayDirectorThreeGuards(base))
        if(!MatchesInstructions(reinterpret_cast<void*>(base+guard.rva),{guard.bytes.data(),guard.size}))return false;
    for(const auto& guard:ReplayShuttleGuards(base))
        if(!MatchesInstructions(reinterpret_cast<void*>(base+guard.rva),{guard.bytes.data(),guard.size}))return false;
    for(const auto& guard:ReplayMenuScopeGuards(base))
        for(size_t offset=0;offset<guard.size;offset+=64)
            if(!MatchesInstructions(reinterpret_cast<void*>(base+guard.rva+offset),
                {guard.bytes.data()+offset,std::min(size_t{64},guard.size-offset)}))return false;
    return true;
}
struct ReplayPlayback {
    ReplayClockSample clock;
    ReplayMenuScope menuScope{};
    uint32_t mode{},direction{},transport{},coreState{},begin{},end{};
    std::array<uint32_t,8> cars{};
    float playRate{},reverseRate{},shuttleScale{};
    double fastRate{},timeScale{};
    uint8_t endpoint{},reverseBoundary{};
    bool operator==(const ReplayPlayback&) const=default;
    uintptr_t Ui()const {return clock.identity[2];}
};
// UI-owned recorded playback, not the renderer's second-eye replay. Require the
// independent offline local-race gate too. Read only; never touch recorder P or
// its buffers. P is non-polymorphic: P+0 is an index, not a vtable.
template<class Read> const char* CaptureRecordedReplay(uintptr_t base,uintptr_t camera,
    uintptr_t provider,uintptr_t entity,Read&& read,ReplayPlayback& result) {
    if(!ReplayStateImage(base))return "replay_image_invalid";
    const auto capture=[&](ReplayPlayback& value)->const char* {
        auto& id=value.clock.identity;
        const auto pointer=[&](uintptr_t at,uint32_t& out) {return read(at,&out,4) && ReplayStateAddress(out);};
        const auto word=[&](uintptr_t at,uint32_t expected) {uint32_t got{};return read(at,&got,4) && got==expected;};
        const auto byte=[&](uintptr_t at,uint8_t expected) {uint8_t got{};return read(at,&got,1) && got==expected;};
        if(!pointer(base+0xdef810,id[0]) || !pointer(id[0]+0x10,id[1]) || !pointer(id[1]+0x148c,id[2]) ||
           !pointer(id[1]+0x1504,id[3]) || !pointer(id[1]+0x14fc,id[4]) || !pointer(id[1]+0x150c,id[5]) ||
           !pointer(id[2]+0x75ec,id[6]) || !pointer(id[3]+0xa8,id[7]) || !pointer(id[4]+0x33c,id[8]))return "replay_owner_unreadable";
        id[9]=static_cast<uint32_t>(camera);id[10]=static_cast<uint32_t>(provider);id[11]=static_cast<uint32_t>(entity);
        if(!ReplayStateAddress(camera) || !ReplayStateAddress(provider) || !ReplayStateAddress(entity) ||
           !pointer(provider+0xc,id[12]))return "replay_target_invalid";
        if(!word(id[2],static_cast<uint32_t>(base+0xd10640)) || !word(id[2]+0x260,static_cast<uint32_t>(base+0xd10574)) ||
           !word(id[3],static_cast<uint32_t>(base+0xd0a374)) || !word(id[5],static_cast<uint32_t>(base+0xce3714)) ||
           !word(id[7],static_cast<uint32_t>(base+0xd1266c)) || !word(id[12],static_cast<uint32_t>(base+0xcdfeb4)))return "replay_owner_type";
        if(!word(id[5]+0x10,id[2]) || !word(id[5]+0x18,id[3]) || !word(id[5]+0x1c,id[4]) ||
           !word(id[3]+0xac,id[4]) || !word(id[2]+0x75f0,id[3]) || !word(id[2]+0x7638,id[7]) ||
           !word(id[6]+0xa8,id[7]) || !word(base+0xd96808,id[7]) || !word(base+0xdaa424,id[7]) ||
           id[12]+0x12c!=provider || !word(id[12]+0x134,id[11]) || !word(id[12]+0x148,id[8]))return "replay_owner_links";
        uintptr_t selectedManager{};
        if(!SelectedOwner(base,camera,read,selectedManager) || selectedManager!=id[8] ||
           !word(camera+0x60,id[10]) || !word(provider,static_cast<uint32_t>(base+0xd0c068)) || !word(provider+8,id[11]))return "replay_selection_changed";
        if(!pointer(id[7]+0xa8,value.begin) || !pointer(id[7]+0xac,value.end) ||
           value.end<=value.begin || (value.end-value.begin)%4 || (value.end-value.begin)/4>8)return "replay_car_range";
        bool found{};
        for(size_t i=0;i<(value.end-value.begin)/4;++i) {
            if(!pointer(value.begin+i*4,value.cars[i]) || !word(value.cars[i],static_cast<uint32_t>(base+0xcdfeb4)))return "replay_car_invalid";
            for(size_t j=0;j<i;++j)if(value.cars[j]==value.cars[i])return "replay_car_duplicate";
            found|=value.cars[i]==id[12];
        }
        if(!found)return "replay_car_not_owned";
        if(!read(id[2]+0x1d8,&value.mode,4) || (value.mode!=11 && (value.mode<1 || value.mode>6)))return "replay_ui_mode_unsupported";
        if(!read(id[5]+0x30,&value.direction,4) || (value.direction!=0 && value.direction!=3 && value.direction!=10))return "replay_director_unsupported";
        if(value.direction==3)if(const auto why=CaptureReplayMenuScope(base,id[1],read,value.menuScope))return why;
        if(!word(id[2]+0x7640,0))return "replay_override_active";
        if(!read(id[2]+0x7604,&value.transport,4) || value.transport>1)return "replay_transport_unsupported";
        if(!byte(id[2]+0x238,1))return "replay_ui_disabled";
        // Preserve native input suppression during overlays/ownership changes.
        // These flags are native input gates, not a general menu detector.
        for(uintptr_t offset:{0x391,0x392,0x3a2,0x3a3})if(!byte(id[4]+offset,0))return "replay_input_blocked";
        if(!byte(id[8]+0x2bc,0) || !byte(id[2]+0x168,0))return "replay_input_blocked";
        if(!read(id[2]+0x48,&value.clock.time,8) || !read(id[2]+0x250,&value.clock.seekTarget,8) ||
           !read(id[2]+0x248,&value.clock.step,8) || !read(id[2]+0x218,&value.playRate,4) ||
           !read(id[2]+0x214,&value.reverseRate,4) || !read(id[2]+0x228,&value.fastRate,8) ||
           !read(id[2]+0x230,&value.timeScale,8) || !read(id[2]+0x28,&value.coreState,4) ||
           !read(id[2]+0x181,&value.endpoint,1) || value.endpoint>1 ||
           !read(id[2]+0x210,&value.reverseBoundary,1) || value.reverseBoundary>1)return "replay_clock_unreadable";
        if(value.mode==11 && (!read(id[2]+0x220,&value.shuttleScale,4) || !std::isfinite(value.shuttleScale)))return "replay_shuttle_invalid";
        if(!std::isfinite(value.playRate) || !std::isfinite(value.reverseRate) || !std::isfinite(value.fastRate) ||
           !std::isfinite(value.timeScale) || !std::isfinite(value.clock.time) || !std::isfinite(value.clock.seekTarget) ||
           !std::isfinite(value.clock.step) || std::abs(value.clock.step)>.25 || std::abs(value.clock.time)>1e9 ||
           std::abs(value.clock.seekTarget)>1e9)return "replay_clock_invalid";
        double rate=value.mode==1?value.playRate:value.mode==2?0:value.mode==3?-value.reverseRate:
            value.mode==4?value.fastRate:value.mode==5?.1*value.playRate:value.mode==6?-.1:
            static_cast<double>(value.shuttleScale)*value.playRate;
        rate*=value.timeScale;
        if((rate<0 && value.reverseBoundary) || (rate>=0 && value.coreState==1 && value.endpoint))rate=0;
        if(!std::isfinite(rate) || std::abs(rate)>64)return "replay_rate_invalid";
        value.clock.rate=rate;
        return nullptr;
    };
    ReplayPlayback first{},second{};
    if(const auto refusal=capture(first))return refusal;
    if(capture(second) || first!=second)return "replay_snapshot_changed";
    result=first;return nullptr;
}
}
