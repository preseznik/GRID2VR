#include "profile_setup.h"
#include <windows.h>
#include <array>
#include <cstring>
#include <cstdio>

namespace grid2vr::profilesetup {
namespace {
constexpr std::uint32_t imageBytes=0x100e000, changedStep=0x4343c0, waitStep=0x730150, beginStep=0x3fdce0;
constexpr std::uint32_t changedSlot=0xcd64d4, waitSlot=0xcd6328, beginSlot=0xcd0b74;
constexpr std::uint8_t changedCode[]{0x55,0x8b,0xec,0x51,0x56,0x8b,0xf1,0x8b,0x46,0x24,0x57,0xa8,0x20,0x75,0x06,0x83,0xc8,0x20,0x89,0x46,0x24,0x6a,0,0x8d,0x4d,0xfc,0xe8,0xd1,0xc8,0xde,0xff};
constexpr std::uint8_t waitCode[]{0x55,0x8b,0xec,0x51,0x53,0x56,0x8b,0xf1,0x8b,0x46,0x24,0x57,0xa8,0x20,0x75,0x06,0x83,0xc8,0x20,0x89,0x46,0x24,0x6a,0,0x8d,0x4d,0xfc,0xe8,0x40,0x0b,0xaf,0xff};
constexpr std::uint8_t beginCode[]{0x55,0x8b,0xec,0x83,0xec,0x2c,0x56,0x8b,0xf1,0x8b,0x46,0x24,0x57,0xa8,0x20,0x75,0x06,0x83,0xc8,0x20,0x89,0x46,0x24,0x6a,0,0x8d,0x4d,0xf4,0xe8,0xaf,0x2f,0xe2,0xff};
constexpr std::uint8_t profileGetter[]{0x33,0xc0,0x83,0x79,0x64,0xff,0x7e,0x03,0x8b,0x41,0x68,0xc3};
bool Read(std::uintptr_t at,void* output,std::size_t bytes) {
    if(at<0x10000 || bytes>0x100e000 || at>UINT32_MAX-bytes)return false;
    SIZE_T copied{};
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(at),output,bytes,&copied)&&copied==bytes;
}
bool U32(std::uintptr_t at,std::uint32_t& value){return Read(at,&value,sizeof(value));}
std::uint32_t Word(const std::uint8_t* at){std::uint32_t value{};std::memcpy(&value,at,4);return value;}
bool Text32(std::uintptr_t at,std::array<char,32>& text) {
    if(!at)return true; // Native A994F0 formats a null type as an empty key.
    for(std::size_t i=0;i<text.size();++i){if(!Read(at+i,&text[i],1))return false;if(!text[i])return true;}
    return false;
}
void InspectOperation(std::uintptr_t base,Snapshot& result) {
    if(!result.operation)return;
    result.resultKnown=U32(result.operation+0x3c,result.operationResult);
    std::uint32_t table{},method{};
    if(!U32(result.operation+0x20,table)||!U32(table+8,method)||method<base||method>=base+0xb15000)return;
    result.statusMethodRva=method-static_cast<std::uint32_t>(base);
    // Observe a simple, exact getter without calling it a second time. Other
    // status implementations remain explicitly unknown in the receipt.
    std::array<std::uint8_t,4> code{};
    if(Read(method,code.data(),code.size())&&code[0]==0x8b&&code[1]==0x41&&code[2]<=0x40&&code[3]==0xc3)
        result.statusKnown=U32(result.operation+0x20+code[2],result.status);
}
void InspectBegin(std::uintptr_t base,std::uintptr_t state,Snapshot& result) {
    std::uint8_t ready{},forced{};
    result.beginKnown=Read(state+0x40,&ready,1)&&Read(state+0x38,&forced,1)&&
        U32(state+0x3c,result.beginOptions)&&U32(state+0x44,result.beginExisting);
    result.beginReady=ready;result.beginForced=forced;
    std::uint32_t context{},array{},index{},manager{},profileSystem{};
    if(!U32(state+0x28,context)||!U32(context+4,array))return;
    if(U32(base+0xde23e4,index)&&index<=4096&&U32(array+index*4,manager)&&
       U32(manager+0x1370,profileSystem)&&U32(profileSystem+0x64,result.profileIndex)){
        if(static_cast<std::int32_t>(result.profileIndex)<=-1)result.profileKnown=1;
        else result.profileKnown=U32(profileSystem+0x68,result.profile);
    }
    std::uint8_t service{};
    if(U32(base+0xed1520,index)&&index<=4096&&U32(array+index*4,manager)&&Read(manager+0x294,&service,1)){
        result.serviceKnown=1;result.serviceReady=service;}
}
}
bool ValidLayout(const std::uint8_t* image,std::size_t bytes,std::uintptr_t base) {
    if(!image||bytes<imageBytes||base>UINT32_MAX-imageBytes)return false;
    return image[0]=='M'&&image[1]=='Z'&&Word(image+0x3c)==0x158&&Word(image+0x158)==IMAGE_NT_SIGNATURE&&
        Word(image+0x160)==0x524b8944&&Word(image+0x1a8)==imageBytes&&
        Word(image+changedSlot)==base+changedStep&&Word(image+waitSlot)==base+waitStep&&Word(image+beginSlot)==base+beginStep&&
        !std::memcmp(image+changedStep,changedCode,sizeof(changedCode))&&
        !std::memcmp(image+waitStep,waitCode,sizeof(waitCode))&&
        !std::memcmp(image+beginStep,beginCode,sizeof(beginCode))&&
        !std::memcmp(image+0x78e3d0,profileGetter,sizeof(profileGetter));
}
Snapshot Inspect(Kind kind,std::uintptr_t base,std::uintptr_t state) {
    Snapshot result{};
    if(kind==Kind::ProfileChanged) {
        std::uint32_t helper{},manager{},index{},table{},entry{};std::uint8_t signedOut{};
        if(!U32(state+0x2c,helper)||!U32(helper,manager)||!U32(manager+0xd8,index))return result;
        if(index>=1){result.readable=1;return result;}
        if(U32(manager+0xc8,table)&&U32(table+index*4,entry)&&Read(entry+8,&signedOut,1)){
            result.readable=1;result.signedOut=signedOut!=0;}
        return result;
    }
    if(kind==Kind::BeginSave)InspectBegin(base,state,result);
    std::array<char,32> wanted{};
    if(!U32(state+0x28,result.context))return result;result.resolverStage=1;
    if(!U32(result.context+4,result.managers))return result;result.resolverStage=2;
    if(!U32(base+0xed14e8,result.managerIndex))return result;result.resolverStage=3;
    if(result.managerIndex>4096)return result;result.resolverStage=4;
    if(!U32(result.managers+result.managerIndex*4,result.manager))return result;result.resolverStage=5;
    if(!U32(state+(kind==Kind::BeginSave?0x34:0x2c),result.type))return result;result.resolverStage=6;
    if(!Text32(result.type,wanted))return result;result.resolverStage=7;
    if(!U32(result.manager+8,result.mapBegin))return result;result.resolverStage=8;
    if(!U32(result.manager+12,result.mapEnd))return result;result.resolverStage=9;
    const auto begin=result.mapBegin,end=result.mapEnd;
    if(end<begin||(end-begin)%40)return result;
    result.mapEntries=(end-begin)/40;if(result.mapEntries>65536)return result;result.resolverStage=10;
    // D9FA30 orders the native 40-byte map entries by type string first.
    // Binary search avoids copying a large registry or guessing its usual size.
    std::uint32_t low{},high=result.mapEntries;
    std::array<std::uint8_t,40> entry{};
    const auto readEntry=[&](std::uint32_t at){return Read(begin+at*40,entry.data(),entry.size())&&std::memchr(entry.data()+4,0,32);};
    while(low<high){const auto middle=low+(high-low)/2;
        if(!readEntry(middle))return result;
        if(std::strcmp(reinterpret_cast<const char*>(entry.data()+4),wanted.data())<0)low=middle+1;else high=middle;
    }
    result.readable=1;result.resolverStage=11;
    std::uint32_t inspected{};
    for(auto i=low;i<result.mapEntries;++i){
        if(!readEntry(i)){result.readable=0;return result;}
        if(std::strcmp(reinterpret_cast<const char*>(entry.data()+4),wanted.data()))break;
        if(++inspected>64){result.readable=0;result.operationKnown=0;result.operation=0;return result;}
        // The native map also supports derived key types. Only the exact save
        // operation key is decoded here; unmatched keys remain unknown.
        if(Word(entry.data())!=base+0xdefe24)continue;
        ++result.mapMatch;std::uint32_t operation{};
        if(U32(Word(entry.data()+36)+4,operation)){result.operationKnown=1;result.operation=operation;}
    }
    if(result.mapMatch!=1){result.operationKnown=0;result.operation=0;return result;}
    result.resolverStage=12;InspectOperation(base,result);return result;
}
std::uint32_t* Observe(Kind kind,std::uintptr_t base,void* state,std::uint32_t* output,Step original,Observation& observation) {
    const auto previousError=GetLastError();
    observation={};observation.kind=kind;observation.state=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(state));
    observation.before=Inspect(kind,base,observation.state);
    SetLastError(previousError);
    auto returned=original(state,output);
    const auto originalError=GetLastError();
    observation.returnedOutput=returned==output;
    observation.outputKnown=U32(reinterpret_cast<std::uintptr_t>(output),observation.output);
    observation.nextKnown=U32(base+0xdaf5fc,observation.next);
    if(observation.outputKnown){
        if(!observation.output)observation.outputCode=1;
        else if(observation.nextKnown&&observation.output==observation.next)observation.outputCode=2;
        else if(observation.output==base+0xc05944)observation.outputCode=3;
        else if(observation.output==base+0xc82bcc)observation.outputCode=4;
        else if(observation.output==base+0xc253fc)observation.outputCode=5;
    }
    observation.after=Inspect(kind,base,observation.state);SetLastError(originalError);return returned;
}
namespace {
constexpr std::size_t maxEvents=32;
constexpr LONG maxCalls=4096;
SRWLOCK auditLock=SRWLOCK_INIT;
std::uintptr_t hostBase{};
Step originalChanged{},originalWait{},originalBegin{};
volatile LONG calls{};
std::array<Observation,maxEvents> events{};
std::array<LONG,maxEvents> eventCalls{};
std::size_t eventCount{};
wchar_t receiptPath[1200]{};
char id[33]{};
void WriteSnapshot(FILE* file,const Snapshot& s) {
    fprintf(file,"{\"readable\":%u,\"signedOut\":%u,\"mapEntries\":%u,\"mapMatch\":%u,\"operationKnown\":%u,\"operation\":%u,\"resultKnown\":%u,\"operationResult\":%u,\"statusKnown\":%u,\"status\":%u,\"statusMethodRva\":%u,\"resolverStage\":%u,\"context\":%u,\"managers\":%u,\"managerIndex\":%u,\"manager\":%u,\"type\":%u,\"mapBegin\":%u,\"mapEnd\":%u,\"beginKnown\":%u,\"beginReady\":%u,\"beginForced\":%u,\"beginOptions\":%u,\"beginExisting\":%u,\"profileKnown\":%u,\"profileIndex\":%u,\"profile\":%u,\"serviceKnown\":%u,\"serviceReady\":%u}",
        s.readable,s.signedOut,s.mapEntries,s.mapMatch,s.operationKnown,s.operation,s.resultKnown,s.operationResult,s.statusKnown,s.status,s.statusMethodRva,
        s.resolverStage,s.context,s.managers,s.managerIndex,s.manager,s.type,s.mapBegin,s.mapEnd,s.beginKnown,s.beginReady,s.beginForced,s.beginOptions,s.beginExisting,s.profileKnown,s.profileIndex,s.profile,s.serviceKnown,s.serviceReady);
}
void Receipt(const char* phase) {
    FILE* file{};if(_wfopen_s(&file,receiptPath,L"wb")||!file)return;
    fprintf(file,"{\"schema\":1,\"phase\":\"%s\",\"profileId\":\"%s\",\"maxEvents\":32,\"maxObservedCalls\":4096,\"eventLimitReached\":%s,\"events\":[",phase,id,eventCount==maxEvents?"true":"false");
    for(std::size_t i=0;i<eventCount;++i){const auto& e=events[i];
        const char* resultNames[]{"unknown","pending","next","signed_out_of_profile","cancelled","list_select"};
        const auto kind=e.kind==Kind::ProfileChanged?"profile_changed":e.kind==Kind::WaitForSave?"wait_for_save":"begin_save";
        fprintf(file,"%s{\"call\":%ld,\"stateKind\":\"%s\",\"state\":%u,\"outputKnown\":%u,\"output\":%u,\"outputName\":\"%s\",\"returnedOutput\":%u,\"nextKnown\":%u,\"next\":%u,\"before\":",i?",":"",eventCalls[i],kind,e.state,e.outputKnown,e.output,resultNames[e.outputCode],e.returnedOutput,e.nextKnown,e.next);
        WriteSnapshot(file,e.before);fputs(",\"after\":",file);WriteSnapshot(file,e.after);fputc('}',file);
    }
    fputs("]}\n",file);fclose(file);
}
std::uint32_t* Invoke(Kind kind,void* state,std::uint32_t* output,Step original) {
    const auto call=InterlockedIncrement(&calls);
    if(call>maxCalls)return original(state,output);
    Observation observed{};auto returned=Observe(kind,hostBase,state,output,original,observed);
    const auto originalError=GetLastError();
    if(TryAcquireSRWLockExclusive(&auditLock)){
        if(eventCount<maxEvents&&(!eventCount||!(events[eventCount-1]==observed))){
            eventCalls[eventCount]=call;events[eventCount++]=observed;Receipt("observing");}
        ReleaseSRWLockExclusive(&auditLock);
    }
    SetLastError(originalError);return returned;
}
std::uint32_t* __fastcall Changed(void* state,void*,std::uint32_t* output){return Invoke(Kind::ProfileChanged,state,output,originalChanged);}
std::uint32_t* __fastcall Wait(void* state,void*,std::uint32_t* output){return Invoke(Kind::WaitForSave,state,output,originalWait);}
std::uint32_t* __fastcall Begin(void* state,void*,std::uint32_t* output){return Invoke(Kind::BeginSave,state,output,originalBegin);}
bool Replace(void** slot,void* expected,void* replacement){
    DWORD old{};if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old))return false;
    const bool changed=InterlockedCompareExchangePointer(slot,replacement,expected)==expected;
    DWORD ignored{};VirtualProtect(slot,sizeof(void*),old,&ignored);return changed;
}
}
bool Install(std::uint8_t* image,std::size_t bytes,const wchar_t* outputFolder,const char* profileId) {
    if(!image||!outputFolder||!profileId||std::strlen(profileId)!=32||receiptPath[0])return false;
    const auto base=reinterpret_cast<std::uintptr_t>(image);
    if(swprintf_s(receiptPath,L"%s\\profile-setup.json",outputFolder)<0||strcpy_s(id,profileId))return false;
    if(!ValidLayout(image,bytes,base)){Receipt("unsupported_layout");return false;}
    hostBase=base;originalChanged=reinterpret_cast<Step>(base+changedStep);originalWait=reinterpret_cast<Step>(base+waitStep);originalBegin=reinterpret_cast<Step>(base+beginStep);
    auto first=reinterpret_cast<void**>(image+changedSlot),second=reinterpret_cast<void**>(image+waitSlot),third=reinterpret_cast<void**>(image+beginSlot);
    if(!Replace(first,reinterpret_cast<void*>(originalChanged),reinterpret_cast<void*>(Changed))){Receipt("install_failed");return false;}
    if(!Replace(second,reinterpret_cast<void*>(originalWait),reinterpret_cast<void*>(Wait))){
        Replace(first,reinterpret_cast<void*>(Changed),reinterpret_cast<void*>(originalChanged));Receipt("install_failed");return false;}
    if(!Replace(third,reinterpret_cast<void*>(originalBegin),reinterpret_cast<void*>(Begin))){
        Replace(second,reinterpret_cast<void*>(Wait),reinterpret_cast<void*>(originalWait));
        Replace(first,reinterpret_cast<void*>(Changed),reinterpret_cast<void*>(originalChanged));Receipt("install_failed");return false;}
    Receipt("armed");return true;
}
}
