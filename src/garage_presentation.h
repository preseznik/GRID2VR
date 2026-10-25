#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

namespace grid2vr {
enum class GarageMenuState { Inactive, Active, Unavailable };
struct GarageMenuSnapshot {
    GarageMenuState state{GarageMenuState::Unavailable};
    uintptr_t application{},frontend{},wrapper{},manager{},registry{},list{},data{},object{},context{};
    uint32_t count{},capacity{};
};

// AVX image 519cd994...: the application's flow manager updates only the
// descriptors in this active list (RVA7a96c0). Root state "57" is StateGarage.
// A constructed but inactive garage must never flatten a race. No game calls,
// heap search, executable writes or cached native pointers are used here.
template<class Reader>
GarageMenuSnapshot ReadGarageMenu(uintptr_t base,Reader read) {
    GarageMenuSnapshot result;
    const auto word=[&](uintptr_t address,uint32_t& value) { return read(address,&value,4); };
    const auto equals=[&](uintptr_t address,uintptr_t expected) {
        uint32_t value{};return word(address,value) && value==expected;
    };
    const auto pointer=[&](uintptr_t address,uintptr_t& value) {
        uint32_t native{};
        if(!word(address,native) || native<0x10000 || native>UINT32_MAX-0x2000 || native%4)return false;
        value=native;return true;
    };
    const auto compareKey=[&](uintptr_t address,int& comparison) {
        uint32_t text{};std::array<char,3> key{};
        if(!word(address,text) || text<0x10000 || text>UINT32_MAX-3 || !read(text,key.data(),key.size()) || !key[0])return false;
        constexpr std::array<unsigned char,3> wanted{'5','7',0};
        comparison=0;
        for(size_t i=0;i<key.size();++i) {
            const auto value=static_cast<unsigned char>(key[i]);
            if(value!=wanted[i]) {comparison=value<wanted[i]?-1:1;break;}
            if(!value)break;
        }
        return true;
    };
    const auto garageKey=[&](uintptr_t address,bool& matches) {
        int comparison{};if(!compareKey(address,comparison))return false;
        matches=comparison==0;return true;
    };
    if(!base || base>UINT32_MAX-0xe00000 ||
        !pointer(base+0xdef804,result.application) || !equals(result.application,base+0xce96b8) ||
        !pointer(result.application+0x1428,result.frontend) || !equals(result.frontend,base+0xccaf98) ||
        !pointer(result.frontend+0x40,result.wrapper) || !equals(result.wrapper,base+0xd53e64) ||
        !pointer(result.wrapper+0x10,result.manager) || !equals(result.manager,base+0xd50e0c) ||
        !pointer(result.manager+8,result.registry) || !equals(result.registry,base+0xd51700) ||
        !pointer(result.manager+0x1c,result.list) || !word(result.list+0x18,result.count) ||
        !word(result.list+0x14,result.capacity) || result.count>result.capacity || result.count>64)return result;
    std::array<uintptr_t,64> descriptors{};
    std::array<bool,64> keys{};
    bool active{};
    if(result.count && !pointer(result.list+0x10,result.data))return result;
    for(uint32_t i=0;i<result.count;++i) {
        bool matches{};
        if(!pointer(result.data+i*4,descriptors[i]) || !garageKey(descriptors[i]+8,matches))return result;
        for(uint32_t j=0;j<i;++j)if(descriptors[i]==descriptors[j])return result;
        if(matches && active)return result;
        keys[i]=matches;
        active|=matches;
    }
    uintptr_t map{},begin{},end{},matchedEntry{};
    if(active) {
        // RVA78cf90 / RVA9a7320: descending lexical {key,value} vector.
        // The captured garage has 1454 entries. Binary search needs at most
        // 13 comparisons under this bound, instead of scanning it every frame.
        if(!pointer(result.registry+8,map) || !pointer(map+4,begin) || !pointer(map+8,end) ||
            end<=begin || (end-begin)%8 || end-begin>4096*8)return result;
        auto low=begin,high=end;
        while(low<high) {
            const auto middle=low+((high-low)/8/2)*8;
            int comparison{};if(!compareKey(middle,comparison))return result;
            if(comparison>0)low=middle+8;else high=middle;
        }
        int comparison{};
        if(low==end || !compareKey(low,comparison) || comparison ||
            (low>begin && (!compareKey(low-8,comparison) || comparison<=0)) ||
            (low+8<end && (!compareKey(low+8,comparison) || comparison>=0)) ||
            !pointer(low+4,result.object) || !equals(result.object,base+0xcd21c0) ||
            !pointer(result.object+0x28,result.context) || !equals(result.frontend+0x10,result.context))return result;
        matchedEntry=low;
        if(!equals(result.registry+8,map) || !equals(map+4,begin) || !equals(map+8,end))return result;
    }
    // Scene changes may rebuild the list on another thread. Reject a changed
    // snapshot instead of keeping an old garage classification across scenes.
    if(!equals(base+0xdef804,result.application) || !equals(result.application+0x1428,result.frontend) ||
        !equals(result.frontend+0x40,result.wrapper) || !equals(result.wrapper+0x10,result.manager) ||
        !equals(result.manager+8,result.registry) || !equals(result.manager+0x1c,result.list) ||
        !equals(result.list+0x18,result.count) || !equals(result.list+0x14,result.capacity) ||
        (result.count && !equals(result.list+0x10,result.data)))return result;
    for(uint32_t i=0;i<result.count;++i) {
        bool matches{};
        if(!equals(result.data+i*4,descriptors[i]) || !garageKey(descriptors[i]+8,matches) || matches!=keys[i])return result;
    }
    if(active) {
        bool matches{};
        if(!garageKey(matchedEntry,matches) || !matches || !equals(matchedEntry+4,result.object) ||
            !equals(result.object,base+0xcd21c0) || !equals(result.object+0x28,result.context) ||
            !equals(result.frontend+0x10,result.context))return result;
    }
    result.state=active?GarageMenuState::Active:GarageMenuState::Inactive;
    return result;
}
inline bool GaragePresentationAllowed(bool garage3d,GarageMenuState state) {
    // Default on does not add an eligibility gate to established world rendering.
    // An explicit opt-out stays on the shared screen during unreadable transitions.
    return garage3d || state==GarageMenuState::Inactive;
}
}
