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
// Diagnostic only: the first check that refused the route and what it saw.
// Image values are RVAs (0 when unread or outside the image).
struct RouteProbe {
    const char* step="none";uint32_t parentTable{},nodes{},roles{},roleValues[8]{},controllerBytes{},controllerTable{};
};
// Read the local driving controller's actual query map. The saved map alone is
// not evidence that gameplay consumes it. No pointer or player ID is changed.
template<class Reader> InputRoute ReadInputRoute(uintptr_t base,Reader read,RouteProbe* probe) {
    InputRoute result;
    RouteProbe ignored;auto& why=probe?*probe:ignored;why={};
    const auto rva=[&](uint32_t value){return value>=base && value-base<0x1000000?static_cast<uint32_t>(value-base):0u;};
    const auto refuse=[&](const char* step){why.step=step;return InputRoute{};};
    const auto word=[&](uintptr_t at,uint32_t& value){return read(at,&value,4);};
    const auto equals=[&](uintptr_t at,uintptr_t expected){uint32_t value{};return word(at,value)&&value==expected;};
    uint32_t parent{},head{},root{},table{};
    if(!word(base+0xd96808,parent))return refuse("parent_pointer");
    if(word(parent,table))why.parentTable=rva(table);
    if(table!=base+0xd1266c)return refuse("parent_table");
    if(!word(parent+0x1a4,head) || !word(head+4,root))return refuse("tree");
    // Normal-launch events field more than eight cars; the bound only stops a
    // corrupt or cyclic tree.
    std::array<uint32_t,32> pending{},seen{};size_t queued{},visited{};
    if(root!=head)pending[queued++]=root;
    while(queued) {
        const auto node=pending[--queued];
        if(visited==seen.size())return refuse("node_limit");
        for(size_t i=0;i<visited;++i)if(seen[i]==node)return refuse("node_cycle");
        seen[visited++]=node;why.nodes=static_cast<uint32_t>(visited);
        std::array<uint32_t,5> words{};uint32_t player{};
        if(!read(node,words.data(),sizeof(words)) || !word(words[3]+0xb0,player))return refuse("node_read");
        if(why.roles<std::size(why.roleValues))why.roleValues[why.roles]=player;
        ++why.roles;
        if(player==0) {
            uint32_t begin{},end{},controller{},input{},map{},controllerTable{};
            if(result.controller)return refuse("second_local_car");
            if(!word(words[4]+8,begin) || !word(words[4]+12,end) || end<begin)return refuse("controllers");
            why.controllerBytes=end-begin;
            if(end-begin!=8 || !word(begin,controller))return refuse("controllers");
            if(word(controller,controllerTable))why.controllerTable=rva(controllerTable);
            if(!ReadLocalControllerIdentity(base,controller,read))return refuse("controller_identity");
            if(!word(controller+0x10,input) || !word(controller+0x5c,map) ||
               !equals(words[4]+8,begin) || !equals(words[4]+12,end) || !equals(begin,controller))return refuse("controller_fields");
            uint32_t physics{};
            // Optional for legacy input routing; the shift gate requires this
            // exact car-to-physics link, also used by native dispatch520cdb.
            if(!word(words[3]+0x30,physics))physics=0;
            result={controller,input,map,false,words[3],physics};
        }
        for(auto child:{words[0],words[2]})if(child!=head) {
            if(queued==pending.size())return refuse("node_limit");
            pending[queued++]=child;
        }
    }
    if(!result.controller)return refuse("no_local_car");
    result.valid=result.controller && equals(base+0xd96808,parent) && equals(parent,base+0xd1266c) &&
        equals(parent+0x1a4,head) && equals(head+4,root) && ReadLocalControllerIdentity(base,result.controller,read) &&
        equals(result.controller+0x10,result.input) && equals(result.controller+0x5c,result.map);
    if(result.physics && !equals(result.car+0x30,result.physics))result.physics=0;
    if(!result.valid)why.step="changed";
    return result;
}
template<class Reader> InputRoute ReadInputRoute(uintptr_t base,Reader read) {return ReadInputRoute(base,read,nullptr);}
}
