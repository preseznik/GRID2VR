#pragma once

namespace grid2vr::instancecapacity {
struct AllocationObservationPolicy {
    bool requested{},runtime{},recordedViews{},unattended{},flicker{};
    bool headset{},normal{},race{},playerControl{};
    const char* Suppression() const {
        if(!requested)return "not_requested";
        if(headset)return "headset";
        if(normal)return "normal_launch";
        if(race)return "race_launch";
        if(playerControl)return "player_control";
        if(!runtime || !recordedViews || !unattended || !flicker)return "not_isolated_ai";
        return nullptr;
    }
};

// Buffer qualification is required independently of optional native-call
// telemetry. In particular, suppressing the inline observer cannot skip Bind.
template<class Bind,class Observe>
bool QualifyReplayResources(bool bindingRequired,bool observationRequested,Bind bind,Observe observe) {
    if(bindingRequired && !bind())return false;
    return !observationRequested || observe();
}
}
