#include "chase_observer_render.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

using namespace grid2vr::chaseobserver;
namespace {
constexpr uintptr_t base=0x400000,root=0x20000000,state=0x21000000,controller=0x22000000;
constexpr uintptr_t manager=0x23000000,camera=0x24000000,mainObject=0x26000000;
constexpr uintptr_t renderer=0x27000000,context=0x28000000,packet=0x29000000;
struct Memory {
    std::map<uintptr_t,uint8_t> bytes;
    std::vector<std::pair<uintptr_t,size_t>> reads;
    uintptr_t changeAt{};uint32_t changeTo{};unsigned changeSize{4},changeHits{},changeWhen{2};
    void Put(uintptr_t at,uint32_t value,unsigned size=4) {
        for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<uint8_t>(value>>(8*i));
    }
    bool Read(uintptr_t at,void* output,size_t size) {
        reads.emplace_back(at,size);
        if(at<0x10000 || size>0x200 || at>UINT32_MAX-size)return false;
        if(at==changeAt && ++changeHits==changeWhen)Put(changeAt,changeTo,changeSize);
        auto* destination=static_cast<uint8_t*>(output);
        for(size_t i=0;i<size;++i) {
            const auto found=bytes.find(at+i);
            if(found==bytes.end())return false;
            destination[i]=found->second;
        }
        return true;
    }
};
Memory Fixture() {
    Memory m;
    m.Put(base+0xdef810,root);m.Put(root+0x10,state);m.Put(state+0x14fc,controller);
    m.Put(controller+0x392,0,1);m.Put(controller+0x393,0,1);m.Put(controller+0x33c,manager);
    m.Put(manager+0x748,camera);m.Put(manager+0x759,0,1);m.Put(camera,base+0xd09948);
    m.Put(mainObject,base+0xcd9da8);m.Put(mainObject+0x14,0x400);m.Put(mainObject+0x18,1,1);
    m.Put(mainObject+0x94,0x30000000);m.Put(mainObject+0x28,renderer);m.Put(renderer,base+0xd4a5a0);
    m.Put(mainObject+0x5e4,context);
    // Distinct payloads prove that the reader captures rather than synthesizes
    // agreement or silently copies one view into another observation.
    unsigned region=1;
    for(auto address:{controller+0x1f0,base+0xdff9b0,mainObject+0x5f0,mainObject+0x680}) {
        for(unsigned i=0;i<36;++i)m.Put(address+4*i,region*0x100000+i);
        ++region;
    }
    for(unsigned i=0;i<128;++i)m.Put(context+4*i,0x500000+i);
    return m;
}
std::array<uintptr_t,25> OwnerStack() {
    std::array<uintptr_t,25> s{};s[0]=base+0x8047a0;s[18]=s[21]=base+0x1185af;s[22]=controller+0x1f0;return s;
}
std::array<uintptr_t,15> SceneStack() {
    return {base+0x3be09e,context,mainObject+0x680,packet,1,2,3,4,base+0x7a7cd2,
        packet,mainObject+0x5f0,mainObject+0x680,context,0x31000000,1};
}
const char* Capture(Memory& m,const OwnerObservation& source,RenderObservation& observed,
                    std::span<const uintptr_t> scene,std::span<const uintptr_t> owner,
                    uintptr_t self=mainObject,uintptr_t nativeContext=context,uintptr_t view=mainObject+0x680,
                    uintptr_t image=base) {
    return CaptureRender(image,self,camera,source,owner,nativeContext,view,packet,scene,
        [&](uintptr_t at,void* output,size_t size){return m.Read(at,output,size);},observed);
}
bool Reason(const char* actual,const char* expected) { return actual && !std::strcmp(actual,expected); }
}
int main() {
    const auto ownerStack=OwnerStack();const auto sceneStack=SceneStack();
    auto m=Fixture();OwnerObservation source;RenderObservation observed;
    if(DecodeOwner(ownerStack,base,camera,[&](uintptr_t at,void* output,size_t size){return m.Read(at,output,size);},source))return 1;
    m.reads.clear();const auto before=m.bytes;
    if(Capture(m,source,observed,sceneStack,ownerStack) || m.bytes!=before || m.reads.size()!=55 ||
       observed.main!=mainObject || observed.mainTable!=base+0xcd9da8 || observed.type!=0x400 || observed.active!=1 ||
       observed.renderer!=renderer || observed.rendererTable!=base+0xd4a5a0 || observed.target!=0x30000000 ||
       observed.context!=context || observed.owner!=source || observed.call.args[1]!=mainObject+0x5f0 ||
       !observed.exportedReadable || !observed.globalReadable || !observed.mainReadable ||
       !observed.renderReadable || !observed.contextReadable)return 2;
    for(unsigned i=0;i<36;++i)if(observed.exportedView[i]!=0x100000+i || observed.globalView[i]!=0x200000+i ||
        observed.mainView[i]!=0x300000+i || observed.renderView[i]!=0x400000+i)return 3;
    for(unsigned i=0;i<128;++i)if(observed.contextWords[i]!=0x500000+i)return 4;

    m=Fixture();if(!Reason(Capture(m,source,observed,{sceneStack.data(),14},ownerStack),"render_scene_stack") || !m.reads.empty())return 5;
    for(unsigned index:{0u,1u,2u,3u,8u,9u,11u,12u,14u}) {
        auto broken=sceneStack;broken[index]^=4;m=Fixture();
        if(!Reason(Capture(m,source,observed,broken,ownerStack),"render_scene_stack") || !m.reads.empty())return 6;
    }
    auto wrongView=sceneStack;wrongView[2]=wrongView[11]=mainObject+0x5f0;wrongView[10]=mainObject+0x680;m=Fixture();
    if(!Reason(Capture(m,source,observed,wrongView,ownerStack,mainObject,context,mainObject+0x5f0),"render_view_arguments"))return 7;
    auto wrongOther=sceneStack;wrongOther[10]=mainObject+0x680;m=Fixture();
    if(!Reason(Capture(m,source,observed,wrongOther,ownerStack),"render_view_arguments"))return 8;
    for(uintptr_t bad:{uintptr_t(0),uintptr_t(0x10001),uintptr_t(0xfffffff0)}) {
        m=Fixture();if(!Reason(Capture(m,source,observed,sceneStack,ownerStack,bad),"render_address_invalid") || !m.reads.empty())return 9;
    }
    m=Fixture();if(!Reason(Capture(m,source,observed,sceneStack,ownerStack,mainObject,context,mainObject+0x680,0xfff00000),"render_address_invalid"))return 10;
    const std::array<uintptr_t,7> identityFields{mainObject,mainObject+0x14,mainObject+0x18,
        mainObject+0x94,mainObject+0x28,renderer,mainObject+0x5e4};
    for(auto address:identityFields) {
        m=Fixture();m.bytes.erase(address);
        if(!Reason(Capture(m,source,observed,sceneStack,ownerStack),"render_main_identity"))return 11;
        m=Fixture();m.Put(address,0,address==mainObject+0x18?1:4);
        if(!Reason(Capture(m,source,observed,sceneStack,ownerStack),"render_main_identity"))return 12;
        m=Fixture();m.changeAt=address;m.changeSize=address==mainObject+0x18?1:4;m.changeTo=0;
        if(!Reason(Capture(m,source,observed,sceneStack,ownerStack),"render_identity_changed") || m.reads.size()>55)return 13;
    }
    auto wrongSource=source;wrongSource.output+=4;m=Fixture();
    if(!Reason(Capture(m,wrongSource,observed,sceneStack,ownerStack),"render_source_changed"))return 14;
    auto wrongOwner=ownerStack;wrongOwner[18]=0;m=Fixture();
    if(!Reason(Capture(m,source,observed,sceneStack,wrongOwner),"owner_export_caller"))return 15;
    for(auto address:{base+0xdef810,root+0x10,state+0x14fc,controller+0x33c,manager+0x748,camera}) {
        m=Fixture();m.changeAt=address;m.changeTo=0;m.changeWhen=3;
        if(!Reason(Capture(m,source,observed,sceneStack,ownerStack),"render_identity_changed") || m.reads.size()>55)return 16;
    }
    for(auto address:{controller+0x1f0,base+0xdff9b0,mainObject+0x5f0,mainObject+0x680,context}) {
        m=Fixture();m.bytes.erase(address+0x40);
        if(!Reason(Capture(m,source,observed,sceneStack,ownerStack),address==context?"render_context_unreadable":"render_view_unreadable") ||
           m.reads.size()>55)return 17;
    }
    puts("Chase render observation: exact main-scene boundary, bounded raw snapshots and unstable-identity refusal; synthetic only.");
}
