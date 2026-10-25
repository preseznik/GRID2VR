#include "foreground_hold.h"
#include "background_play.h"
int main() {
    grid2vr::ForegroundHold hold;
    if (hold.Observe(0,true) || hold.Observe(1499,true) || !hold.Observe(1500,true)) return 1;
    if (hold.Observe(1501,false) || hold.Observe(3000,true) || hold.Observe(4499,true) || !hold.Observe(4500,true)) return 2;
    hold.Reset();
    if (hold.Observe(5000,true) || !hold.Observe(6500,true)) return 3;
    if (hold.Observe(4000,true) || hold.Observe(5499,true) || !hold.Observe(5500,true)) return 4;
    for(unsigned state=0;state<8;++state)for(unsigned flags=0;flags<16;++flags) {
        const bool expected=state==4 && flags==7;
        if(grid2vr::UnattendedStereoWindow(state,flags&1,flags&2,flags&4,flags&8)!=expected)return 5;
    }
    // Only live Normal garage rendering can use XR focus instead of desktop
    // ownership. Recorded diagnostics, hidden/minimized/unknown windows,
    // inactive/unreadable garages and unready XR views keep their old gate.
    constexpr unsigned states[]{0,1,2,3,4,5,6,7,~0u};
    for(unsigned state:states)for(unsigned flags=0;flags<64;++flags) {
        const bool expected=(state==4 || state==5) && flags==59;
        if(grid2vr::NormalGarageXrWindow(state,flags&1,flags&2,flags&4,flags&8,flags&16,flags&32)!=expected)return 6;
    }
    grid2vr::ForegroundHold garage;
    const auto observe=[&](uint64_t now,unsigned state,bool xrReady,bool activeGarage) {
        return garage.Observe(now,grid2vr::NormalGarageXrWindow(state,true,true,false,true,xrReady,activeGarage));
    };
    // The desktop may change owners while the game stays visible and XR stays
    // focused. An overlay or lost tracking/shouldRender must reset the hold.
    if(observe(0,5,true,true) || observe(1499,4,true,true) || !observe(1500,4,true,true))return 7;
    if(observe(1501,4,false,true) || observe(2000,4,true,true) || observe(3499,4,true,true) ||
        !observe(3500,4,true,true))return 8;
    // Leaving the garage for a load/race is not a latched refusal. A later
    // verified garage can recover after a fresh hold; a minimized window cannot.
    if(observe(3501,4,true,false) || observe(4000,4,true,true) || !observe(5500,4,true,true) ||
        observe(5501,6,true,true) || observe(6000,4,true,true) || !observe(7500,4,true,true))return 9;
    garage.Reset();
    if(observe(8000,4,true,true) || observe(9499,4,true,true) || !observe(9500,4,true,true))return 10;
    // Native race readiness remains independent: the existing background-play
    // installation can resume a visible race after garage classification ends.
    grid2vr::ForegroundHold native;
    const auto window=[&](uint64_t now,bool activeGarage,bool backgroundPlay) {
        const bool nativeReady=native.Observe(now,grid2vr::backgroundplay::WindowEligible(4,backgroundPlay));
        const bool garageReady=observe(now,4,true,activeGarage);
        return nativeReady || garageReady;
    };
    if(!window(9600,true,false) || window(9700,false,false) || window(10000,false,true) ||
        window(11499,false,true) || !window(11500,false,true))return 11;
    // Closing the background path must not leave a stale garage exception.
    if(window(11501,false,false) || window(12000,true,false) || !window(13500,true,false))return 12;
}
