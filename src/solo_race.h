#pragma once
#include "guarded_hooks.h"
#include "local_controller_identity.h"
#include <array>

namespace grid2vr {
inline bool LocalControllerIdentity(uintptr_t base,uintptr_t object) {
    return ReadLocalControllerIdentity(base,object,[](uintptr_t at,void* output,size_t bytes){
        return ReadMemory(reinterpret_cast<void*>(at),output,bytes);});
}
// Read-only readiness for the single-car normal entry observed on 2026-10-10.
// Both image references must identify the same live parent. This is not a
// general race/LAN gate and does not enable or select any controller.
inline bool NormalSoloRaceReady(uintptr_t base) {
    const auto word=[](uintptr_t at,uint32_t expected) {
        uint32_t value{};return ReadMemory(reinterpret_cast<void*>(at),&value,sizeof(value)) && value==expected;
    };
    const auto byte=[](uintptr_t at,uint8_t expected) {
        uint8_t value{};return ReadMemory(reinterpret_cast<void*>(at),&value,1) && value==expected;
    };
    const auto pointer=[](uintptr_t at,uintptr_t& value) {
        return ReadMemory(reinterpret_cast<void*>(at),&value,sizeof(value)) && value>=0x10000 &&
            value<=UINTPTR_MAX-0x10000 && value%4==0;
    };
    uintptr_t parent{},otherParent{},begin{},end{},car{},head{},node{},group{},children{},childrenEnd{},local{},ai{};
    if(!word(base+0xe61010,0) || !word(base+0xe5917c,2) ||
       !pointer(base+0xd96808,parent) || !pointer(base+0xdaa424,otherParent) || parent!=otherParent ||
       !word(parent,static_cast<uint32_t>(base+0xd1266c)) ||
       !pointer(parent+0xa8,begin) || !pointer(parent+0xac,end) || end-begin!=4 || !pointer(begin,car) || !word(car+0xb0,0)) return false;
    const auto manager=parent+0x190;
    if(!word(manager,static_cast<uint32_t>(base+0xd0bc48)) || !word(manager+0x18,1) || !byte(manager+0x24,1) ||
       !word(manager+0x2c,4) || !word(manager+0x30,2) || !byte(manager+0x34,0) ||
       !pointer(manager+0x14,head) || !pointer(head,node) || !word(node+12,static_cast<uint32_t>(car)) ||
       !pointer(node+16,group) || !word(group,static_cast<uint32_t>(base+0xd0bb30)) || !word(group+0x18,0) ||
       !pointer(group+8,children) || !pointer(group+12,childrenEnd) || childrenEnd-children!=8 ||
       !pointer(children,local) || !pointer(children+4,ai)) return false;
    return LocalControllerIdentity(base,local) && byte(local+0x40,1) &&
        word(ai,static_cast<uint32_t>(base+0xd039ac));
}

// Read-only extension from the observed two- and eight-car stock races (2026-10-24).
// The first car must own the local controller; all remaining cars must have
// only their native AI controller. This is deliberately not a LAN/replay gate.
inline bool NormalLocalRaceReady(uintptr_t base) {
    const auto word=[](uintptr_t at,uint32_t expected) {
        uint32_t value{};return ReadMemory(reinterpret_cast<void*>(at),&value,sizeof(value)) && value==expected;
    };
    const auto byte=[](uintptr_t at,uint8_t expected) {
        uint8_t value{};return ReadMemory(reinterpret_cast<void*>(at),&value,1) && value==expected;
    };
    const auto pointer=[](uintptr_t at,uintptr_t& value) {
        return ReadMemory(reinterpret_cast<void*>(at),&value,sizeof(value)) && value>=0x10000 &&
            value<=UINTPTR_MAX-0x10000 && value%4==0;
    };
    uintptr_t parent{},otherParent{},begin{},end{};
    if(!word(base+0xe61010,0) || !word(base+0xe5917c,2) ||
       !pointer(base+0xd96808,parent) || !pointer(base+0xdaa424,otherParent) || parent!=otherParent ||
       !word(parent,static_cast<uint32_t>(base+0xd1266c)) ||
       !pointer(parent+0xa8,begin) || !pointer(parent+0xac,end) || end<=begin || (end-begin)%4 || (end-begin)/4>8) return false;
    const auto count=static_cast<unsigned>((end-begin)/4);
    std::array<uintptr_t,8> cars{},seen{},groups{},controllers{};
    for(unsigned i=0;i<count;++i) {
        // +0xb0 is the local/AI role, not the position in the car vector.
        if(!pointer(begin+i*4,cars[i]) || !word(cars[i],static_cast<uint32_t>(base+0xcdfeb4)) || !word(cars[i]+0xb0,i?1:0))return false;
        for(unsigned j=0;j<i;++j)if(cars[i]==cars[j])return false;
    }
    const auto manager=parent+0x190;
    uintptr_t head{},root{},first{},last{};
    if(!word(manager,static_cast<uint32_t>(base+0xd0bc48)) || !word(manager+0x18,count) || !byte(manager+0x24,1) ||
       !word(manager+0x2c,4) || !word(manager+0x30,2) || !byte(manager+0x34,0) ||
       !pointer(manager+0x14,head) || !pointer(head+4,root) || root==head ||
       !pointer(head,first) || !pointer(head+8,last))return false;
    struct Pending { uintptr_t node,parent,lower,upper; };
    std::array<Pending,8> pending{};unsigned queued=1,visited=0,owned=0;
    pending[0]={root,head,0,UINTPTR_MAX};
    uintptr_t minimum=UINTPTR_MAX,maximum=0,minimumNode{},maximumNode{},localController{};
    while(queued) {
        const auto current=pending[--queued];
        if(visited>=count)return false;
        for(unsigned i=0;i<visited;++i)if(seen[i]==current.node)return false;
        uintptr_t left{},right{},owner{},car{},group{},children{},childrenEnd{},ai{};
        if(!pointer(current.node,left) || !pointer(current.node+4,owner) || owner!=current.parent ||
           !pointer(current.node+8,right) || !byte(current.node+0x15,0) ||
           !pointer(current.node+12,car) || car<=current.lower || car>=current.upper ||
           !pointer(current.node+16,group) || !word(group,static_cast<uint32_t>(base+0xd0bb30)) || !word(group+0x18,0) ||
           !pointer(group+8,children) || !pointer(group+12,childrenEnd) || childrenEnd<=children)return false;
        unsigned index=0;while(index<count && cars[index]!=car)++index;
        if(index==count || (owned&(1u<<index)))return false;
        for(unsigned i=0;i<visited;++i)if(groups[i]==group)return false;
        if(index==0) {
            if(childrenEnd-children!=8 || !pointer(children,localController) ||
               !LocalControllerIdentity(base,localController) || !byte(localController+0x40,1) ||
               !pointer(children+4,ai))return false;
        } else if(childrenEnd-children!=4 || !pointer(children,ai))return false;
        if(!word(ai,static_cast<uint32_t>(base+0xd039ac)))return false;
        for(unsigned i=0;i<visited;++i)if(controllers[i]==ai)return false;
        seen[visited]=current.node;groups[visited]=group;controllers[visited++]=ai;owned|=1u<<index;
        if(car<minimum) { minimum=car;minimumNode=current.node; }
        if(car>maximum) { maximum=car;maximumNode=current.node; }
        if(left!=head) {
            if(queued>=pending.size())return false;
            pending[queued++]={left,current.node,current.lower,car};
        }
        if(right!=head) {
            if(queued>=pending.size())return false;
            pending[queued++]={right,current.node,car,current.upper};
        }
    }
    if(visited!=count || owned!=(1u<<count)-1 || first!=minimumNode || last!=maximumNode || !localController)return false;
    // Reject a graph that changed while the passive checks were in progress.
    return word(base+0xd96808,static_cast<uint32_t>(parent)) && word(base+0xdaa424,static_cast<uint32_t>(parent)) &&
        word(parent+0xa8,static_cast<uint32_t>(begin)) && word(parent+0xac,static_cast<uint32_t>(end)) && word(manager+0x18,count) &&
        word(base+0xe61010,0) && word(base+0xe5917c,2);
}
}
