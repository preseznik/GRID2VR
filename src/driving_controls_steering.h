#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

namespace grid2vr::drivingcontrols {
struct WheelSteeringInput {
    std::array<float,2> wheel{},other{}; // left, right; normalized action values
    bool connected{};
};
struct WheelCurveOverride { uintptr_t object{};float native{},applied{}; };
// An unrelated callback or temporarily missing route cannot revoke ownership.
// A verified native change does revoke it, without replacing the new value.
inline bool RestoreWheelCurve(WheelCurveOverride& patch,uintptr_t object,bool identityVerified,float currentValue,float& restoreValue) {
    if(!patch.object || patch.object!=object || !identityVerified)return false;
    const auto saved=patch;patch={};
    if(currentValue!=saved.applied)return false;
    restoreValue=saved.native;return true;
}

// AVX local controller: +28 is Right-Left, +2c is wheel linearity, and
// +38 bit 0x20 selects pad steering. The consumer applies signed power
// abs(steering)^(1 - linearity/2) on the wheel path. Our slider defines
// -10 as quicker near centre, 0 as linear, and +10 as gentler near centre;
// this does not assert the native menu's displayed sign convention.
// The caller owns lifetime/eligibility guards and restoring +2c when this
// selection ends: the native driver update does not reset that field.
inline bool ApplyWheelSteering(std::span<uint8_t> controller,const WheelSteeringInput& input,int linearity) {
    if(controller.size()!=0x6c || controller[0x40]!=1 || !input.connected || linearity<-10 || linearity>10)return false;
    for(const auto& values:{input.wheel,input.other})
        for(const auto value:values)if(!std::isfinite(value) || value<0 || value>1)return false;
    const auto wheel=std::max(input.wheel[0],input.wheel[1]);
    const auto other=std::max(input.other[0],input.other[1]);
    if(!(wheel>other || other==0))return false;
    const auto steering=std::max(input.wheel[1],input.other[1])-std::max(input.wheel[0],input.other[0]);
    const auto curve=-static_cast<float>(linearity)/10.0f;
    std::memcpy(controller.data()+0x28,&steering,sizeof(steering));
    std::memcpy(controller.data()+0x2c,&curve,sizeof(curve));
    controller[0x38]&=static_cast<uint8_t>(~0x20u);
    return true;
}
}
