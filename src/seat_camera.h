#pragma once
#include "seat_cars.h"
#include "seat_menu.h"

namespace grid2vr::seat {
inline constexpr size_t CarCount=baseCars.size()+dlccockpit::cars.size();
inline const char* CarCode(size_t car) {return car<baseCars.size()?baseCars[car].code:car<CarCount?dlccockpit::cars[car-baseCars.size()].code:"";}
inline const wchar_t* CarLabel(size_t car) {
    for(const auto& label:carLabels)if(std::string_view(label.first)==CarCode(car))return label.second;
    return L"Car not identified";
}
inline int FindCar(std::string_view code) {
    for(size_t i=0;i<CarCount;++i)if(code==CarCode(i))return static_cast<int>(i);
    return -1;
}
using CameraObject=std::array<uint8_t,dlccockpit::objectBytes>;
using CameraPair=std::array<CameraObject,2>;
struct CameraSettings {size_t car{};Position position;int fov{};bool desktop{},hideDriver{};};
inline bool Valid(const CameraSettings& value) {
    return value.car<CarCount && Valid(value.position) && value.fov>=-30 && value.fov<=30 && (value.desktop || !value.fov);
}
inline bool ExpectedPair(const CameraSettings& settings,uint32_t table,CameraPair& output) {
    if(!Valid(settings))return false;
    CameraPair next{};
    const bool base=settings.car<baseCars.size();
    for(size_t view=0;view<2;++view) {
        auto& object=next[view];
        std::memcpy(object.data(),&table,4);
        const auto name=view?"bonnet_reverse":"bonnet";
        std::memcpy(object.data()+0x19,name,std::strlen(name)+1);
        const auto hash=dlccockpit::NameHash(name);std::memcpy(object.data()+0x98,&hash,4);
        const auto& original=base?baseCars[settings.car].views[view]:dlccockpit::cars[settings.car-baseCars.size()].original[view];
        for(size_t i=0;i<dlccockpit::floatOffsets.size();++i)
            std::memcpy(object.data()+dlccockpit::floatOffsets[i],&original.values[i],4);
        for(size_t i=0;i<dlccockpit::flagOffsets.size();++i)
            if(original.flags[i]>=0)object[dlccockpit::flagOffsets[i]]=static_cast<uint8_t>(original.flags[i]);
        if(base) {
            const float offsets[]{-settings.position.right/100.f,settings.position.height/100.f,settings.position.forward/100.f};
            for(size_t i=0;i<3;++i) {
                const float value=original.values[i]+offsets[i];
                std::memcpy(object.data()+dlccockpit::floatOffsets[i],&value,4);
            }
            object[dlccockpit::flagOffsets[2]]=settings.hideDriver?1:0;
            const float fov=original.values[19]+settings.fov;
            if(fov<15 || fov>140)return false;
            std::memcpy(object.data()+0x14,&fov,4);
        }
    }
    if(!base) {
        const auto car=settings.car-baseCars.size();auto position=dlccockpit::cars[car].seat;
        position[0]-=settings.position.right/100.f;position[1]+=settings.position.height/100.f;position[2]+=settings.position.forward/100.f;
        if(!dlccockpit::PatchPair(next[0],next[1],table,{car,position},settings.hideDriver,settings.fov))return false;
    }
    output=next;return true;
}
inline bool Matches(std::span<const uint8_t> object,const CameraObject& expected,const CameraSettings& settings,size_t view,uint32_t table) {
    if(object.size()!=dlccockpit::objectBytes || view>1 || !Valid(settings) || object[dlccockpit::poseCacheValidOffset]>1)return false;
    uint32_t actual{};std::memcpy(&actual,object.data(),4);if(actual!=table)return false;
    const auto name=view?"bonnet_reverse":"bonnet";
    if(std::memcmp(object.data()+0x19,name,std::strlen(name)+1))return false;
    std::memcpy(&actual,object.data()+0x98,4);if(actual!=dlccockpit::NameHash(name))return false;
    for(auto at:dlccockpit::floatOffsets) {
        const auto value=dlccockpit::FloatAt(object,at);
        if(!std::isfinite(value) || std::abs(value-dlccockpit::FloatAt(expected,at))>.0001f)return false;
    }
    const auto& flags=settings.car<baseCars.size()?baseCars[settings.car].views[view].flags:dlccockpit::cars[settings.car-baseCars.size()].original[view].flags;
    for(size_t i=0;i<flags.size();++i)
        if(flags[i]>=0 && object[dlccockpit::flagOffsets[i]]!=expected[dlccockpit::flagOffsets[i]])return false;
    return true;
}
// Call only after the host has revalidated the local owner/manager graph and
// fingerprinted native consumers. Changes only authored offset/FOV and clears
// Mounted's pose-valid byte, so its native update rebuilds both views next frame.
inline bool EditPair(std::span<uint8_t> forward,std::span<uint8_t> reverse,uint32_t table,const CameraSettings& from,const CameraSettings& to) {
    if(from.car!=to.car || from.desktop!=to.desktop || from.hideDriver!=to.hideDriver)return false;
    const auto a=reinterpret_cast<uintptr_t>(forward.data()),b=reinterpret_cast<uintptr_t>(reverse.data());
    if((a>b?a-b:b-a)<dlccockpit::objectBytes)return false;
    CameraPair expected,next;
    if(!ExpectedPair(from,table,expected) || !ExpectedPair(to,table,next) ||
       !Matches(forward,expected[0],from,0,table) || !Matches(reverse,expected[1],from,1,table))return false;
    for(size_t view=0;view<2;++view) {
        auto object=view?reverse:forward;bool changed{};
        for(auto at:{size_t(0x2230),size_t(0x2234),size_t(0x2238),size_t(0x14)}) {
            if(std::memcmp(object.data()+at,next[view].data()+at,4)) {
                std::memcpy(object.data()+at,next[view].data()+at,4);changed=true;
            }
        }
        if(changed)object[dlccockpit::poseCacheValidOffset]=0;
    }
    return true;
}
}
