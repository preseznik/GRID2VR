#pragma once
#include <cstdint>
#include "driving_controls_route.h"

namespace grid2vr::drivingcontrols::shift {
struct Status {
    const char* reason="not_requested";
    bool enabled{},ready{};
    unsigned threshold=100,attached{};
    uint64_t callbacks{},localJobs{},clonedJobs{},refused{},gameDataInstanceWrites{},restores{};
};
void Configure(uintptr_t imageBase,unsigned thresholdPercent,bool enabled);
void Permit(const InputRoute&,bool allowed,uint64_t sampledAt);
void Tick();
void Stop();
Status Snapshot();

#ifdef GRID2VR_DRIVING_CONTROLS_SHIFT_TEST
namespace testing {
// The mapped image is read-only evidence. Tests inspect private copies and use
// owned callbacks; they never execute the mapped game or its copied bodies.
bool Prepare(uintptr_t imageBase,unsigned thresholdPercent);
uintptr_t Clone(unsigned index);
uintptr_t Table();
uintptr_t Threshold();
bool Eligible(void* job,const InputRoute& current,uint64_t now);
void Dispatch(void* job,const InputRoute& current,uint64_t now,void* nativeCallback,void* cloneCallback,bool focused=true);
}
#endif
}
