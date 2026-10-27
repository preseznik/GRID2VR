#include "chase_observer_destination.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

using namespace grid2vr::chaseobserver;
namespace {
constexpr uintptr_t base=0x400000,camera=0x20000000,root=0x21000000,state=0x22000000,controller=0x23000000;
constexpr uintptr_t returnSlot=0x3000100c,normalOutput=controller+0x1f0;
struct Memory {
    std::map<uintptr_t,uint8_t> bytes;
    std::vector<std::pair<uintptr_t,size_t>> reads;
    uintptr_t changeAt{};unsigned changeHits{};bool eraseOnChange{};
    void Put(uintptr_t at,uint32_t value,unsigned size=4) {
        for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<uint8_t>(value>>(8*i));
    }
    bool Read(uintptr_t at,void* destination,size_t size) {
        reads.emplace_back(at,size);
        if(at<0x10000 || size>0x90 || at>UINT32_MAX-size)return false;
        if(at==changeAt && ++changeHits==2) {
            if(eraseOnChange)bytes.erase(at);else bytes[at]^=4;
        }
        auto* out=static_cast<uint8_t*>(destination);
        for(size_t i=0;i<size;++i) {
            const auto found=bytes.find(at+i);if(found==bytes.end())return false;out[i]=found->second;
        }
        return true;
    }
};
std::array<uintptr_t,25> Stack(uintptr_t parent,unsigned index=23,uintptr_t slot=returnSlot) {
    std::array<uintptr_t,25> s{};
    s[0]=base+0x8047a0;s[17]=slot+0x1c4;s[18]=s[index]=base+parent;
    s[index+1]=parent==0x2e3c40?slot+0x74:normalOutput;
    return s;
}
Memory Fixture(std::span<const uintptr_t> stack,uintptr_t slot=returnSlot,uintptr_t self=camera,uintptr_t owner=controller) {
    Memory m;
    for(size_t i=0;i<stack.size();++i)m.Put(slot+4*i,static_cast<uint32_t>(stack[i]));
    m.Put(self,base+0xd09948);m.Put(base+0xdef810,root);m.Put(root+0x10,state);m.Put(state+0x14fc,owner);
    for(size_t i=0;i<0x90/4;++i)m.Put(slot+0x74+i*4,0x12340000+static_cast<uint32_t>(i));
    return m;
}
Destination Decode(Memory& m,std::span<const uintptr_t> stack,uintptr_t slot=returnSlot,uintptr_t image=base,uintptr_t self=camera) {
    return DecodeDestination(stack,slot,image,self,[&](uintptr_t at,void* output,size_t size){return m.Read(at,output,size);});
}
bool Reason(const Destination& actual,const char* expected) {
    return !actual.auxiliary && actual.reason && !std::strcmp(actual.reason,expected);
}
}
int main() {
    if(!Reason(Destination{},"destination_stack_unreadable"))return 30;
    // Both native normal parents and all converter alignment residues retain
    // their actual destination even when owner flag qualification is busy.
    for(uintptr_t parent:{0x1185af,0x21a621})for(unsigned index=20;index<=23;++index)for(unsigned flags=0;flags<4;++flags) {
        const auto stack=Stack(parent,index);auto m=Fixture(stack);
        m.Put(controller+0x392,flags&1,1);m.Put(controller+0x393,flags>>1,1);
        const auto before=m.bytes;const auto d=Decode(m,stack);
        if(d.reason || d.auxiliary || d.output!=normalOutput || m.bytes!=before || m.reads.size()!=2)return 1;
        for(const auto& r:m.reads)if(r!=std::pair<uintptr_t,size_t>{returnSlot,100})return 2;
    }
    auto stack=Stack(0x2e3c40);auto m=Fixture(stack);const auto before=m.bytes;auto d=Decode(m,stack);
    if(d.reason || !d.auxiliary || d.output!=returnSlot+0x74 || m.reads.size()!=11 || m.bytes!=before)return 3;
    const auto reads=m.reads;
    for(const auto& r:reads)if(r.second>0x90)return 4;
    m=Fixture(stack);if(!Reason(Decode(m,{stack.data(),24}),"destination_stack_short") || !m.reads.empty())return 5;
    for(uintptr_t bad:{uintptr_t(0),uintptr_t(0x10001),uintptr_t(0xfffffff0)}) {
        for(unsigned argument=0;argument<3;++argument) {
            m=Fixture(stack);d=Decode(m,stack,argument==0?bad:returnSlot,argument==1?bad:base,argument==2?bad:camera);
            if(!Reason(d,"destination_address_invalid") || !m.reads.empty())return 6;
        }
    }
    m=Fixture(stack);if(!Reason(Decode(m,stack,returnSlot,0xff300000),"destination_address_invalid"))return 7;
    m=Fixture(stack);if(!Reason(Decode(m,stack,returnSlot+4),"destination_address_invalid") || !m.reads.empty())return 8;
    auto bad=stack;bad[0]^=4;m=Fixture(bad);if(!Reason(Decode(m,bad),"destination_getter_caller"))return 9;
    bad=Stack(0x2e3bff);m=Fixture(bad);if(!Reason(Decode(m,bad),"destination_parent"))return 10;
    bad=stack;bad[20]=bad[18];m=Fixture(bad);if(!Reason(Decode(m,bad),"destination_stack_ambiguous"))return 11;
    bad=stack;bad[23]=0;m=Fixture(bad);if(!Reason(Decode(m,bad),"destination_stack_ambiguous"))return 12;
    for(uintptr_t output:{uintptr_t(0),uintptr_t(0xfffffff0),normalOutput+4}) {
        bad=stack;bad[24]=output;m=Fixture(bad);if(!Reason(Decode(m,bad),"destination_output_invalid"))return 13;
    }
    for(unsigned index=20;index<23;++index) {
        bad=Stack(0x2e3c40,index);m=Fixture(bad);if(!Reason(Decode(m,bad),"destination_auxiliary_frame"))return 14;
    }
    bad=stack;bad[17]+=16;m=Fixture(bad);if(!Reason(Decode(m,bad),"destination_auxiliary_frame"))return 15;
    bad=stack;bad[24]+=16;m=Fixture(bad);if(!Reason(Decode(m,bad),"destination_auxiliary_frame"))return 16;
    // Even a plausible local-frame output is not exempt if it overlaps source
    // record, the current shared controller view or the native global view.
    const auto local=returnSlot+0x74;
    m=Fixture(stack,returnSlot,local-0xb0);if(!Reason(Decode(m,stack,returnSlot,base,local-0xb0),"destination_overlap"))return 17;
    m=Fixture(stack,returnSlot,camera,local-0x1f0);if(!Reason(Decode(m,stack),"destination_overlap"))return 18;
    const auto globalSlot=base+0xdff9b0-0x74;bad=Stack(0x2e3c40,23,globalSlot);m=Fixture(bad,globalSlot);
    if(!Reason(Decode(m,bad,globalSlot),"destination_overlap"))return 19;
    m=Fixture(stack);m.Put(camera,base+0xd0994c);if(!Reason(Decode(m,stack),"destination_camera_table"))return 20;
    for(uintptr_t at:{camera,base+0xdef810,root+0x10,state+0x14fc}) {
        m=Fixture(stack);m.bytes.erase(at);if(!Reason(Decode(m,stack),"destination_identity_unreadable"))return 21;
        m=Fixture(stack);m.Put(at,0);if(!Reason(Decode(m,stack),"destination_identity_unreadable"))return 22;
        m=Fixture(stack);m.changeAt=at;if(!Reason(Decode(m,stack),"destination_identity_changed") || m.reads.size()>11)return 23;
    }
    for(uintptr_t at:{returnSlot,returnSlot+99}) {
        m=Fixture(stack);m.bytes.erase(at);if(!Reason(Decode(m,stack),"destination_stack_unreadable"))return 24;
    }
    m=Fixture(stack);m.Put(returnSlot+4,1);if(!Reason(Decode(m,stack),"destination_stack_changed"))return 25;
    m=Fixture(stack);m.changeAt=returnSlot;if(!Reason(Decode(m,stack),"destination_stack_changed") || m.reads.size()!=11)return 26;
    m=Fixture(stack);m.changeAt=returnSlot;m.eraseOnChange=true;if(!Reason(Decode(m,stack),"destination_stack_unreadable"))return 27;
    for(uintptr_t at:{local,local+0x8f}) {
        m=Fixture(stack);m.bytes.erase(at);if(!Reason(Decode(m,stack),"destination_output_unreadable"))return 28;
    }
    // A >32-bit copied value cannot silently truncate into a native word.
    if constexpr(sizeof(uintptr_t)>4) {
        bad=stack;bad[1]=static_cast<uintptr_t>(UINT32_MAX)+1;m=Fixture(bad);
        if(!Reason(Decode(m,bad),"destination_stack_changed"))return 29;
    }
    puts("Destination classification: busy normal writes, exact stack-only auxiliary proof and conservative malformed/overlap refusal; synthetic only.");
}
