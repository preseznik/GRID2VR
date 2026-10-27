#include "chase_observer_owner.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

using namespace grid2vr::chaseobserver;
namespace {
constexpr uintptr_t base=0x400000,root=0x20000000,state=0x21000000,controller=0x22000000;
constexpr uintptr_t manager=0x23000000,camera=0x24000000,alternate=0x25000000;
struct Memory {
    std::map<uintptr_t,uint8_t> bytes;
    std::vector<uintptr_t> reads;
    uintptr_t changeAt{};uint32_t changeTo{};unsigned changeSize{4},changeHits{};
    void Put(uintptr_t at,uint32_t value,unsigned size=4) {
        for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<uint8_t>(value>>(8*i));
    }
    bool Read(uintptr_t at,void* output,size_t size) {
        reads.push_back(at);
        if(at<0x10000 || size>4 || at>UINT32_MAX-size)return false;
        if(at==changeAt && ++changeHits==2)Put(changeAt,changeTo,changeSize);
        auto* destination=static_cast<uint8_t*>(output);
        for(size_t i=0;i<size;++i) {
            const auto found=bytes.find(at+i);
            if(found==bytes.end())return false;
            destination[i]=found->second;
        }
        return true;
    }
};
Memory Fixture(bool useAlternate=false) {
    Memory memory;
    memory.Put(base+0xdef810,root);memory.Put(root+0x10,state);memory.Put(state+0x14fc,controller);
    memory.Put(controller+0x392,0,1);memory.Put(controller+0x393,0,1);
    memory.Put(controller+0x33c,manager);memory.Put(manager+0x748,camera);
    memory.Put(manager+0x759,useAlternate?1:0,1);memory.Put(camera+0x70,useAlternate?alternate:0);
    memory.Put(camera,base+0xd09948);memory.Put(alternate,base+0xd09948);
    return memory;
}
std::array<uintptr_t,25> Stack(unsigned returnWord=21,uintptr_t outerReturn=0x1185af) {
    std::array<uintptr_t,25> stack{};
    stack[0]=base+0x8047a0;stack[18]=base+outerReturn;
    stack[returnWord]=stack[18];stack[returnWord+1]=controller+0x1f0;
    return stack;
}
const char* Decode(Memory& memory,std::span<const uintptr_t> stack,OwnerObservation& observed,
                   uintptr_t self=camera,uintptr_t image=base) {
    return DecodeOwner(stack,image,self,[&](uintptr_t at,void* output,size_t size) {
        return memory.Read(at,output,size);
    },observed);
}
bool Reason(const char* actual,const char* expected) { return actual && !std::strcmp(actual,expected); }
}
int main() {
    OwnerObservation observed;
    // Derive the original-return position independently from the native stack
    // arithmetic for all four possible outer stack alignment residues.
    for(uintptr_t residue:{0u,4u,8u,12u})for(uintptr_t caller:{0x1185afu,0x21a621u}) {
        const uintptr_t outer=0x100000+residue;
        const uintptr_t aligned=(outer-12)&~uintptr_t(15),getterReturn=aligned-0x44;
        const auto word=static_cast<unsigned>((outer-getterReturn)/4);
        if((aligned+4-getterReturn)/4!=18 || word<20 || word>23)return 1;
        auto memory=Fixture();const auto before=memory.bytes;
        if(Decode(memory,Stack(word,caller),observed) || observed.callerRva!=caller ||
           observed.output!=controller+0x1f0 || observed.controller!=controller || observed.globalController!=controller || observed.root!=root ||
           observed.state!=state || observed.manager!=manager || observed.selected!=camera ||
           observed.record!=camera+0xb0 || observed.cameraTable!=base+0xd09948 ||
           memory.bytes!=before || memory.reads.size()!=18)return 2;
    }
    auto stack=Stack();auto memory=Fixture();
    if(!Reason(Decode(memory,{stack.data(),24},observed),"owner_stack_short") || !memory.reads.empty())return 3;
    for(unsigned index:{0u,18u}) {
        auto bad=stack;bad[index]^=4;memory=Fixture();
        if(!Decode(memory,bad,observed) || !memory.reads.empty())return 4;
    }
    for(bool duplicate:{false,true}) {
        auto bad=stack;if(duplicate)bad[23]=bad[18];else bad[21]=0;
        memory=Fixture();
        if(!Reason(Decode(memory,bad,observed),"owner_stack_ambiguous") || !memory.reads.empty())return 5;
    }
    for(uintptr_t bad:{uintptr_t(0),uintptr_t(0x10004),uintptr_t(0xfffffff0),uintptr_t(0x220001f1)}) {
        auto broken=stack;broken[22]=bad;memory=Fixture();
        if(!Reason(Decode(memory,broken,observed),"owner_output_invalid") || !memory.reads.empty())return 6;
    }
    for(uintptr_t bad:{uintptr_t(0),uintptr_t(0x10001),uintptr_t(0xfffffff0)}) {
        memory=Fixture();
        if(!Reason(Decode(memory,stack,observed,bad),"owner_address_invalid") || !memory.reads.empty())return 7;
    }
    memory=Fixture();
    if(!Reason(Decode(memory,stack,observed,camera,0xfff00000),"owner_address_invalid") || !memory.reads.empty())return 8;
    auto foreign=stack;foreign[22]+=0x1000;memory=Fixture();
    if(!Reason(Decode(memory,foreign,observed),"owner_controller_mismatch"))return 9;
    memory=Fixture();memory.Put(state+0x14fc,controller+0x1000);
    if(!Reason(Decode(memory,stack,observed),"owner_controller_mismatch") || observed.globalController!=controller+0x1000)return 10;
    for(unsigned offset:{0x392u,0x393u}) {
        memory=Fixture();memory.Put(controller+offset,1,1);
        if(!Reason(Decode(memory,stack,observed),"owner_controller_busy"))return 11;
    }
    memory=Fixture();memory.Put(camera,base+0xd09944);
    if(!Reason(Decode(memory,stack,observed),"owner_camera_table"))return 12;
    memory=Fixture();memory.Put(manager+0x748,alternate);
    if(!Reason(Decode(memory,stack,observed),"owner_camera_mismatch"))return 13;
    for(uint8_t enabled:{uint8_t(1),uint8_t(2)}) {
        memory=Fixture(true);memory.Put(manager+0x759,enabled,1);
        if(Decode(memory,stack,observed,alternate) || observed.primary!=camera || observed.alternate!=alternate ||
           observed.selected!=alternate || observed.alternateEnabled!=enabled || memory.reads.size()!=20)return 14;
    }
    memory=Fixture(true);memory.Put(camera+0x70,0);
    if(Decode(memory,stack,observed) || observed.alternate || observed.selected!=camera)return 15;
    memory=Fixture();memory.bytes.erase(camera+0x70);
    if(Decode(memory,stack,observed))return 16; // Disabled alternate must not be read.

    const std::array<uintptr_t,10> graphAddresses{base+0xdef810,root+0x10,state+0x14fc,
        controller+0x392,controller+0x393,controller+0x33c,manager+0x748,manager+0x759,camera+0x70,alternate};
    for(auto address:graphAddresses) {
        memory=Fixture(true);memory.bytes.erase(address);
        if(!Decode(memory,stack,observed,alternate) || memory.reads.size()>20)return 17;
    }
    // Each reread field changes independently; both valid changed pointers and
    // now-invalid fields must fail, with bounded reads and no retry loop.
    for(auto address:graphAddresses) {
        memory=Fixture(true);memory.changeAt=address;
        memory.changeSize=(address==controller+0x392 || address==controller+0x393 || address==manager+0x759)?1:4;
        memory.changeTo=memory.changeSize==1?2:0x26000000;
        if(!Reason(Decode(memory,stack,observed,alternate),"owner_graph_changed") || memory.reads.size()>20)return 18;
    }
    for(auto address:{base+0xdef810,root+0x10,state+0x14fc,controller+0x33c,manager+0x748,camera+0x70}) {
        memory=Fixture(true);memory.Put(address,0xfffffff0);
        if(!Decode(memory,stack,observed,alternate) || memory.reads.size()>20)return 19;
    }
    puts("Chase export provenance: bounded native stack/graph checks; synthetic fixtures only.");
}
