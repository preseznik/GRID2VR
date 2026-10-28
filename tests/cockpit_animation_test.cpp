#include "cockpit_animation.h"
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

using namespace grid2vr::cockpitanimation;
#define CHECK(value) do {if(!(value)){printf("Failed line %d: %s\n",__LINE__,#value);return 1;}} while(false)
unsigned animationUpdates{},timeUpdates{};
void __fastcall Animation(void*,void*,float,unsigned jobs) {if(jobs==6)++animationUpdates;}
void __fastcall Time(void*,void*,float) {++timeUpdates;}
void Jump(uint8_t* at,void* function) {
    at[0]=0xe9;const auto displacement=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(function)-reinterpret_cast<uintptr_t>(at+5));
    memcpy(at+1,&displacement,4);
}
int wmain(int argc,wchar_t** argv) {
    CHECK(argc==2);
    const auto requests=std::filesystem::temp_directory_path()/
        (L"GRID2VR-animation-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    CHECK(!std::filesystem::exists(requests));
    if(std::wstring(argv[1])==L"--inactive") {
        CHECK(Install(0,requests));CHECK(Install(0,requests));Tick(true);Stop();
        puts("Non-VR attachment remains a successful no-op.");return 0;
    }
    std::ifstream input(argv[1],std::ios::binary);CHECK(input);
    struct Fixture {uint32_t rva;std::vector<uint8_t> bytes;std::vector<uint32_t> relocations;};
    std::vector<Fixture> fixtures;
    while(input.peek()!=std::char_traits<char>::eof()) {
        std::array<uint32_t,3> row{};CHECK(input.read(reinterpret_cast<char*>(row.data()),sizeof(row)));
        CHECK(row[1]<=256 && row[2]<=16);
        Fixture item{row[0],std::vector<uint8_t>(row[1]),std::vector<uint32_t>(row[2])};
        CHECK(input.read(reinterpret_cast<char*>(item.bytes.data()),item.bytes.size()));
        CHECK(input.read(reinterpret_cast<char*>(item.relocations.data()),item.relocations.size()*4));
        fixtures.push_back(std::move(item));
    }
    CHECK(fixtures.size()==10);
    auto* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,0xe40000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));CHECK(image);
    const auto base=reinterpret_cast<uintptr_t>(image);
    for(const auto& item:fixtures) {
        CHECK(item.rva+item.bytes.size()<0xe40000);
        memcpy(image+item.rva,item.bytes.data(),item.bytes.size());
        for(auto at:item.relocations) {
            CHECK(at+4<=item.bytes.size());uint32_t address{};memcpy(&address,image+item.rva+at,4);
            address+=static_cast<uint32_t>(base-0x400000);memcpy(image+item.rva+at,&address,4);
        }
    }
    CHECK(InstructionsMatch(base));CHECK(!InstructionsMatch(0));
    for(const auto& item:fixtures) {
        // Reject modified logic, opcodes, offsets and relocated operands.
        for(size_t at=0;at<item.bytes.size();++at) {
            image[item.rva+at]^=1;CHECK(!InstructionsMatch(base));image[item.rva+at]^=1;
        }
    }
    auto& quality=*reinterpret_cast<int*>(image+qualityRva);
    QualityOverride state;CHECK(!state.Refresh(true));CHECK(state.Bind(base));CHECK(!state.Bind(base));
    quality=0;CHECK(state.Refresh(false));CHECK(quality==0 && !state.Active());
    CHECK(state.Refresh(true));CHECK(quality==1 && state.Requested()==0 && state.Changes()==1);
    for(unsigned i=0;i<100;++i)CHECK(state.Refresh(true));
    CHECK(state.Active() && state.Requested()==0 && state.Changes()==1);
    state.Release();CHECK(quality==1 && !state.Active());
    for(unsigned i=0;i<3;++i) {
        quality=0;CHECK(state.Refresh(true));CHECK(quality==1);
        quality=2;CHECK(state.Refresh(true));CHECK(state.Requested()==2 && !state.Active());state.Release();CHECK(quality==2);
    }
    quality=0;CHECK(state.Refresh(true));quality=1;CHECK(state.Refresh(true));
    state.Release();CHECK(quality==1); // A later Medium choice is indistinguishable; never undo it.
    quality=0;CHECK(state.Refresh(false));CHECK(quality==0);
    for(int unknown:{-1,3,4,100}) {quality=unknown;CHECK(!state.Refresh(true));CHECK(quality==unknown && !state.Active());}
    quality=0;CHECK(state.Refresh(true));quality=2;state.Release();CHECK(quality==2);
    DWORD protection{};CHECK(VirtualProtect(image+qualityRva,4,PAGE_READONLY,&protection));
    QualityOverride readOnly;CHECK(!readOnly.Bind(base));
    CHECK(VirtualProtect(image+qualityRva,4,protection,&protection));
    quality=0;CHECK(VirtualProtect(image+qualityRva,4,PAGE_READONLY,&protection));
    CHECK(!state.Refresh(true));CHECK(quality==0);
    CHECK(VirtualProtect(image+qualityRva,4,protection,&protection));

    // Exercise the production entry points without initializing/linking MinHook.
    // Qualification and repeated device attachment must leave all code and the
    // startup quality untouched. Only a ready world frame may change the data.
    CHECK(std::filesystem::create_directory(requests));
    for(const auto name:{L"runtime-session.request",L"xr-session.request",L"xr-projection.request"})
        CHECK(std::ofstream(requests/name).put('\n'));
    CHECK(Install(base,requests));CHECK(Install(base,requests));CHECK(quality==0);
    Tick(false);CHECK(quality==0);Tick(true);CHECK(quality==1);Tick(true);CHECK(quality==1);
    CHECK(InstructionsMatch(base));
    quality=2;Tick(true);CHECK(quality==2);
    quality=0;Tick(true);CHECK(quality==1);quality=1;Tick(true);Stop();CHECK(quality==1);
    quality=0;Tick(true);CHECK(quality==0);CHECK(InstructionsMatch(base));
    std::filesystem::remove_all(requests);

    // Run the captured native quality-to-mode instructions in a small function
    // wrapper. Low selects mode4, which skips the actual character update gate.
    // Restoring characterQuality1 recovers that gate with geometry still Low.
    auto* modeCode=image+0x1000;
    constexpr uint8_t prefix[]{0x55,0x8b,0xec,0x83,0xec,0x08};
    constexpr uint8_t suffix[]{0x8b,0x45,0xf8,0xc9,0xc3};
    memcpy(modeCode,prefix,sizeof(prefix));memcpy(modeCode+sizeof(prefix),image+0x3f6c6f,37);
    memcpy(modeCode+sizeof(prefix)+37,suffix,sizeof(suffix));
    Jump(image+0x3c1cb0,reinterpret_cast<void*>(Animation));Jump(image+0x3f7e50,reinterpret_cast<void*>(Time));
    image[0xd93460]=1;
    CHECK(FlushInstructionCache(GetCurrentProcess(),image,0xe40000));
    using ModeFn=int(__cdecl*)();using UpdateFn=void(__thiscall*)(void*,float);
    const auto mode=reinterpret_cast<ModeFn>(modeCode);
    const auto update=reinterpret_cast<UpdateFn>(image+0x1564b0);
    std::vector<uint8_t> manager(0x8330);std::array<uint32_t,0xa0/4> owner{};
    owner[0x98/4]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(manager.data()));owner[0x8c/4]=4;
    auto& nativeMode=*reinterpret_cast<int*>(manager.data()+0x8324);
    quality=0;nativeMode=mode();CHECK(nativeMode==4);update(owner.data(),0.016f);CHECK(animationUpdates==0 && timeUpdates==0);
    CHECK(state.Refresh(true));nativeMode=mode();CHECK(nativeMode==1);
    update(owner.data(),0.016f);CHECK(animationUpdates==1 && timeUpdates==1);
    quality=2;CHECK(state.Refresh(true));nativeMode=mode();CHECK(nativeMode==2);
    update(owner.data(),0.016f);CHECK(animationUpdates==2 && timeUpdates==2);
    // These are still native character/camera eligibility checks, not bypassed.
    owner[0x8c/4]=3;update(owner.data(),0.016f);CHECK(animationUpdates==2);
    owner[0x8c/4]=4;image[0xd93460]=0;update(owner.data(),0.016f);CHECK(animationUpdates==2);
    CHECK(VirtualFree(image,0,MEM_RELEASE));
    puts("Captured Low animation bypass reproduced; no startup/code mutation, frame minimum, native recovery and Medium preservation pass.");
    return 0;
}
