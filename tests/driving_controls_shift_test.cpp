#include "driving_controls_shift.h"
#include "driving_controls_shift_native.h"
#include <windows.h>
#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace grid2vr::drivingcontrols;
namespace {
unsigned nativeCalls{},copyCalls{},events{};
float expectedClutch=.65f;
template<class T> T& At(uintptr_t p,size_t offset=0){return *reinterpret_cast<T*>(p+offset);}
void __fastcall Native(void* job,void*) {
    ++nativeCalls;++events;
    assert(At<float>(At<uintptr_t>(reinterpret_cast<uintptr_t>(job),8),0x12f0)==expectedClutch);
}
void __fastcall Copy(void* job,void*) {
    ++copyCalls;++events;
    assert(At<float>(At<uintptr_t>(reinterpret_cast<uintptr_t>(job),8),0x12f0)==expectedClutch);
}
struct Fixture {
    uintptr_t base{},physical{},car{};size_t size{};
    explicit Fixture(const char* path) {
        std::ifstream file(path,std::ios::binary);std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
        assert(bytes.size()>4096);auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(bytes.data());
        auto pe=reinterpret_cast<IMAGE_NT_HEADERS32*>(bytes.data()+dos->e_lfanew);
        assert(pe->Signature==IMAGE_NT_SIGNATURE && pe->OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC);
        size=pe->OptionalHeader.SizeOfImage;
        base=reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));assert(base);
        std::memcpy(reinterpret_cast<void*>(base),bytes.data(),pe->OptionalHeader.SizeOfHeaders);
        auto sections=IMAGE_FIRST_SECTION(pe);
        for(unsigned i=0;i<pe->FileHeader.NumberOfSections;++i) {
            const auto& s=sections[i];assert(size_t(s.PointerToRawData)+s.SizeOfRawData<=bytes.size());
            std::memcpy(reinterpret_cast<void*>(base+s.VirtualAddress),bytes.data()+s.PointerToRawData,s.SizeOfRawData);
        }
        const auto directory=pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        size_t offset=directory.VirtualAddress;
        while(offset<directory.VirtualAddress+directory.Size) {
            auto block=reinterpret_cast<IMAGE_BASE_RELOCATION*>(base+offset);assert(block->SizeOfBlock>=8);
            auto entries=reinterpret_cast<uint16_t*>(block+1);
            for(size_t i=0;i<(block->SizeOfBlock-8)/2;++i) {
                const auto type=entries[i]>>12;if(type==IMAGE_REL_BASED_ABSOLUTE)continue;
                assert(type==IMAGE_REL_BASED_HIGHLOW);
                At<uint32_t>(base,block->VirtualAddress+(entries[i]&4095))+=static_cast<uint32_t>(base-pe->OptionalHeader.ImageBase);
            }
            offset+=block->SizeOfBlock;
        }
        // No fixture code is executable. Only authored writable sections can
        // receive the test's game-data instance pointer writes.
        DWORD previous{};assert(VirtualProtect(reinterpret_cast<void*>(base),size,PAGE_READONLY,&previous));
        for(unsigned i=0;i<pe->FileHeader.NumberOfSections;++i)if(sections[i].Characteristics&IMAGE_SCN_MEM_WRITE)
            assert(VirtualProtect(reinterpret_cast<void*>(base+sections[i].VirtualAddress),sections[i].Misc.VirtualSize,PAGE_READWRITE,&previous));
        physical=reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr,0x4000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));assert(physical);
        car=reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr,0x1000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));assert(car);
        At<uintptr_t>(car,0x30)=physical;At<float>(physical,0x12f0)=expectedClutch;
        for(unsigned i=0;i<24;++i) {
            const auto job=base+0xe72b40+i*0x1c;
            At<uintptr_t>(job)=base+0xd43158;At<uintptr_t>(job,8)=physical;At<float>(job,0xc)=.01f;At<uint32_t>(job,0x18)=0;
        }
        At<uint32_t>(base,0xe73e60)=24;At<uint32_t>(base,0xe72ad8)=1;At<uintptr_t>(base,0xe72a68)=physical;
    }
    // Native/clone instructions are never executed, and process-lifetime owned
    // executable copies intentionally remain allocated after module Stop.
    ~Fixture(){VirtualFree(reinterpret_cast<void*>(car),0,MEM_RELEASE);VirtualFree(reinterpret_cast<void*>(physical),0,MEM_RELEASE);VirtualFree(reinterpret_cast<void*>(base),0,MEM_RELEASE);}
};
}
int main(int argc,char** argv) {
    assert(argc==2 || argc==3);const auto percent=argc==3?static_cast<unsigned>(std::stoul(argv[2])):50u;
    assert(percent>=10 && percent<=100);Fixture f(argv[1]);const auto job=f.base+0xe72b40;
    shift::Configure(f.base,100,false);shift::Tick();assert(!shift::Snapshot().enabled && !shift::Snapshot().ready);
    // Whole-body hashing must reject changes away from the tiny patched gate.
    const auto corrupt=f.base+shift::detail::bodies[0].rva+0x10;DWORD protection{};
    assert(VirtualProtect(reinterpret_cast<void*>(corrupt),1,PAGE_READWRITE,&protection));
    At<uint8_t>(corrupt)^=1;assert(!shift::testing::Prepare(f.base,percent));At<uint8_t>(corrupt)^=1;
    DWORD ignored{};assert(VirtualProtect(reinterpret_cast<void*>(corrupt),1,protection,&ignored));
    assert(shift::testing::Prepare(f.base,percent));
    unsigned relativeCalls{},absoluteOperands{},checkedBytes{};
    for(unsigned i=0;i<std::size(shift::detail::bodies);++i) {
        const auto& body=shift::detail::bodies[i];const auto copy=shift::testing::Clone(i);
        MEMORY_BASIC_INFORMATION info{};assert(VirtualQuery(reinterpret_cast<void*>(copy),&info,sizeof(info)) && info.Protect==PAGE_EXECUTE_READ && info.Type==MEM_PRIVATE);
        std::vector<uint8_t> expected(body.size);std::memcpy(expected.data(),reinterpret_cast<void*>(f.base+body.rva),body.size);
        for(const auto call:body.calls) {
            uintptr_t target=f.base+call.target;
            for(unsigned n=0;n<std::size(shift::detail::bodies);++n)if(call.target==shift::detail::bodies[n].rva)target=shift::testing::Clone(n);
            const auto displacement=static_cast<uint32_t>(target-(copy+call.offset+5));std::memcpy(expected.data()+call.offset+1,&displacement,4);++relativeCalls;
        }
        for(const auto offset:body.relocations) {
            if(i==4 && offset==0xde)continue;
            assert(At<uint32_t>(copy,offset)==At<uint32_t>(f.base+body.rva,offset));++absoluteOperands;
        }
        if(i==4) {
            const auto operand=static_cast<uint32_t>(shift::testing::Threshold());std::memcpy(expected.data()+0xde,&operand,4);
            expected[0xe5]=1;expected[0xe6]=0x74;++absoluteOperands;
            constexpr uint8_t retry[]{0xc6,0x81,0x5c,0x1e,0,0,0,0xe9,0x2c,0xff,0xff,0xff,0x90};
            std::memcpy(expected.data()+0xf6,retry,sizeof(retry));
        }
        assert(!std::memcmp(expected.data(),reinterpret_cast<void*>(copy),body.size));checkedBytes+=body.size;
    }
    assert(relativeCalls==45 && absoluteOperands==24 && checkedBytes==2323);
    assert(At<float>(shift::testing::Threshold())==static_cast<float>(percent)/100.f);
    InputRoute route{0x123400,0x123800,5,true,f.car,f.physical};shift::Permit(route,true,1000);
    std::vector<uint8_t> before(f.size);std::memcpy(before.data(),reinterpret_cast<void*>(f.base),f.size);
    // Exercise the production attachment loop: stop at a foreign object, then
    // finish the same pool without rewriting an already attached instance.
    At<uintptr_t>(job+0x1c)=0x12345678;shift::Tick();assert(shift::Snapshot().attached==1);
    assert(At<uintptr_t>(job+0x1c)==0x12345678 && At<uintptr_t>(job+0x38)==f.base+0xd43158);
    At<uintptr_t>(job+0x1c)=f.base+0xd43158;shift::Tick();assert(shift::Snapshot().attached==24);
    assert(shift::Snapshot().gameDataInstanceWrites==24);
    auto expectedImage=before;
    for(unsigned i=0;i<24;++i){const auto pointer=shift::testing::Table();std::memcpy(expectedImage.data()+0xe72b40+i*0x1c,&pointer,4);}
    assert(!std::memcmp(expectedImage.data(),reinterpret_cast<void*>(f.base),f.size));
    const auto dispatch=[&](const InputRoute& current,uint64_t now,bool expected){
        const auto n=nativeCalls,c=copyCalls,e=events;const float clutch=At<float>(f.physical,0x12f0);
        shift::testing::Dispatch(reinterpret_cast<void*>(job),current,now,reinterpret_cast<void*>(&Native),reinterpret_cast<void*>(&Copy));
        assert(events==e+1 && nativeCalls==n+!expected && copyCalls==c+expected && At<float>(f.physical,0x12f0)==clutch);
    };
    dispatch(route,1000,true);dispatch(route,1250,true);dispatch(route,1251,false);dispatch(route,999,false);
    {
        const auto n=nativeCalls,e=events;
        shift::testing::Dispatch(reinterpret_cast<void*>(job),route,1000,reinterpret_cast<void*>(&Native),reinterpret_cast<void*>(&Copy),false);
        assert(nativeCalls==n+1 && events==e+1);
    }
    auto other=route;other.physics+=4;dispatch(other,1000,false);other=route;other.car+=4;dispatch(other,1000,false);
    other=route;other.controller+=4;dispatch(other,1000,false);other=route;other.valid=false;dispatch(other,1000,false);
    for(unsigned i=0;i<5;++i){At<uint8_t>(f.physical,0x1e44+i)=1;dispatch(route,1000,false);At<uint8_t>(f.physical,0x1e44+i)=0;}
    for(unsigned kind:{1u,2u,3u}){At<uint32_t>(job,0x18)=kind;dispatch(route,1000,false);}At<uint32_t>(job,0x18)=0;
    for(float dt:{0.f,-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        At<float>(job,0xc)=dt;dispatch(route,1000,false);
    }At<float>(job,0xc)=.01f;
    for(float clutch:{-.01f,1.01f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        At<float>(f.physical,0x12f0)=clutch;assert(!shift::testing::Eligible(reinterpret_cast<void*>(job),route,1000));
    }At<float>(f.physical,0x12f0)=expectedClutch;
    At<uint32_t>(f.base,0xe72ad8)=2;At<uintptr_t>(f.base,0xe72a6c)=f.physical;dispatch(route,1000,false);
    At<uint32_t>(f.base,0xe72ad8)=1;At<uintptr_t>(f.base,0xe72a6c)=0;
    At<uintptr_t>(f.car,0x30)=0;dispatch(route,1000,false);At<uintptr_t>(f.car,0x30)=f.physical;
    shift::Permit(route,false,1000);dispatch(route,1000,false);shift::Permit(route,true,1000);
    dispatch(route,1000,true);assert(At<float>(f.physical,0x12f0)==.65f);
    // Restore only owned pointers; retain a later foreign replacement.
    At<uintptr_t>(job+0x1c)=0x12345678;shift::Stop();assert(At<uintptr_t>(job+0x1c)==0x12345678);
    assert(shift::Snapshot().attached==0 && shift::Snapshot().restores==23 && shift::Snapshot().gameDataInstanceWrites==47);
    dispatch(route,1000,false);shift::Permit(route,true,1000);dispatch(route,1000,false);shift::Stop();
    assert(shift::Snapshot().restores==23);At<uintptr_t>(job+0x1c)=f.base+0xd43158;
    // Registry test scratch was initially zero and is now restored too.
    assert(!std::memcmp(before.data(),reinterpret_cast<void*>(f.base),f.size));
    MEMORY_BASIC_INFORMATION retained{};assert(VirtualQuery(reinterpret_cast<void*>(shift::testing::Clone(0)),&retained,sizeof(retained)) && retained.Protect==PAGE_EXECUTE_READ);
    std::cout<<"{\"threshold\":"<<percent<<",\"copiedBytes\":"<<checkedBytes<<",\"relativeCalls\":"<<relativeCalls<<",\"absoluteOperands\":"<<absoluteOperands
        <<",\"ownedNativeCallbacks\":"<<nativeCalls<<",\"ownedCloneCallbacks\":"<<copyCalls<<",\"ownedCompletionEvents\":"<<events
        <<",\"nativeGameCalls\":0,\"gameCodeWrites\":0,\"sharedTableWrites\":0,\"jobInstanceWrites\":47,\"realClutchUnchanged\":true,\"passed\":true}\n";
}
