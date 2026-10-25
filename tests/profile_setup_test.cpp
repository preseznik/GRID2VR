#include "profile_setup.h"
#include <windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace grid2vr::profilesetup;
namespace {
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void Put(std::vector<std::uint8_t>& image,std::size_t at,std::uint32_t value){std::memcpy(image.data()+at,&value,4);}
std::uintptr_t seenState{};std::uint32_t* seenOutput{};unsigned originalCalls{};
std::uint32_t outputValue{},alternateResult{};bool alternate{};
std::uint32_t* __fastcall FakeStep(void* state,void*,std::uint32_t* output){
    Check(GetLastError()==0x1234,"original receives caller last-error");
    ++originalCalls;seenState=reinterpret_cast<std::uintptr_t>(state);seenOutput=output;*output=outputValue;SetLastError(0x5678);
    return alternate?&alternateResult:output;
}
}
int main(){
    try{
        std::vector<std::uint8_t> image(0x100e000);
        const auto base=reinterpret_cast<std::uintptr_t>(image.data());
        Check(base<=UINT32_MAX-image.size(),"32-bit fixture address");
        image[0]='M';image[1]='Z';Put(image,0x3c,0x158);Put(image,0x158,IMAGE_NT_SIGNATURE);
        Put(image,0x160,0x524b8944);Put(image,0x1a8,0x100e000);
        Put(image,0xcd64d4,static_cast<std::uint32_t>(base+0x4343c0));Put(image,0xcd6328,static_cast<std::uint32_t>(base+0x730150));
        Put(image,0xcd0b74,static_cast<std::uint32_t>(base+0x3fdce0));
        const std::uint8_t changed[]{0x55,0x8b,0xec,0x51,0x56,0x8b,0xf1,0x8b,0x46,0x24,0x57,0xa8,0x20,0x75,0x06,0x83,0xc8,0x20,0x89,0x46,0x24,0x6a,0,0x8d,0x4d,0xfc,0xe8,0xd1,0xc8,0xde,0xff};
        const std::uint8_t wait[]{0x55,0x8b,0xec,0x51,0x53,0x56,0x8b,0xf1,0x8b,0x46,0x24,0x57,0xa8,0x20,0x75,0x06,0x83,0xc8,0x20,0x89,0x46,0x24,0x6a,0,0x8d,0x4d,0xfc,0xe8,0x40,0x0b,0xaf,0xff};
        const std::uint8_t begin[]{0x55,0x8b,0xec,0x83,0xec,0x2c,0x56,0x8b,0xf1,0x8b,0x46,0x24,0x57,0xa8,0x20,0x75,0x06,0x83,0xc8,0x20,0x89,0x46,0x24,0x6a,0,0x8d,0x4d,0xf4,0xe8,0xaf,0x2f,0xe2,0xff};
        const std::uint8_t profileGet[]{0x33,0xc0,0x83,0x79,0x64,0xff,0x7e,0x03,0x8b,0x41,0x68,0xc3};
        std::memcpy(image.data()+0x4343c0,changed,sizeof(changed));std::memcpy(image.data()+0x730150,wait,sizeof(wait));
        std::memcpy(image.data()+0x3fdce0,begin,sizeof(begin));std::memcpy(image.data()+0x78e3d0,profileGet,sizeof(profileGet));
        Check(ValidLayout(image.data(),image.size(),base),"guarded AVX layout");
        image[0x730150]^=1;Check(!ValidLayout(image.data(),image.size(),base),"changed step instruction refused");image[0x730150]^=1;
        image[0xcd64d4]^=1;Check(!ValidLayout(image.data(),image.size(),base),"foreign vtable route refused");image[0xcd64d4]^=1;
        image[0x3fdce0]^=1;Check(!ValidLayout(image.data(),image.size(),base),"changed save-begin instruction refused");image[0x3fdce0]^=1;
        image[0x78e3d0]^=1;Check(!ValidLayout(image.data(),image.size(),base),"changed profile getter refused");image[0x78e3d0]^=1;
        Check(!ValidLayout(image.data(),image.size()-1,base),"truncated layout refused");
        // Synthetic state graph exercises safe reads only, never game code.
        auto pointer=[&](std::size_t at,std::size_t target){Put(image,at,static_cast<std::uint32_t>(base+target));};
        constexpr std::size_t state=0x1000,helper=0x2000,manager=0x3000,table=0x4000,entry=0x5000;
        pointer(state+0x2c,helper);pointer(helper,manager);pointer(manager+0xc8,table);pointer(table,entry);image[entry+8]=1;
        auto changedState=Inspect(Kind::ProfileChanged,base,base+state);
        Check(changedState.readable&&changedState.signedOut,"signed-out predicate observed");
        image[entry+8]=0;Check(!Inspect(Kind::ProfileChanged,base,base+state).signedOut,"signed-in predicate observed");
        Put(image,manager+0xd8,1);changedState=Inspect(Kind::ProfileChanged,base,base+state);
        Check(changedState.readable&&!changedState.signedOut,"native out-of-range profile is false");Put(image,manager+0xd8,0);
        Check(!Inspect(Kind::ProfileChanged,base,1).readable,"invalid state safely unknown");
        constexpr std::size_t context=0x6000,managers=0x7000,map=0x8000,entries=0x9000,payload=0xa000,operation=0xb000,vtable=0xc000,getter=0xd000,type=0xe000;
        pointer(state+0x28,context);pointer(context+4,managers);Put(image,0xed14e8,2);pointer(managers+8,map);
        pointer(state+0x2c,type);std::memcpy(image.data()+type,"profile",8);pointer(map+8,entries);pointer(map+12,entries+40);
        pointer(entries,0xdefe24);std::memset(image.data()+entries+4,0xcc,32);std::memcpy(image.data()+entries+4,"profile",8);pointer(entries+36,payload);
        pointer(payload+4,operation);pointer(operation+0x20,vtable);pointer(vtable+8,getter);
        const std::uint8_t get[]{0x8b,0x41,0x18,0xc3};std::memcpy(image.data()+getter,get,4);Put(image,operation+0x38,0x3c);Put(image,operation+0x3c,7);
        auto waiting=Inspect(Kind::WaitForSave,base,base+state);
        Check(waiting.readable&&waiting.mapMatch==1&&waiting.operationKnown&&waiting.resultKnown&&waiting.operationResult==7,"exact map entry and result decoded despite string padding");
        Check(waiting.statusKnown&&waiting.status==0x3c&&waiting.statusMethodRva==getter,"simple status getter read without execution");
        image[getter]=0xe8;waiting=Inspect(Kind::WaitForSave,base,base+state);
        Check(!waiting.statusKnown&&waiting.statusMethodRva==getter,"nontrivial method remains unknown");image[getter]=0x8b;
        Put(image,payload+4,0);waiting=Inspect(Kind::WaitForSave,base,base+state);
        Check(waiting.operationKnown&&!waiting.operation&&!waiting.resultKnown,"missing operation distinguished from unreadable map");pointer(payload+4,operation);
        image[entries]^=1;waiting=Inspect(Kind::WaitForSave,base,base+state);
        Check(waiting.readable&&!waiting.mapMatch&&!waiting.operationKnown,"derived or unrelated key stays unknown");image[entries]^=1;
        pointer(map+12,entries+65537*40);waiting=Inspect(Kind::WaitForSave,base,base+state);
        Check(!waiting.readable&&waiting.resolverStage==9&&waiting.mapEntries==65537,"map bound refusal explains its stage and size");pointer(map+12,entries+40);
        constexpr std::size_t largerEntries=0x20000;
        for(std::size_t i=0;i<599;++i)std::memcpy(image.data()+largerEntries+i*40+4,"other",6);
        std::memcpy(image.data()+largerEntries+599*40,image.data()+entries,40);
        pointer(map+8,largerEntries);pointer(map+12,largerEntries+600*40);
        waiting=Inspect(Kind::WaitForSave,base,base+state);
        Check(waiting.readable&&waiting.mapEntries==600&&waiting.operationKnown&&waiting.operation==base+operation,"bounded binary search handles larger sorted registry");
        pointer(map+8,entries);pointer(map+12,entries+40);
        Put(image,0xed14e8,4097);waiting=Inspect(Kind::WaitForSave,base,base+state);
        Check(waiting.resolverStage==3&&waiting.managerIndex==4097,"manager index bound captured");Put(image,0xed14e8,2);
        Put(image,state+0x2c,0);image[entries+4]=0;waiting=Inspect(Kind::WaitForSave,base,base+state);
        Check(waiting.readable&&waiting.mapMatch==1,"native null type maps to empty key");pointer(state+0x2c,type);image[entries+4]='p';
        constexpr std::size_t profileManager=0x10000,profileSystem=0x12000,service=0x13000;
        pointer(state+0x34,type);image[state+0x40]=1;image[state+0x38]=1;Put(image,state+0x3c,8);
        Put(image,0xde23e4,3);pointer(managers+12,profileManager);pointer(profileManager+0x1370,profileSystem);Put(image,profileSystem+0x64,UINT32_MAX);
        Put(image,0xed1520,4);pointer(managers+16,service);image[service+0x294]=1;
        auto beginning=Inspect(Kind::BeginSave,base,base+state);
        Check(beginning.beginKnown&&beginning.beginReady&&beginning.beginForced&&beginning.beginOptions==8,"save begin state flags observed");
        Check(beginning.profileKnown&&!beginning.profile&&beginning.profileIndex==UINT32_MAX,"native missing-profile descriptor distinguished");
        Check(beginning.serviceKnown&&beginning.serviceReady&&beginning.readable&&beginning.operationKnown,"save begin service and operation observed");
        Put(image,profileSystem+0x64,0);pointer(profileSystem+0x68,0x14000);image[service+0x294]=0;
        beginning=Inspect(Kind::BeginSave,base,base+state);
        Check(beginning.profileKnown&&beginning.profile==base+0x14000&&beginning.serviceKnown&&!beginning.serviceReady,"present descriptor and inactive service distinguished");
        std::uint32_t output{};Observation observation{};outputValue=static_cast<std::uint32_t>(base+0xc82bcc);
        auto step=reinterpret_cast<Step>(FakeStep);
        SetLastError(0x1234);
        auto returned=Observe(Kind::WaitForSave,base,image.data()+state,&output,step,observation);
        Check(GetLastError()==0x5678,"original last-error preserved");
        Check(originalCalls==1&&seenState==base+state&&seenOutput==&output&&returned==&output&&output==outputValue,"original called exactly once with unchanged ABI and result");
        Check(observation.outputCode==4&&observation.before.operationResult==7&&observation.after.operationResult==7,"cancelled state result and operation captured");
        alternate=true;SetLastError(0x1234);returned=Observe(Kind::WaitForSave,base,image.data()+state,&output,step,observation);
        Check(returned==&alternateResult&&!observation.returnedOutput&&originalCalls==2,"nonstandard original return also preserved");
        outputValue=static_cast<std::uint32_t>(base+0xc05944);SetLastError(0x1234);Observe(Kind::WaitForSave,base,image.data()+state,&output,step,observation);
        Check(observation.outputCode==3,"signed-out result classified by exact native address");
        std::cout<<"profile setup passive audit fixtures passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
