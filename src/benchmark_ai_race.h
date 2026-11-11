#pragma once
#include "driving_controls_route.h"
#include "flashback_observer_guards.h"
#include "flashback_observer_model.h"

namespace grid2vr {
// Read-only readiness of a plain -benchmark AI race, for installing background
// play only (the session's other native hooks keep their human-race gates). The
// monitor fields are trusted only while the flashback observer's code guards
// match; the car comes from the guarded local input route.
template<class Reader> bool BenchmarkAiRaceReady(uintptr_t base,Reader read) {
    if(!flashbackobserver::InstructionsMatch(base)) return false;
    const auto route=drivingcontrols::ReadInputRoute(base,read);
    if(!route.valid || !route.car) return false;
    auto sample=flashbackobserver::ReadSample(base,route.car,read);
    flashbackobserver::ReadWorld(base,read,sample);
    return flashbackobserver::BenchmarkAiRacing(sample);
}
}
