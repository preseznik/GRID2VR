#include "visibility_dispatch.h"
#include "guarded_hooks.h"
#include <intrin.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <vector>

namespace {
struct Record { uint32_t rva{};std::vector<uint32_t> relocations;std::vector<uint8_t> bytes; };
struct Fixture {
    uint32_t preferredBase{};
    std::vector<Record> records;
    bool Load(const char* path) {
        std::ifstream file(path,std::ios::binary);
        char magic[8]{};uint32_t count{};
        const auto word=[&](uint32_t& value){return bool(file.read(reinterpret_cast<char*>(&value),4));};
        if(!file.read(magic,8) || memcmp(magic,"G2VD0001",8) || !word(preferredBase) ||
            preferredBase!=0x400000 || !word(count) || count!=5)return false;
        for(uint32_t i=0;i<count;++i) {
            Record record;uint32_t length{},relocations{};
            if(!word(record.rva) || !word(length) || !word(relocations) || length>160 || length<4 ||
                relocations>7 || record.rva>=0xe00000 || length>0xe00000-record.rva)return false;
            record.relocations.resize(relocations);record.bytes.resize(length);
            for(auto& offset:record.relocations)if(!word(offset) || offset>length-4)return false;
            if(!file.read(reinterpret_cast<char*>(record.bytes.data()),length))return false;
            records.push_back(std::move(record));
        }
        return file.peek()==EOF;
    }
    Fixture Relocated(uintptr_t base) const {
        Fixture result=*this;
        for(auto& record:result.records)for(auto offset:record.relocations) {
            uint32_t value{};memcpy(&value,record.bytes.data()+offset,4);
            value+=static_cast<uint32_t>(base)-preferredBase;
            memcpy(record.bytes.data()+offset,&value,4);
        }
        return result;
    }
    bool Read(uintptr_t base,uintptr_t at,void* out,size_t bytes) const {
        if(at<base)return false;
        const auto rva=at-base;
        for(const auto& record:records)if(rva>=record.rva && rva-record.rva<=record.bytes.size() &&
            bytes<=record.bytes.size()-(rva-record.rva)) {
            memcpy(out,record.bytes.data()+(rva-record.rva),bytes);return true;
        }
        return false;
    }
};

bool CodeTests(const Fixture& raw) {
    for(uintptr_t base:{uintptr_t(0x400000),uintptr_t(0x22000000)}) {
        auto fixture=raw.Relocated(base);
        const auto read=[&](uintptr_t at,void* out,size_t bytes){return fixture.Read(base,at,out,bytes);};
        if(!grid2vr::VisibilityDispatchCode(base,read))return false;
        // Every fixture byte belongs to a guarded native window or table.
        for(auto& record:fixture.records)for(auto& byte:record.bytes) {
            byte^=1;
            const bool accepted=grid2vr::VisibilityDispatchCode(base,read);
            byte^=1;if(accepted)return false;
        }
        unsigned reads{};
        if(!grid2vr::VisibilityDispatchCode(base,[&](uintptr_t at,void* out,size_t size){
            ++reads;return read(at,out,size);
        }))return false;
        for(unsigned failure=1;failure<=reads;++failure) {
            unsigned index{};
            if(grid2vr::VisibilityDispatchCode(base,[&](uintptr_t at,void* out,size_t size){
                return ++index!=failure && read(at,out,size);
            }))return false;
        }
    }
    constexpr uintptr_t base=0x22000000,main=0x60000000;
    std::array<uintptr_t,7> stack{base+0x78d778,1,main,3,4,base+0x117711,main+0x160};
    if(!grid2vr::VisibilityDispatchStack(stack,base,main) ||
        grid2vr::VisibilityDispatchStack(stack,0,main) ||
        grid2vr::VisibilityDispatchStack(stack,base,0))return false;
    for(size_t count=0;count<7;++count)
        if(grid2vr::VisibilityDispatchStack({stack.data(),count},base,main))return false;
    for(size_t slot:{0u,2u,5u,6u}) {
        stack[slot]^=1;const bool accepted=grid2vr::VisibilityDispatchStack(stack,base,main);stack[slot]^=1;
        if(accepted)return false;
    }
    // Saved non-identity registers are not guessed or treated as pointers.
    stack[1]=stack[3]=stack[4]=0xffffffff;
    return grid2vr::VisibilityDispatchStack(stack,base,main);
}

enum Step { LockStep,NativeResetStep,CallbackStep,SubmitStep,JobStep,UnlockStep };
struct Execution {
    uintptr_t base{},main{},pool{};
    HANDLE event{};
    bool healthy{true},queued{};
    unsigned lockDepth{},resets{},callbacks{},jobs{};
    std::vector<Step> order;
    void* queuedJob{};
} execution;
using ResetFn=int (__thiscall*)(void*);
ResetFn originalReset{};
void Check(bool condition) { execution.healthy&=condition; }
BOOL WINAPI CountReset(HANDLE event) {
    Check(event==execution.event && execution.lockDepth==1);
    ++execution.resets;execution.order.push_back(NativeResetStep);
    return ResetEvent(event);
}
void __fastcall Lock(void* self,void*) {
    Check(uintptr_t(self)==execution.main+0x4c4 && !execution.lockDepth);
    ++execution.lockDepth;execution.order.push_back(LockStep);
}
void __fastcall Unlock(void* self,void*) {
    Check(uintptr_t(self)==execution.main+0x4c4 && execution.lockDepth==1);
    --execution.lockDepth;execution.order.push_back(UnlockStep);
}
void RunJob(void* job) {
    Check(uintptr_t(job)==execution.main+0x160 && execution.callbacks==1 && execution.resets==1);
    Check(WaitForSingleObject(execution.event,0)==WAIT_TIMEOUT);
    Check(execution.lockDepth==(execution.queued?0u:1u));
    ++execution.jobs;execution.order.push_back(JobStep);
}
int __fastcall Submit(void* self,void*,void* job) {
    Check(uintptr_t(self)==execution.pool && uintptr_t(job)==execution.main+0x160 && execution.lockDepth==1);
    Check(*reinterpret_cast<const uint8_t*>(execution.main+0x4fc)==1);
    Check(*reinterpret_cast<const uint32_t*>(execution.main+0x424)==0);
    execution.order.push_back(SubmitStep);
    if(execution.queued)execution.queuedJob=job;else RunJob(job);
    return 0;
}
__declspec(noinline) int __fastcall Reset(void* self,void*) {
    std::array<uintptr_t,7> stack{};
    const bool read=grid2vr::ReadMemory(_AddressOfReturnAddress(),stack.data(),sizeof(stack));
    const auto result=originalReset(self);
    Check(read && grid2vr::VisibilityDispatchStack(stack,execution.base,execution.main));
    Check(uintptr_t(self)==execution.main+grid2vr::visibilityEventOffset && execution.lockDepth==1);
    Check(execution.resets==1 && result==0 && WaitForSingleObject(execution.event,0)==WAIT_TIMEOUT);
    ++execution.callbacks;execution.order.push_back(CallbackStep);
    return result;
}
void Jump(uint8_t* at,const void* target) {
    at[0]=0xe9;const uint32_t relative=static_cast<uint32_t>(uintptr_t(target)-uintptr_t(at+5));
    memcpy(at+1,&relative,4);
}
bool ExecutionTest(const Fixture& raw) {
    static_assert(sizeof(void*)==4,"The retained dispatcher is x86 code.");
    // Fourteen MiB private arena; no game process, game files or executable
    // pages are touched. Only the two retained native functions execute.
    constexpr size_t arenaBytes=0xe00000;
    auto* arena=static_cast<uint8_t*>(VirtualAlloc(nullptr,arenaBytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!arena)return false;
    const auto base=reinterpret_cast<uintptr_t>(arena);
    const auto fixture=raw.Relocated(base);
    for(const auto& record:fixture.records)memcpy(arena+record.rva,record.bytes.data(),record.bytes.size());
    bool healthy=grid2vr::VisibilityDispatchCode(base,[&](uintptr_t at,void* out,size_t bytes){
        return at>=base && at-base<arenaBytes && bytes<=arenaBytes-(at-base) &&
            grid2vr::ReadMemory(reinterpret_cast<void*>(at),out,bytes);
    });
    // Synthetic outer caller reproduces native ESI ownership and call address
    // 11770c, leaving the actual dispatcher bytes entirely unchanged.
    uint8_t outer[]{0x55,0x8b,0xec,0x56,0x8b,0xf1,0x8b,0x4e,0x24,
        0x8d,0x86,0x60,0x01,0,0,0x50,0xe8,0x3f,0x60,0x67,0,0x5e,0x5d,0xc3};
    memcpy(arena+0x1176fc,outer,sizeof(outer));
    Jump(arena+0x392680,reinterpret_cast<const void*>(&Lock));
    Jump(arena+0x648910,reinterpret_cast<const void*>(&Unlock));
    Jump(arena+0x163700,reinterpret_cast<const void*>(&Submit));
    *reinterpret_cast<uintptr_t*>(arena+0xb16270)=reinterpret_cast<uintptr_t>(&CountReset);
    DWORD oldProtection{};
    healthy&=VirtualProtect(arena,arenaBytes,PAGE_EXECUTE_READ,&oldProtection)!=0;
    healthy&=FlushInstructionCache(GetCurrentProcess(),arena,arenaBytes)!=0;
    alignas(16) std::array<uint8_t,0xb00> main{};
    std::array<uintptr_t,3> scheduler{};
    uintptr_t pool{};
    scheduler[2]=reinterpret_cast<uintptr_t>(&pool);
    *reinterpret_cast<uintptr_t*>(main.data()+0x24)=reinterpret_cast<uintptr_t>(scheduler.data());
    auto** nativeTable=reinterpret_cast<void**>(arena+grid2vr::visibilityEventTableRva);
    auto* event=main.data()+grid2vr::visibilityEventOffset;
    *reinterpret_cast<void***>(event)=nativeTable;
    HANDLE handle=CreateEventW(nullptr,TRUE,TRUE,nullptr);
    *reinterpret_cast<HANDLE*>(event+4)=handle;
    originalReset=reinterpret_cast<ResetFn>(arena+0x3701c0);
    const grid2vr::VtablePatch patch{6,reinterpret_cast<void*>(originalReset),reinterpret_cast<void*>(&Reset)};
    auto** shadow=healthy && handle?grid2vr::AttachShadowVtable(event,nativeTable,7,{&patch,1}):nullptr;
    healthy&=shadow!=nullptr;
    if(healthy)for(bool queued:{false,true}) {
        execution={};execution.base=base;execution.main=reinterpret_cast<uintptr_t>(main.data());
        execution.pool=reinterpret_cast<uintptr_t>(&pool);execution.event=handle;execution.queued=queued;
        *reinterpret_cast<uint32_t*>(main.data()+0x424)=2;
        main[0x4fc]=0;
        healthy&=SetEvent(handle)!=0;
        reinterpret_cast<void (__thiscall*)(void*)>(arena+0x1176fc)(main.data());
        if(queued) {
            Check(execution.queuedJob!=nullptr && !execution.lockDepth && !execution.jobs);
            RunJob(execution.queuedJob);
        }
        const std::vector<Step> expected=queued?
            std::vector<Step>{LockStep,NativeResetStep,CallbackStep,SubmitStep,UnlockStep,JobStep}:
            std::vector<Step>{LockStep,NativeResetStep,CallbackStep,SubmitStep,JobStep,UnlockStep};
        healthy&=execution.healthy && execution.order==expected && execution.resets==1 &&
            execution.callbacks==1 && execution.jobs==1 && !execution.lockDepth;
        for(size_t i=0;i<7;++i)healthy&=reinterpret_cast<uintptr_t>(nativeTable[i])==base+grid2vr::visibilityEventMethods[i];
    }
    if(shadow) {
        const bool restored=grid2vr::SwapGuardedPointer(reinterpret_cast<void**>(event),shadow,nativeTable);
        healthy&=restored;if(restored)VirtualFree(shadow-1,0,MEM_RELEASE);
    }
    if(handle)CloseHandle(handle);
    VirtualFree(arena,0,MEM_RELEASE);return healthy;
}
}
int main(int argc,char** argv) {
    if(argc!=2)return 1;
    Fixture fixture;if(!fixture.Load(argv[1]))return 2;
    if(!CodeTests(fixture))return 3;
    if(!ExecutionTest(fixture))return 4;
    puts("Native visibility dispatch guards, seven-word stack, Reset forwarding and pre-job ordering passed.");
}
