#pragma once
#include "opponent_lod_runtime.h"
#include <atomic>
#include <cstdint>

namespace grid2vr::opponentlod::host {
// Controller observations only. Startup visibilityReadyFrame is deliberately
// absent: the native consumer and center camera must keep advancing together.
struct CenterWorld {
    uint64_t presentFrame{},cameraFrame{},consumerFrame{},pairFrame{},sceneFrame{};
    bool controller{},installed{},rendering{},failed{},replaying{},cameraValid{},
        objectsOwned{},worldActive{},visibilityEnabled{},visibilityHealthy{},normalPhase{};
};
inline bool Eligible(const CenterWorld& s) noexcept {
    return s.controller && s.installed && s.rendering && !s.failed && !s.replaying &&
        s.cameraValid && s.objectsOwned && s.worldActive && s.visibilityEnabled &&
        s.visibilityHealthy && s.normalPhase && s.presentFrame>1 &&
        s.cameraFrame==s.presentFrame-1 && s.consumerFrame==s.cameraFrame &&
        s.pairFrame==s.cameraFrame && s.sceneFrame==s.cameraFrame;
}

// Invalidation may originate on a worker. Never inspect XR/native objects here.
// A revision also prevents a controller Tick from leaving a stale publication
// enabled if a worker revoked while that Tick was executing.
inline std::atomic<uint32_t> revocations{};
static_assert(std::atomic<uint32_t>::is_always_lock_free);
inline void RevokeWorld() noexcept {
    revocations.fetch_add(1,std::memory_order_seq_cst);
    runtime::RevokeWorld();
}
template<class Readiness>
void PublishWorld(Readiness readiness) noexcept {
    const auto revision=revocations.load(std::memory_order_seq_cst);
    const bool eligible=readiness();
    runtime::Tick(eligible);
    // No second enabling Tick: invalidation wins this Present, including during
    // a transaction/receipt. A later controller decision may qualify a new epoch.
    if(revocations.load(std::memory_order_seq_cst)!=revision || !readiness())
        runtime::RevokeWorld();
}
} // namespace grid2vr::opponentlod::host
