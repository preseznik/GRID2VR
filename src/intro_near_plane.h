#pragma once
#include "replay_camera.h"
#include <cstdint>

namespace grid2vr {
// VR pre-race and replay shots. The tracks' director cameras (intro, grid and
// TV shots) render with GRID 2's 0.2 m near plane, like the chase cameras. On a
// monitor the view never reaches that close; a headset's much wider field of
// view looks steeply down from these low cameras, and the ground nearer than
// 0.2 m is cut away. While those shots play, the main object's near-plane input
// (main+0x40, read by camera builder 0x25bf00) becomes 5 cm before the native
// camera is built, so the native (left) eye and both replayed eyes match, and
// the game's own value returns as soon as the shots end. Only a world camera
// (far above 100 m) at exactly 0.2 m is changed: GRID 2's short/menu pass
// (.2/100) and the cockpit's 2 cm and bonnet 0.1 m planes are never touched.
class IntroNearPlane {
public:
    static constexpr uint32_t gameNear=0x3e4ccccdu;  // 0.2f
    static constexpr uint32_t introNear=0x3d4ccccdu; // 0.05f
    enum class Action { None, Apply, Restore };
    // At native Prepare: nearBits/farBits are main+0x40/+0x44; cinematic is the
    // pre-race/replay phase. Apply writes introNear, Restore writes Original().
    Action Next(bool enabled,bool cinematic,uint32_t nearBits,uint32_t farBits) {
        const bool wanted=enabled && cinematic && MenuSourceFar(farBits);
        if (active_ && nearBits==introNear) {
            if (wanted) return Action::None;
            active_=false;return Action::Restore;
        }
        // Not ours (any more): the game wrote its own value, which stays.
        active_=false;
        if (wanted && nearBits==gameNear) { active_=true;original_=nearBits;return Action::Apply; }
        return Action::None;
    }
    // The native camera must have taken the value; otherwise stop for the session.
    bool Verified(uint32_t mainNear,uint32_t cameraNear) const {
        return !active_ || (mainNear==introNear && cameraNear==introNear);
    }
    bool Active() const { return active_; }
    uint32_t Original() const { return original_; }
    void Reset() { active_=false; }
private:
    bool active_{};
    uint32_t original_{gameNear};
};
}
