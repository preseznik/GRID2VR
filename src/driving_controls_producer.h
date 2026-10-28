#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include "driving_controls_model.h"

namespace grid2vr::drivingcontrols {
inline bool NativeInputEnabled(uint32_t blocked,uint8_t enabled) {return !(blocked&0xff) && enabled!=0;}
inline int HPatternIndex(std::string_view action) {
    if(action=="Gear Reverse")return 7;
    return action.size()==6 && action.starts_with("Gear ") && action[5]>='1' && action[5]<='7'?action[5]-'1':-1;
}
// Native device config setter RVA 175b60: transmission_type maps to these
// two local-controller fields. The update copies +04 into the gearbox enum
// output +34; +44 selects sequential versus numbered-gear input queries.
inline bool ApplyTransmission(std::span<uint8_t> controller,Transmission transmission) {
    if(controller.size()!=0x6c || controller[0x40]!=1 || transmission==Transmission::Game)return false;
    uint32_t mode{},selection{};std::memcpy(&mode,controller.data()+4,4);std::memcpy(&selection,controller.data()+0x44,4);
    if(!((mode==4 && selection==1) || (mode==2 && (selection==1 || selection==2)) || (mode==1 && selection==2)))return false;
    switch(transmission) {
    case Transmission::Automatic:mode=4;selection=1;break;
    case Transmission::Sequential:mode=2;selection=1;break;
    case Transmission::HPattern:mode=2;selection=2;break;
    case Transmission::HPatternClutch:mode=1;selection=2;break;
    default:return false;
    }
    std::memcpy(controller.data()+4,&mode,4);std::memcpy(controller.data()+0x44,&selection,4);return true;
}
// AVX local controller RVA 1aab50: +30 is the Clutch query result; +24 is
// the H-pattern gear (1..7, -1 reverse, 9 neutral), enabled only in mode +44=2.
// These are volatile outputs. No action maps, saved bindings or gear mode are
// added/changed. The native update and its eligibility/forced-clutch rules run
// first. This path covers actions absent from the selected native action map.
struct SupplementalInput {
    bool clutchConnected{};
    float clutch{};
    std::array<bool,8> gears{}; // gears 1..7, reverse
};
inline bool ApplySupplementalInput(std::span<uint8_t> controller,const SupplementalInput& input) {
    if(controller.size()!=0x6c || controller[0x40]!=1 || !std::isfinite(input.clutch) || input.clutch<0 || input.clutch>1)return false;
    uint32_t forced{},mode{},transmission{};int32_t nativeGear{};float nativeClutch{};
    std::memcpy(&forced,controller.data()+0x3c,4);std::memcpy(&mode,controller.data()+0x44,4);
    std::memcpy(&transmission,controller.data()+0x34,4);
    std::memcpy(&nativeGear,controller.data()+0x24,4);std::memcpy(&nativeClutch,controller.data()+0x30,4);
    if(mode>2 || !std::isfinite(nativeClutch) || nativeClutch<0 || nativeClutch>1 ||
        (mode==2 && nativeGear!=-1 && nativeGear!=9 && (nativeGear<1 || nativeGear>7)))return false;
    bool changed{};
    if(input.clutchConnected && forced!=1) {
        auto value=std::max(nativeClutch,input.clutch);
        // The manual-clutch gearbox gate (AVX RVA 892a4a) requires exactly
        // 1.0. PC4090's calibrated G920 pedal tops out at .987919/.988281,
        // so retain analog travel and allow 2% end-stop tolerance in this mode.
        // Saved calibration, other transmissions and native-only input stay intact.
        if(mode==2 && transmission==1 && input.clutch>=.98f)value=1.f;
        if(value!=nativeClutch){std::memcpy(controller.data()+0x30,&value,4);changed=true;}
    }
    if(mode==2) {
        unsigned count{};int32_t selected=9;
        for(unsigned i=0;i<input.gears.size();++i)if(input.gears[i]){++count;selected=i==7?-1:static_cast<int32_t>(i+1);}
        // A released custom lever leaves native keyboard/controller gears
        // available. Conflicting custom gears fail neutral, never pick one.
        if(count>1)selected=9;
        if(count && selected!=nativeGear){std::memcpy(controller.data()+0x24,&selected,4);changed=true;}
    }
    return changed;
}
}
