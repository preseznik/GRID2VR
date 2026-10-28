#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "local_controller_identity.h"

namespace grid2vr::drivingcontrols {
struct InputRoute { uintptr_t controller{},input{};uint32_t map{};bool valid{};uintptr_t car{},physics{}; };
inline bool SameInputRoute(const InputRoute& sampled,const InputRoute& current) {
    return sampled.valid && current.valid && sampled.controller==current.controller && sampled.input==current.input && sampled.map==current.map;
}
inline bool SamePhysicsRoute(const InputRoute& sampled,const InputRoute& current) {
    return SameInputRoute(sampled,current) && sampled.car && sampled.physics &&
        sampled.car==current.car && sampled.physics==current.physics;
}
// Read the local driving controller's actual query map. The saved map alone is
// not evidence that gameplay consumes it. No pointer or player ID is changed.
template<class Reader> InputRoute ReadInputRoute(uintptr_t base,Reader read) {
    InputRoute result;
    const auto word=[&](uintptr_t at,uint32_t& value){return read(at,&value,4);};
    const auto equals=[&](uintptr_t at,uintptr_t expected){uint32_t value{};return word(at,value)&&value==expected;};
    uint32_t parent{},head{},root{};
    if(!word(base+0xd96808,parent) || !equals(parent,base+0xd1266c) ||
       !word(parent+0x1a4,head) || !word(head+4,root))return result;
    std::array<uint32_t,8> pending{},seen{};size_t queued{},visited{};
    if(root!=head)pending[queued++]=root;
    while(queued) {
        const auto node=pending[--queued];
        if(visited==seen.size())return {};
        for(size_t i=0;i<visited;++i)if(seen[i]==node)return {};
        seen[visited++]=node;
        std::array<uint32_t,5> words{};uint32_t player{};
        if(!read(node,words.data(),sizeof(words)) || !word(words[3]+0xb0,player))return {};
        if(player==0) {
            uint32_t begin{},end{},controller{},input{},map{};
            if(result.controller || !word(words[4]+8,begin) || !word(words[4]+12,end) ||
               end<begin || end-begin!=8 || !word(begin,controller) || !ReadLocalControllerIdentity(base,controller,read) ||
               !word(controller+0x10,input) || !word(controller+0x5c,map) ||
               !equals(words[4]+8,begin) || !equals(words[4]+12,end) || !equals(begin,controller))return {};
            uint32_t physics{};
            // Optional for legacy input routing; the shift gate requires this
            // exact car-to-physics link, also used by native dispatch520cdb.
            if(!word(words[3]+0x30,physics))physics=0;
            result={controller,input,map,false,words[3],physics};
        }
        for(auto child:{words[0],words[2]})if(child!=head) {
            if(queued==pending.size())return {};
            pending[queued++]=child;
        }
    }
    result.valid=result.controller && equals(base+0xd96808,parent) && equals(parent,base+0xd1266c) &&
        equals(parent+0x1a4,head) && equals(head+4,root) && ReadLocalControllerIdentity(base,result.controller,read) &&
        equals(result.controller+0x10,result.input) && equals(result.controller+0x5c,result.map);
    if(result.physics && !equals(result.car+0x30,result.physics))result.physics=0;
    return result;
}
}
