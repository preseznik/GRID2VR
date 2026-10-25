#pragma once
#include <cstdint>
#include <cstddef>

namespace grid2vr {
enum class RecordedReplayState { Inactive, Active, Unavailable };

// Read the recorded-playback owner already observed by the player-control
// diagnostic. This does not select a controller, camera or simulation mode.
template<class Reader>
RecordedReplayState RecordedReplay(uintptr_t base,Reader read) {
    const auto pointer=[&](uintptr_t address,uintptr_t& value) {
        return read(address,&value,sizeof(value)) && value>=0x10000 &&
            value<=UINTPTR_MAX-0x10000 && value%4==0;
    };
    uintptr_t playback{},parent{},table{};
    if(!base || !read(base+0xe61010,&playback,sizeof(playback))) return RecordedReplayState::Unavailable;
    if(!playback) return RecordedReplayState::Inactive;
    if(playback<0x10000 || playback>UINTPTR_MAX-0x10000 || playback%4 ||
        !pointer(playback+0xa8,parent) || !pointer(parent,table) || table!=base+0xd1266c)
        return RecordedReplayState::Unavailable;
    return RecordedReplayState::Active;
}
inline bool ReplayPresentationAllowed(bool replay3d,RecordedReplayState state) {
    // An explicit opt-out also keeps an unreadable/stale playback owner flat.
    // The default never changes the existing camera/replay eligibility rules.
    return replay3d || state==RecordedReplayState::Inactive;
}
}
