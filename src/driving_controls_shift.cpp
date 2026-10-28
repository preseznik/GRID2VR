#include "driving_controls_shift.h"
#include "driving_controls_shift_native.h"
#include "driving_controls_instance.h"
#include "guarded_hooks.h"
#include "game_executable.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace grid2vr::drivingcontrols::shift {
namespace {
using RunFn=void (__thiscall*)(void*);
constexpr uintptr_t nativeTable=0xd43158,jobPool=0xe72b40,eventCount=0xe73e60;
constexpr unsigned jobCount=24,jobSize=0x1c;
constexpr uintptr_t methods[]{0x8a76f0,0x141390,0x8bf910,0x8868c0,0xa5c020};
constexpr InstanceBusyRange lifetime[]{{0xb04930,0xb0496b},{0xb12950,0xb129f0},
    {0x8a76f0,0x8a772f},{0x88c170,0x88c240}};
std::mutex configurationMutex,permissionMutex;
uintptr_t base{},cache{},table{},thresholdAddress{};
std::array<uintptr_t,5> clones{};
unsigned threshold=100;
std::atomic<bool> enabled{},ready{},stopped{true};
std::atomic<unsigned> attached{};
std::array<std::atomic<bool>,jobCount> owned{};
std::atomic<const char*> reason{"not_requested"};
std::atomic<uint64_t> callbacks{},localJobs{},clonedJobs{},refused{},instanceWrites{},restores{};
struct Permission {InputRoute route;bool allowed{};uint64_t sampledAt{};};
Permission permission;
#ifdef GRID2VR_DRIVING_CONTROLS_SHIFT_TEST
bool fixtureImage{};
#endif

template<class T> bool Read(uintptr_t address,T& value) {
    return address>=0x10000 && ReadMemory(reinterpret_cast<void*>(address),&value,sizeof(value));
}
bool Writable(uintptr_t address,size_t size,DWORD type) {
    MEMORY_BASIC_INFORMATION info{};
    return address && size && address<=UINTPTR_MAX-size && VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info)) &&
        info.State==MEM_COMMIT && info.Type==type && (info.Protect==PAGE_READWRITE || info.Protect==PAGE_WRITECOPY) &&
        address+size<=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
}
bool JobAddress(uintptr_t object) {
    return object>=base+jobPool && object<base+jobPool+jobCount*jobSize && (object-base-jobPool)%jobSize==0;
}
bool MutableJob(uintptr_t object) {
#ifdef GRID2VR_DRIVING_CONTROLS_SHIFT_TEST
    if(fixtureImage)return JobAddress(object) && Writable(object,jobSize,MEM_PRIVATE);
#endif
    return JobAddress(object) && Writable(object,jobSize,MEM_IMAGE);
}
Permission PermissionSnapshot() {
    std::lock_guard lock(permissionMutex);return permission;
}
bool Foreground() {
    DWORD process{};GetWindowThreadProcessId(GetForegroundWindow(),&process);return process==GetCurrentProcessId();
}
bool MayRunLocalJob(uintptr_t object) {
    if(!enabled.load() || stopped.load() || !ready.load() || !JobAddress(object))return false;
    std::array<uint32_t,5> fields{};
    if(!Read(object+8,fields) || fields[4]!=0)return false;
    const auto permitted=PermissionSnapshot();
    return permitted.allowed && permitted.route.physics && fields[0]==permitted.route.physics;
}
bool EligibleImpl(uintptr_t object,const InputRoute& current,uint64_t now) {
    if(!enabled.load() || stopped.load() || !ready.load() || !JobAddress(object))return false;
    const auto permitted=PermissionSnapshot();
    if(!permitted.allowed || !permitted.sampledAt || now<permitted.sampledAt || now-permitted.sampledAt>250 ||
       !SamePhysicsRoute(permitted.route,current))return false;
    uintptr_t physical{};uint32_t kind{};float dt{},clutch{};std::array<uint8_t,5> modes{};
    if(!Read(object+8,physical) || physical!=current.physics || !Read(object+0x18,kind) || kind!=0 ||
       !Read(object+0xc,dt) || !std::isfinite(dt) || dt<=0 ||
       !Writable(physical,0x2c18,MEM_PRIVATE) || !Read(physical+0x12f0,clutch) || !std::isfinite(clutch) || clutch<0 || clutch>1 ||
       !Read(physical+0x1e44,modes))return false;
    for(const auto mode:modes)if(mode)return false;
    // The native scheduler binds one distinct physics object to each type-0
    // job. Verify both the exact slot and unique membership on every call.
    uint32_t count{};if(!Read(base+0xe72ad8,count) || !count || count>jobCount)return false;
    const auto index=static_cast<unsigned>((object-base-jobPool)/jobSize);
    if(index>=count)return false;
    unsigned matches{};
    for(unsigned i=0;i<count;++i) {
        uint32_t candidate{};if(!Read(base+0xe72a68+i*4,candidate))return false;
        if(candidate==physical){if(i!=index)return false;++matches;}
    }
    uintptr_t again{};
    return matches==1 && Read(current.car+0x30,again) && again==physical &&
        Read(object+8,again) && again==physical && enabled.load() && !stopped.load();
}
void DispatchImpl(void* self,const InputRoute& current,uint64_t now,RunFn native,RunFn copy,bool focused) {
    ++callbacks;
    const bool local=focused && MayRunLocalJob(reinterpret_cast<uintptr_t>(self)) && EligibleImpl(reinterpret_cast<uintptr_t>(self),current,now);
    if(local){++localJobs;++clonedJobs;}
    else ++refused;
    // No lock spans native work. The copied Run still signals its own original
    // completion event, exactly once, after the entire native physics update.
    (local?copy:native)(self);
}
void __fastcall Run(void* self,void*) {
    InputRoute current;
    const bool focused=MayRunLocalJob(reinterpret_cast<uintptr_t>(self)) && Foreground();
    if(focused)
        current=ReadInputRoute(base,[](uintptr_t at,void* data,size_t bytes){return ReadMemory(reinterpret_cast<void*>(at),data,bytes);});
    DispatchImpl(self,current,GetTickCount64(),reinterpret_cast<RunFn>(base+methods[2]),reinterpret_cast<RunFn>(clones[0]),focused);
}
bool PrepareCode() {
    std::array<std::array<uint8_t,0x500>,5> native{};
    for(size_t i=0;i<std::size(detail::bodies);++i) {
        const auto& body=detail::bodies[i];
        if(body.size>native[i].size() || !ReadMemory(reinterpret_cast<void*>(base+body.rva),native[i].data(),body.size))return false;
        auto normalized=native[i];
        for(const auto offset:body.relocations) {
            uint32_t value{};std::memcpy(&value,normalized.data()+offset,4);
            value-=static_cast<uint32_t>(base-0x400000);std::memcpy(normalized.data()+offset,&value,4);
        }
        if(gameexe::Digest(normalized.data(),body.size)!=body.sha256)return false;
    }
    std::array<uintptr_t,5> observed{};
    if(!Read(base+nativeTable,observed))return false;
    for(size_t i=0;i<observed.size();++i)if(observed[i]!=base+methods[i])return false;
    // One bounded owned page, writable only during construction. It remains
    // alive for the process because a worker may retain a callback across Stop.
    const auto allocation=reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!allocation)return false;
    size_t cursor{};std::array<uintptr_t,5> prepared{};
    for(size_t i=0;i<prepared.size();++i){cursor=(cursor+15)&~size_t(15);prepared[i]=allocation+cursor;cursor+=detail::bodies[i].size;}
    cursor=(cursor+15)&~size_t(15);const auto valueAddress=allocation+cursor;cursor+=4;
    const auto privateTable=allocation+cursor;cursor+=sizeof(observed);
    if(cursor>4096){VirtualFree(reinterpret_cast<void*>(allocation),0,MEM_RELEASE);return false;}
    const float value=static_cast<float>(threshold)/100.f;std::memcpy(reinterpret_cast<void*>(valueAddress),&value,4);
    for(size_t i=0;i<prepared.size();++i) {
        const auto& body=detail::bodies[i];auto output=reinterpret_cast<uint8_t*>(prepared[i]);
        std::memcpy(output,native[i].data(),body.size);
        for(const auto call:body.calls) {
            uintptr_t target=base+call.target;
            for(size_t j=0;j<prepared.size();++j)if(call.target==detail::bodies[j].rva)target=prepared[j];
            const auto displacement=static_cast<uint32_t>(target-(prepared[i]+call.offset+5));
            std::memcpy(output+call.offset+1,&displacement,4);
        }
    }
    // Native LAHF; TEST AH,44h; JNP means ordered equality with1. The private
    // copy keeps LAHF and tests CF instead: TEST AH,01h; JE is ordered >=.
    // NaN keeps CF=1 and is rejected. No real or copied clutch value is written.
    const auto gate=reinterpret_cast<uint8_t*>(prepared[4]);
    const auto operand=static_cast<uint32_t>(valueAddress);std::memcpy(gate+0xde,&operand,4);
    gate[0xe5]=0x01;gate[0xe6]=0x74;
    // Once the pedal passes the comparison, retry the still-selected gear:
    // clear the rejection latch and enter the original acceptance block.
    // Below-threshold rejection and native neutral/automatic paths are intact.
    // MOV byte ptr [ECX+1e5c],0; JMP gate+2e; NOP replaces the old latch check.
    constexpr uint8_t retry[]{0xc6,0x81,0x5c,0x1e,0,0,0,0xe9,0x2c,0xff,0xff,0xff,0x90};
    std::memcpy(gate+0xf6,retry,sizeof(retry));
    observed[2]=reinterpret_cast<uintptr_t>(&Run);
    std::memcpy(reinterpret_cast<void*>(privateTable),observed.data(),sizeof(observed));
    DWORD previous{};
    if(!VirtualProtect(reinterpret_cast<void*>(allocation),4096,PAGE_EXECUTE_READ,&previous) ||
       !FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(allocation),4096)) {
        VirtualFree(reinterpret_cast<void*>(allocation),0,MEM_RELEASE);return false;
    }
    cache=allocation;clones=prepared;table=privateTable;thresholdAddress=valueAddress;ready=true;return true;
}
bool AttachCandidate(uintptr_t object) {
    uintptr_t actual{};uint32_t count{};
    return enabled.load() && !stopped.load() && ready.load() && JobAddress(object) &&
        MutableJob(object) && Read(object,actual) && actual==base+nativeTable &&
        Read(base+eventCount,count) && count==jobCount;
}
void Attached(uintptr_t object) noexcept {
    const auto index=(object-base-jobPool)/jobSize;
    if(!owned[index].exchange(true))++attached;
    ++instanceWrites;
}
}

