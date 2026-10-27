#include "chase_observer_prequery.h"
#include <cstdio>
#include <map>
#include <vector>

using namespace grid2vr::chaseobserver;
namespace {
constexpr uintptr_t base=0x400000,camera=0x20000000,timing=0x21000000;
constexpr uintptr_t provider=0x22000000,entity=0x23000000,output=0x24000000;
struct Memory {
    std::map<uintptr_t,uint8_t> bytes;
    std::vector<std::pair<uintptr_t,size_t>> reads;
    uintptr_t changeAt{};unsigned changeHits{},changeWhen{2};bool eraseOnChange{};
    void Put(uintptr_t at,uint32_t value,unsigned size=4) {
        for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<uint8_t>(value>>(8*i));
    }
    void Name(const char* value) {
        for(unsigned i=0;i<32;++i)bytes[camera+0x19+i]=0;
        for(size_t i=0;i<std::strlen(value);++i)bytes[camera+0x19+i]=static_cast<uint8_t>(value[i]);
    }
    bool Read(uintptr_t at,void* destination,size_t size) {
        reads.emplace_back(at,size);
        if(at<0x10000 || size>0x1b0 || at>UINT32_MAX-size)return false;
        if(at==changeAt && ++changeHits==changeWhen) {
            if(eraseOnChange)bytes.erase(at);
            else bytes[at]^=1;
        }
        auto* out=static_cast<uint8_t*>(destination);
        for(size_t i=0;i<size;++i) {
            const auto found=bytes.find(at+i);if(found==bytes.end())return false;
            out[i]=found->second;
        }
        return true;
    }
};
Memory Fixture() {
    Memory m;
    m.Put(camera,base+0xd09948);m.Put(camera+0x60,provider);m.Put(provider,base+0xd0c068);m.Put(provider+8,entity);
    m.Put(camera+0x0c,1);m.Put(camera+0xa4,0,1);m.Name("chase_close");m.Put(camera+0x98,0x12345678);
    for(unsigned i=0;i<108;++i)m.Put(camera+0xb0+4*i,0x100000+i);
    m.Put(camera+0xb0,0x7fc01234);m.Put(camera+0xb0+0x1a4,0x100000); // Preserve raw nonfinite bits.
    for(unsigned i=0;i<4;++i)m.Put(timing+4*i,0x200000+i);
    for(unsigned i=0;i<12;++i)m.Put(entity+0x300+4*i,0x300000+i);
    constexpr uint32_t position[]{0x7fc00001,0x80000000,0x7f800000,0};
    for(unsigned i=0;i<4;++i) { m.Put(entity+0x330+4*i,position[i]);m.Put(output+4*i,position[i]); }
    for(unsigned i=0;i<12;++i)m.Put(camera+0x2430+4*i,0x400000+i);
    m.Put(provider+0x0c,0x25000000);m.Put(camera+0x24c0,0x26000000);m.Put(camera+0x24c4,0x27000000);
    m.Put(camera+0x24d8,3);m.Put(entity+0x11a8,0x28000000);m.Put(camera+0xae,1,1);m.Put(camera+0x2564,1,1);
    unsigned n=0;for(uintptr_t offset:{0x253c,0x2540,0x2544,0x254c})m.Put(camera+offset,0x500000+n++);
    return m;
}
const char* Capture(Memory& m,PreQueryObservation& observed,uintptr_t image=base,uintptr_t self=camera,
                    uintptr_t nativeTiming=timing,uintptr_t nativeProvider=provider,
                    uintptr_t nativeOutput=output,uintptr_t returned=output) {
    return CapturePreQuery(image,self,nativeTiming,nativeProvider,nativeOutput,returned,
        [&](uintptr_t at,void* destination,size_t size){return m.Read(at,destination,size);},observed);
}
bool Reason(const char* actual,const char* expected) { return actual && !std::strcmp(actual,expected); }
}
int main() {
    auto m=Fixture();const auto unchanged=m.bytes;PreQueryObservation observed;
    if(Capture(m,observed) || m.bytes!=unchanged || m.reads.size()!=50)return 1;
    const auto reads=m.reads;
    if(observed.camera!=camera || observed.timing!=timing || observed.provider!=provider || observed.output!=output ||
       observed.returned!=output || observed.record!=camera+0xb0 || observed.cameraTable!=base+0xd09948 ||
       observed.providerTable!=base+0xd0c068 || observed.entity!=entity || observed.camera0c!=1 || observed.cameraa4 ||
       observed.nameHash!=0x12345678 || std::strcmp(observed.name.data(),"chase_close") || observed.provider0c!=0x25000000 ||
       observed.geometry24c0!=0x26000000 || observed.geometry24c4!=0x27000000 || observed.constraint24d8!=3 ||
       observed.entity11a8!=0x28000000 || observed.cameraae!=1 || observed.camera2564!=1)return 2;
    for(unsigned i=0;i<108;++i) {
        const auto expected=i==0?0x7fc01234u:i==0x1a4/4?0x100000u:0x100000u+i;
        if(observed.recordWords[i]!=expected)return 3;
    }
    for(unsigned i=0;i<4;++i)if(observed.timingWords[i]!=0x200000+i || observed.obstructionWords[i]!=0x500000+i)return 4;
    for(unsigned i=0;i<12;++i)if(observed.entityBasisWords[i]!=0x300000+i || observed.localOffsetWords[i]!=0x400000+i)return 5;
    if(observed.outputWords!=observed.entityPositionWords || observed.outputWords!=std::array<uint32_t,4>{0x7fc00001,0x80000000,0x7f800000,0})return 6;
    for(size_t i=0;i<25;++i)if(reads[i]!=reads[i+25] || reads[i].second>0x1b0)return 7;

    m=Fixture();m.Name("chase_far");if(Capture(m,observed) || std::strcmp(observed.name.data(),"chase_far"))return 8;
    for(const char* name:{"cockpit","chase_close_extra","chase_farther","CHASE_CLOSE",""}) {
        m=Fixture();m.Name(name);if(!Reason(Capture(m,observed),"prequery_camera_name"))return 9;
    }
    for(uintptr_t bad:{uintptr_t(0),uintptr_t(0x10001),uintptr_t(0xfffffff0)}) {
        for(unsigned argument=0;argument<5;++argument) {
            m=Fixture();const auto result=Capture(m,observed,argument==0?bad:base,argument==1?bad:camera,
                argument==2?bad:timing,argument==3?bad:provider,argument==4?bad:output);
            if(!Reason(result,"prequery_address_invalid") || !m.reads.empty())return 10;
        }
    }
    m=Fixture();if(!Reason(Capture(m,observed,0xff300000),"prequery_address_invalid") || !m.reads.empty())return 11;
    m=Fixture();if(!Reason(Capture(m,observed,base,camera,timing,provider,output+4,output+4),"prequery_address_invalid") || !m.reads.empty())return 12;
    m=Fixture();if(!Reason(Capture(m,observed,base,camera,timing,provider,output,output+16),"prequery_return_mismatch") || !m.reads.empty())return 13;
    struct Mismatch { uintptr_t address;uint32_t value;const char* reason;unsigned size{4}; };
    for(const auto& change:std::array<Mismatch,9>{{
        {camera,base+0xd0994c,"prequery_camera_table"},
        {provider,base+0xd0c06c,"prequery_provider_table"},
        {camera+0x60,provider+4,"prequery_provider_mismatch"},
        {provider+8,0,"prequery_identity_unreadable"},
        {camera+0x0c,0,"prequery_camera_gate"},
        {camera+0xa4,1,"prequery_camera_gate",1},
        {camera+0xb0+0x1a4,2,"prequery_record_gate"},
        {output,0,"prequery_output_mismatch"},
        {entity+0x330,0,"prequery_output_mismatch"}}}) {
        m=Fixture();m.Put(change.address,change.value,change.size);
        if(!Reason(Capture(m,observed),change.reason))return 14;
    }
    // Only the traced flag bit is a refusal; other raw flags/context values
    // remain observations rather than inferred validity constraints.
    m=Fixture();m.Put(camera+0xb0+0x1a4,0xfffffffd);m.Put(camera+0x24c0,0);m.Put(camera+0x24c4,0);
    m.Put(provider+0x0c,0);m.Put(entity+0x11a8,0);
    if(Capture(m,observed) || observed.recordWords[0x1a4/4]!=0xfffffffd || observed.geometry24c0 || observed.provider0c)return 15;

    // Every distinct read surface must be readable initially and remain stable
    // on the second pass. No callback writes or retries are hidden by a failure.
    for(size_t i=0;i<25;++i) {
        const auto address=reads[i].first;
        m=Fixture();m.bytes.erase(address);
        if(!Capture(m,observed) || m.reads.size()>50)return 16;
        m=Fixture();m.changeAt=address;
        if(!Reason(Capture(m,observed),"prequery_changed") || m.reads.size()>50)return 17;
        m=Fixture();m.changeAt=address;m.eraseOnChange=true;
        if(!Reason(Capture(m,observed),"prequery_changed") || m.reads.size()>50)return 18;
    }
    // A truncated payload must not succeed merely because its first word is readable.
    for(const auto [address,size]:reads) {
        m=Fixture();m.bytes.erase(address+size-1);
        if(!Capture(m,observed) || m.reads.size()>50)return 19;
    }
    puts("Chase pre-query observation: bounded raw snapshots, exact identities/gates and changing-source refusal; synthetic only.");
}
