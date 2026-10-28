#pragma once
#include <cstdint>
namespace grid2vr {
// The explicit simulated AI diagnostic has its own background allowance.
// Live headset sessions cannot use it; their focused rendering policy is below.
constexpr bool UnattendedStereoWindow(unsigned state,bool requested,bool runtime,bool recorded,bool xr) {
    return state==4 && requested && runtime && recorded && !xr;
}
// VDXR may own the desktop foreground while the game's garage/race is usable.
// This is rendering eligibility only, never input or foreground ownership.
// focusedViews must come from ReadyForViews(true), including a fresh focused,
// tracked, shouldRender sample; a merely VISIBLE dashboard session is rejected.
// nativeWorld must be fresh camera/visibility readiness, never a cached race
// selection. Scene completeness and replay eligibility are checked separately.
constexpr bool NormalXrRenderWindow(unsigned state,bool runtime,bool normal,bool recorded,
                                    bool liveXr,bool focusedViews,bool activeGarage,bool nativeWorld) {
    return (state==4 || state==5) && runtime && normal && !recorded && liveXr && focusedViews && (activeGarage || nativeWorld);
}
// Passive startup condition only; never changes foreground ownership.
class ForegroundHold {
public:
    void Reset() { observed_=false; }
    bool Observe(uint64_t now,bool foreground) {
        if (!foreground) { Reset();return false; }
        if (!observed_ || now<since_) { since_=now;observed_=true; }
        return now-since_>=1500;
    }
private:
    uint64_t since_{};
    bool observed_{};
};
}