void Configure(uintptr_t imageBase,unsigned thresholdPercent,bool requested) {
    std::lock_guard lock(configurationMutex);
    // A session config is immutable once executable copies exist.
    if(cache) {reason="already_configured";return;}
    base=imageBase;threshold=thresholdPercent;
    enabled=requested && threshold>=10 && threshold<=100;
    stopped=!enabled.load();reason=enabled.load()?"waiting_for_job_pool":"native_shift_gate";
}
void Permit(const InputRoute& route,bool allowed,uint64_t sampledAt) {
    std::lock_guard lock(permissionMutex);
    permission={route,allowed && enabled.load() && !stopped.load(),sampledAt};
}
void Tick() {
    std::lock_guard lock(configurationMutex);
    if(!enabled.load() || stopped.load())return;
    if(!ready.load() && !PrepareCode()){enabled=false;reason="native_body_guard_failed";return;}
    const auto permitted=PermissionSnapshot();
    if(!permitted.allowed || !permitted.route.physics)return;
    for(unsigned i=0;i<jobCount;++i) {
        const auto object=base+jobPool+i*jobSize;uintptr_t current{};
        if(!Read(object,current)){reason="job_pool_unreadable";return;}
        if(current==table)continue;
        if(owned[i].exchange(false))--attached;
        if(current!=base+nativeTable){reason="job_pool_foreign_table";return;}
        const auto failure=AttachInputInstance(reinterpret_cast<void*>(object),reinterpret_cast<void*>(base+nativeTable),
            reinterpret_cast<void*>(table),base,AttachCandidate,lifetime,Attached);
        if(failure){reason=failure;return;}
    }
    reason="private_job_instances_attached";
}
void Stop() {
    stopped=true;enabled=false;
    {std::lock_guard lock(permissionMutex);permission.allowed=false;}
    std::lock_guard lock(configurationMutex);
    if(!table)return;
    for(unsigned i=0;i<jobCount;++i) {
        const auto object=base+jobPool+i*jobSize;
        if(!MutableJob(object))continue;
        if(InterlockedCompareExchangePointer(reinterpret_cast<void**>(object),reinterpret_cast<void*>(base+nativeTable),reinterpret_cast<void*>(table))==reinterpret_cast<void*>(table)) {
            ++restores;++instanceWrites;
        }
        if(owned[i].exchange(false))--attached;
    }
    reason="stopped";
}
Status Snapshot() {
    std::lock_guard lock(configurationMutex);
    return {reason.load(),enabled.load(),ready.load(),threshold,attached.load(),callbacks.load(),localJobs.load(),clonedJobs.load(),
        refused.load(),instanceWrites.load(),restores.load()};
}
#ifdef GRID2VR_DRIVING_CONTROLS_SHIFT_TEST
namespace testing {
bool Prepare(uintptr_t imageBase,unsigned thresholdPercent) {
    Configure(imageBase,thresholdPercent,true);std::lock_guard lock(configurationMutex);fixtureImage=true;return enabled.load() && PrepareCode();
}
uintptr_t Clone(unsigned index) {return index<clones.size()?clones[index]:0;}
uintptr_t Table(){return table;}
uintptr_t Threshold(){return thresholdAddress;}
bool Eligible(void* job,const InputRoute& current,uint64_t now){return EligibleImpl(reinterpret_cast<uintptr_t>(job),current,now);}
void Dispatch(void* job,const InputRoute& current,uint64_t now,void* nativeCallback,void* cloneCallback,bool focused) {
    DispatchImpl(job,current,now,reinterpret_cast<RunFn>(nativeCallback),reinterpret_cast<RunFn>(cloneCallback),focused);
}
}
#endif
}
