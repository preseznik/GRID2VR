#include "replay_presentation.h"
#include <map>
#include <cstring>
#include <cstdio>

int main() {
    using namespace grid2vr;
    constexpr uintptr_t base=0x400000,playback=0x2000000,parent=0x2100000;
    std::map<uintptr_t,uintptr_t> words;
    const auto read=[&](uintptr_t address,void* destination,size_t bytes) {
        const auto it=words.find(address);
        if(it==words.end() || bytes!=sizeof(uintptr_t))return false;
        std::memcpy(destination,&it->second,bytes);return true;
    };
    if(RecordedReplay(base,read)!=RecordedReplayState::Unavailable)return 1;
    words[base+0xe61010]=0;
    if(RecordedReplay(base,read)!=RecordedReplayState::Inactive)return 2;
    words[base+0xe61010]=playback;words[playback+0xa8]=parent;words[parent]=base+0xd1266c;
    if(RecordedReplay(base,read)!=RecordedReplayState::Active)return 3;
    for(auto address:{base+0xe61010,playback+0xa8,parent}) {
        const auto saved=words[address];words[address]=1;
        if(RecordedReplay(base,read)!=RecordedReplayState::Unavailable)return 4;
        words[address]=saved;
    }
    for(auto state:{RecordedReplayState::Inactive,RecordedReplayState::Active,RecordedReplayState::Unavailable}) {
        if(!ReplayPresentationAllowed(true,state))return 5;
        if(ReplayPresentationAllowed(false,state)!=(state==RecordedReplayState::Inactive))return 6;
    }
    if(RecordedReplay(0,read)!=RecordedReplayState::Unavailable)return 7;
    std::puts("Replay presentation preserves default stereo and safely opts recorded playback out without changing input or race state.");
}
