#pragma once
#include "chase_camera_playback_state.h"
#include <cstring>

namespace grid2vr::chasecamera {
using ReplayMenuScope=std::array<uint32_t,10>;
// D3 is shared by pause-menu Replay and Flashback. Read the native flow's
// explicit entry flag; never invoke its lookup/setter or change the recording.
// The caller captures twice and compares the complete scope with the camera.
template<class Read> const char* CaptureReplayMenuScope(uintptr_t base,uint32_t state,Read&& read,ReplayMenuScope& id) {
    if(base>UINT32_MAX-0x100e000)return "replay_menu_image_invalid";
    const auto pointer=[&](uintptr_t at,uint32_t& out) {return read(at,&out,4) && ReplayStateAddress(out);};
    const auto word=[&](uintptr_t at,uint32_t expected) {uint32_t got{};return read(at,&got,4) && got==expected;};
    if(!pointer(base+0xdef804,id[0]) || id[0]!=state || !word(id[0],static_cast<uint32_t>(base+0xce96b8)) ||
       !pointer(id[0]+0x1428,id[1]) || !word(id[1],static_cast<uint32_t>(base+0xccaf98)) ||
       !pointer(id[1]+0x10,id[2]) || !pointer(id[2]+4,id[3]) ||
       !read(base+0xed14e8,&id[4],4) || id[4]>4096 || !pointer(id[3]+id[4]*4,id[5]) ||
       !word(id[1]+0x38,id[5]) || !word(id[5],static_cast<uint32_t>(base+0xccaf88)))return "replay_menu_owner_invalid";
    if(!pointer(id[5]+8,id[6]) || !pointer(id[5]+12,id[7]) || id[7]<=id[6] ||
       (id[7]-id[6])%40 || (id[7]-id[6])/40>65536)return "replay_menu_map_invalid";
    constexpr char key[]="replay_from_menu_flag";
    const auto count=(id[7]-id[6])/40;
    std::array<uint8_t,40> entry{};
    const auto entryAt=[&](uint32_t index) {
        return read(id[6]+index*40,entry.data(),entry.size()) && std::memchr(entry.data()+4,0,32);
    };
    const auto entryWord=[&](size_t offset) {uint32_t value{};std::memcpy(&value,entry.data()+offset,4);return value;};
    uint32_t low{},high=count;
    // The native comparator orders by name before key type. Bound equal-name
    // entries too, and accept only one exact bool key, never a derived key.
    while(low<high) {
        const auto middle=low+(high-low)/2;
        if(!entryAt(middle))return "replay_menu_entry_unreadable";
        if(std::strcmp(reinterpret_cast<const char*>(entry.data()+4),key)<0)low=middle+1;else high=middle;
    }
    unsigned matches{},inspected{};
    for(auto index=low;index<count;++index) {
        if(!entryAt(index))return "replay_menu_entry_unreadable";
        if(std::strcmp(reinterpret_cast<const char*>(entry.data()+4),key))break;
        if(++inspected>64)return "replay_menu_flag_ambiguous";
        if(entryWord(0)!=base+0xed05bc)continue;
        if(++matches>1)return "replay_menu_flag_ambiguous";
        id[8]=id[6]+index*40;id[9]=entryWord(36);
    }
    if(matches!=1 || !ReplayStateAddress(id[9]) || !word(id[9],static_cast<uint32_t>(base+0xcc5ba4)))return "replay_menu_flag_missing";
    uint8_t fromMenu{};
    if(!read(id[9]+4,&fromMenu,1) || fromMenu>1)return "replay_menu_flag_invalid";
    return fromMenu?nullptr:"replay_flashback_excluded";
}
}
