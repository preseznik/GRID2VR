#include "quiesced_heap_transaction.h"
#include <winternl.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>
#include <utility>

#if !defined(_M_IX86) || !defined(GRID2VR_QUIESCED_HEAP_TRANSACTION_TEST)
#error This fixture requires MSVC x86 and GRID2VR_QUIESCED_HEAP_TRANSACTION_TEST.
#endif
#define CHECK(x) do {if(!(x)){std::printf("line %d: %s\n",__LINE__,#x);return false;}} while(false)
namespace q=grid2vr::quiescence;
namespace {
void* Ptr(uintptr_t n) noexcept {return reinterpret_cast<void*>(n);}
void Describe(const q::Result& r) {
    std::printf("  status=%s committed=%u layout=%u peers=%zu suspended=%zu stack=%zu written=%x rollback=%x foreign=%x resume=%u close=%u tid=%lu index=%zu win32=%lu nt=%08lx\n",
        q::FailureName(r.failure),r.committed,r.layoutValidated,r.peerCount,r.suspendedCount,r.stackBytesScanned,
        r.writtenMask,r.rolledBackMask,r.rollbackConflictMask,r.resumeFailures,r.closeFailures,
        r.threadId,r.index,r.win32Error,r.ntStatus);
}
struct Fixture {
    void** slots=static_cast<void**>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    unsigned char* rx=static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    std::array<q::PointerWrite,2> writes{};
    q::BusyRange range{};
    volatile LONG validations{},publications{},badPublication{};
    bool allowed=true;
    bool valid{};
    Fixture() {
        if(!slots || !rx)return;
        std::memset(rx,0xcc,4096);DWORD old{};
        if(!VirtualProtect(rx,4096,PAGE_EXECUTE_READ,&old))return;
        slots[0]=Ptr(0x11110010);slots[1]=Ptr(0x22220020);
        writes={q::PointerWrite{slots,slots[0],Ptr(0x33330030)},q::PointerWrite{slots+1,slots[1],Ptr(0x44440040)}};
        range={reinterpret_cast<uintptr_t>(rx),reinterpret_cast<uintptr_t>(rx)+4096};valid=true;
    }
    ~Fixture() {if(slots)VirtualFree(slots,0,MEM_RELEASE);if(rx)VirtualFree(rx,0,MEM_RELEASE);}
    static bool Validate(void* context) noexcept {
        auto& f=*static_cast<Fixture*>(context);InterlockedIncrement(&f.validations);return f.allowed;
    }
    static void Publish(void* context) noexcept {
        auto& f=*static_cast<Fixture*>(context);
        if(f.slots[0]!=f.writes[0].desired || f.slots[1]!=f.writes[1].desired)
            InterlockedExchange(&f.badPublication,1);
        InterlockedIncrement(&f.publications);
    }
    q::Request Request() noexcept {return {true,writes,{&range,1},Validate,Publish,this};}
    bool Original() const noexcept {return slots[0]==writes[0].expected && slots[1]==writes[1].expected;}
};

bool SuccessAndRestore() {
    Fixture f;CHECK(f.valid);
    auto r=q::Transact(f.Request());Describe(r);
    CHECK(r && r.committed && r.layoutValidated && r.writtenMask==3 && !r.rolledBackMask && !r.rollbackConflictMask);
    CHECK(f.validations==1 && f.publications==1 && !f.badPublication);
    for(auto& write:f.writes){const auto old=write.expected;write.expected=write.desired;write.desired=old;}
    r=q::Transact(f.Request());Describe(r);
    CHECK(r && r.writtenMask==3 && f.validations==2 && f.publications==2 && !f.badPublication);
    CHECK(f.slots[0]==Ptr(0x11110010) && f.slots[1]==Ptr(0x22220020));
    return true;
}
bool MaximumBatch() {
    Fixture f;CHECK(f.valid);
    std::array<q::PointerWrite,q::MaxWrites> writes;
    for(std::size_t i=0;i<writes.size();++i) {
        f.slots[i]=Ptr(0x11110000+i*16);
        writes[i]={f.slots+i,f.slots[i],Ptr(0x22220000+i*16)};
    }
    auto request=f.Request();request.writes=writes;request.committed=nullptr;
    const auto r=q::Transact(request);Describe(r);
    CHECK(r && r.writtenMask==0xff && f.validations==1);
    for(const auto& write:writes)CHECK(*write.slot==write.desired);
    return true;
}
bool ProbeOnlyNoWrites() {
    Fixture f;CHECK(f.valid);auto request=f.Request();request.probeOnly=true;
    auto r=q::Transact(request);Describe(r);
    CHECK(r.failure==q::Failure::None && r.probePassed && !r && !r.committed && !r.writtenMask);
    CHECK(r.layoutValidated && r.timingAvailable && r.pauseScopeMeasured && r.totalMicroseconds>=r.pauseScopeMicroseconds);
    CHECK(f.Original() && f.validations==1 && !f.publications && !r.resumeFailures && !r.closeFailures);
    f.allowed=false;r=q::Transact(request);
    CHECK(r.failure==q::Failure::ValidationRefused && !r.probePassed && f.Original());
    f.slots[1]=Ptr(0x55550050);r=q::Transact(request);
    CHECK(r.failure==q::Failure::SlotValue && !r.probePassed && !r.committed && !r.writtenMask && !f.publications);
    return true;
}
bool DisabledAndInvalid() {
    Fixture f;CHECK(f.valid);auto request=f.Request();request.enabled=false;
    CHECK(q::Transact(request).failure==q::Failure::Disabled && f.Original());
    request=f.Request();request.validate=nullptr;
    CHECK(q::Transact(request).failure==q::Failure::InvalidRequest);
    request=f.Request();request.writes={};
    CHECK(q::Transact(request).failure==q::Failure::InvalidRequest);
    request=f.Request();request.busyRanges={};
    CHECK(q::Transact(request).failure==q::Failure::InvalidRequest);
    std::array<q::PointerWrite,q::MaxWrites+1> tooMany{};request=f.Request();request.writes=tooMany;
    CHECK(q::Transact(request).failure==q::Failure::InvalidRequest);
    std::array<q::BusyRange,q::MaxBusyRanges+1> tooManyRanges{};request=f.Request();request.busyRanges=tooManyRanges;
    CHECK(q::Transact(request).failure==q::Failure::InvalidRequest);
    f.range.end=f.range.begin;
    CHECK(q::Transact(f.Request()).failure==q::Failure::InvalidRequest);
    f.range.end=f.range.begin+4096;
    const auto first=f.writes[0];f.writes[0].slot=nullptr;
    CHECK(q::Transact(f.Request()).failure==q::Failure::InvalidRequest);
    f.writes[0]=first;f.writes[1]=first;
    CHECK(q::Transact(f.Request()).failure==q::Failure::InvalidRequest);
    f.writes[1]={f.slots+1,Ptr(0x22220020),Ptr(0x44440040)};
    f.writes[0].desired=f.writes[0].expected;
    CHECK(q::Transact(f.Request()).failure==q::Failure::InvalidRequest);
    f.writes[0]=first;f.writes[0].slot=reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(f.slots)+1);
    CHECK(q::Transact(f.Request()).failure==q::Failure::InvalidRequest);
    CHECK(!f.publications && !f.validations);
    return true;
}
void* imageSlot=reinterpret_cast<void*>(0x11110010);
bool ForeignAndSlotMemory() {
    Fixture f;CHECK(f.valid);
    f.slots[1]=Ptr(0x55550050);
    CHECK(q::Transact(f.Request()).failure==q::Failure::SlotValue);
    CHECK(f.slots[0]==f.writes[0].expected && f.slots[1]==Ptr(0x55550050));
    f.slots[1]=f.writes[1].expected;
    const auto first=f.writes[0];
    f.writes[0].slot=&imageSlot;
    CHECK(q::Transact(f.Request()).failure==q::Failure::SlotMemory && imageSlot==first.expected);
    f.writes[0].slot=reinterpret_cast<void**>(&ForeignAndSlotMemory);
    CHECK(q::Transact(f.Request()).failure==q::Failure::SlotMemory);
    f.writes[0].slot=reinterpret_cast<void**>(f.rx);
    CHECK(q::Transact(f.Request()).failure==q::Failure::SlotMemory);
    for(unsigned i=0;i<4096;++i)CHECK(f.rx[i]==0xcc);
    MEMORY_BASIC_INFORMATION rxMemory{};CHECK(VirtualQuery(f.rx,&rxMemory,sizeof(rxMemory))==sizeof(rxMemory));
    CHECK(rxMemory.Protect==PAGE_EXECUTE_READ);
    f.writes[0].slot=reinterpret_cast<void**>(0x10000);
    CHECK(q::Transact(f.Request()).failure==q::Failure::SlotMemory);
    const auto mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,4096,nullptr);
    CHECK(mapping);
    auto** mapped=static_cast<void**>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,4096));
    if(!mapped){CloseHandle(mapping);CHECK(mapped);}
    *mapped=first.expected;f.writes[0].slot=mapped;
    const auto r=q::Transact(f.Request());const bool untouched=*mapped==first.expected;
    const bool unmapped=UnmapViewOfFile(mapped)!=FALSE,closed=CloseHandle(mapping)!=FALSE;
    CHECK(r.failure==q::Failure::SlotMemory && untouched && unmapped && closed);
    f.writes[0]=first;
    DWORD old{};CHECK(VirtualProtect(f.slots,4096,PAGE_READONLY,&old));
    const auto readOnly=q::Transact(f.Request());
    CHECK(VirtualProtect(f.slots,4096,old,&old));
    CHECK(readOnly.failure==q::Failure::SlotMemory && f.Original());
    CHECK(!f.publications && !f.validations);
    return true;
}
bool ValidatorRefuses() {
    Fixture f;CHECK(f.valid);f.allowed=false;
    auto r=q::Transact(f.Request());Describe(r);
    CHECK(r.failure==q::Failure::ValidationRefused && !r.committed && !r.writtenMask && !r.resumeFailures && !r.closeFailures);
    CHECK(f.validations==1 && !f.publications && f.Original());
    return true;
}
bool ChangeDuringValidation(void* context) noexcept {
    auto& f=*static_cast<Fixture*>(context);InterlockedIncrement(&f.validations);
    InterlockedExchangePointer(f.slots+1,Ptr(0x55550050));return true;
}
bool CompletePreflightAfterValidation() {
    Fixture f;CHECK(f.valid);auto request=f.Request();request.validate=ChangeDuringValidation;
    const auto r=q::Transact(request);Describe(r);
    CHECK(r.failure==q::Failure::SlotValue && !r.committed && !r.writtenMask);
    CHECK(f.slots[0]==f.writes[0].expected && f.slots[1]==Ptr(0x55550050));
    CHECK(f.validations==1 && !f.publications && !r.resumeFailures && !r.closeFailures);
    return true;
}
struct Mutation {Fixture* f;bool replaceFirst;};
void Mutate(void* context,std::size_t index) noexcept {
    if(index!=1)return;
    auto& m=*static_cast<Mutation*>(context);
    InterlockedExchangePointer(m.f->slots+1,Ptr(0x55550050));
    if(m.replaceFirst)InterlockedExchangePointer(m.f->slots,Ptr(0x66660060));
}
bool ConditionalRollback() {
    for(bool replaceFirst:{false,true}) {
        Fixture f;CHECK(f.valid);Mutation mutation{&f,replaceFirst};
        q::testing::Hooks hooks;hooks.beforeWrite=Mutate;hooks.context=&mutation;
        const auto r=q::testing::TransactWithHooks(f.Request(),hooks);Describe(r);
        CHECK(r.failure==q::Failure::CompareExchange && !r.committed && r.writtenMask==1 && !r.resumeFailures && !r.closeFailures);
        CHECK(r.rolledBackMask==(replaceFirst?0u:1u) && r.rollbackConflictMask==(replaceFirst?1u:0u));
        CHECK(f.slots[0]==(replaceFirst?Ptr(0x66660060):f.writes[0].expected) && f.slots[1]==Ptr(0x55550050));
        CHECK(f.validations==1 && !f.publications);
    }
    return true;
}

struct SpinState {
    volatile LONG ready{},stop{};
    uint32_t marker{};
    volatile DWORD entryEsp{},markerAt{};
};
static_assert(offsetof(SpinState,entryEsp)==12 && offsetof(SpinState,markerAt)==16);
// Entirely test-owned instructions: no game code and no window/message loop.
// A 3500-byte-deep word deliberately defeats the old 128-dword heuristic.
__declspec(naked) DWORD WINAPI Spin(void*) {
    __asm {
        mov ecx,[esp+4]
        sub esp,4096
        mov dword ptr [esp],0
        mov [ecx+12],esp
        lea edx,[esp+3500]
        mov [ecx+16],edx
        mov eax,[ecx+8]
        mov [edx],eax
        mov eax,1
        xchg [ecx],eax
    again:
        cmp dword ptr [ecx+4],0
        je again
        add esp,4096
        xor eax,eax
        ret 4
    }
}
struct Worker {
    SpinState state;
    HANDLE handle{};
    explicit Worker(uint32_t marker=0) {
        state.marker=marker;handle=CreateThread(nullptr,128*1024,Spin,&state,0,nullptr);
        if(handle)for(unsigned i=0;i<5000 && !state.ready;++i)Sleep(1);
    }
    ~Worker() {
        if(handle){InterlockedExchange(&state.stop,1);WaitForSingleObject(handle,10000);CloseHandle(handle);}
    }
    bool Ready() const noexcept {return handle && state.ready;}
};
bool PeerEipBusy() {
    Fixture f;CHECK(f.valid);Worker worker;CHECK(worker.Ready());
    f.range={reinterpret_cast<uintptr_t>(&Spin),reinterpret_cast<uintptr_t>(&Spin)+96};
    const auto r=q::Transact(f.Request());Describe(r);
    CHECK(r.failure==q::Failure::BusyInstruction && r.suspendedCount>=1 && !r.committed && !r.resumeFailures && !r.closeFailures);
    CHECK(r.instruction>=f.range.begin && r.instruction<f.range.end && r.busyValue==r.instruction && !r.busyStackAddress);
    CHECK(r.threadId==GetThreadId(worker.handle) && r.stackPointer && r.busyRangeIndex==0 &&
        r.busyRangeBegin==f.range.begin && r.busyRangeEnd==f.range.end);
    CHECK(f.Original() && !f.validations && !f.publications);
    return true;
}
bool DeepBusyStack() {
    Fixture f;CHECK(f.valid);Worker worker(static_cast<uint32_t>(f.range.begin+123));CHECK(worker.Ready());
    CHECK(worker.state.markerAt-worker.state.entryEsp==3500 && worker.state.markerAt-worker.state.entryEsp>128*4);
    const auto r=q::Transact(f.Request());Describe(r);
    CHECK(r.failure==q::Failure::BusyStack && r.stackBytesScanned>3500 && r.suspendedCount>=1);
    CHECK(r.threadId==GetThreadId(worker.handle) && r.instruction && r.stackPointer && r.busyStackAddress>=r.stackPointer);
    CHECK(r.busyValue==worker.state.marker && r.busyRangeIndex==0 &&
        r.busyRangeBegin==f.range.begin && r.busyRangeEnd==f.range.end);
    CHECK(r.timingAvailable && r.pauseScopeMeasured && r.totalMicroseconds>=r.pauseScopeMicroseconds);
    CHECK(!r.committed && !r.resumeFailures && !r.closeFailures && f.Original() && !f.validations && !f.publications);
    auto probe=f.Request();probe.probeOnly=true;const auto observed=q::Transact(probe);
    CHECK(observed.failure==q::Failure::BusyStack && !observed.probePassed && !observed.committed && f.Original());
    return true;
}
bool StackBoundsAndCompleteness() {
    Fixture f;CHECK(f.valid);
    auto* memory=static_cast<uint32_t*>(VirtualAlloc(nullptr,2*q::MaxStackBytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    CHECK(memory);const auto address=reinterpret_cast<uintptr_t>(memory);
    const auto scan=[&](uintptr_t esp,uintptr_t limit,uintptr_t base) {
        return q::testing::ScanStack(esp,limit,base,{&f.range,1});
    };
    // Keep all assertions after release, so even a failed fixture frees scratch.
    const auto exact=scan(address,address,address+q::MaxStackBytes);
    memory[q::MaxStackBytes/4-1]=static_cast<uint32_t>(f.range.begin+17);
    const auto last=scan(address,address,address+q::MaxStackBytes);
    memory[q::MaxStackBytes/4-1]=0;
    const auto oversized=scan(address,address,address+q::MaxStackBytes+4096);
    const auto below=scan(address,address+4096,address+8192);
    const auto empty=scan(address,address,address);
    const auto misaligned=scan(address+1,address,address+4096);
    const auto badLimit=scan(address,address+1,address+4096);
    const auto badBase=scan(address,address,address+4095);
    DWORD old{};const bool protectedPage=VirtualProtect(memory+1024,4096,PAGE_NOACCESS,&old)!=FALSE;
    const auto unreadable=scan(address,address,address+8192);
    DWORD unused{};const bool restored=VirtualProtect(memory+1024,4096,old,&unused)!=FALSE;
    const bool guarded=VirtualProtect(memory+1024,4096,PAGE_READWRITE|PAGE_GUARD,&old)!=FALSE;
    const auto guard=scan(address,address,address+8192);
    MEMORY_BASIC_INFORMATION information{};VirtualQuery(memory+1024,&information,sizeof(information));
    const bool guardUntouched=(information.Protect&PAGE_GUARD)!=0;
    const bool restoredGuard=VirtualProtect(memory+1024,4096,old,&unused)!=FALSE;
    const bool freed=VirtualFree(memory,0,MEM_RELEASE)!=FALSE;
    CHECK(exact.failure==q::Failure::None && exact.stackBytesScanned==q::MaxStackBytes);
    CHECK(last.failure==q::Failure::BusyStack && last.stackBytesScanned==q::MaxStackBytes);
    CHECK(last.busyValue==f.range.begin+17 && last.busyStackAddress==address+q::MaxStackBytes-4 && last.busyRangeIndex==0);
    CHECK(exact.busyRangeIndex==q::MaxBusyRanges && !exact.busyValue && !exact.busyStackAddress);
    CHECK(oversized.failure==q::Failure::StackBounds && !oversized.stackBytesScanned);
    CHECK(below.failure==q::Failure::StackBounds && empty.failure==q::Failure::StackBounds);
    CHECK(misaligned.failure==q::Failure::StackBounds && badLimit.failure==q::Failure::StackBounds && badBase.failure==q::Failure::StackBounds);
    CHECK(protectedPage && restored && guarded && restoredGuard && guardUntouched && freed);
    CHECK(unreadable.failure==q::Failure::StackMemory && guard.failure==q::Failure::StackMemory);
    return true;
}
bool NtLayoutParserBounds() {
    static_assert(sizeof(SYSTEM_PROCESS_INFORMATION)==184 && sizeof(SYSTEM_THREAD_INFORMATION)==64);
    alignas(8) std::array<std::byte,2*(184+64)> bytes{};
    auto* info=reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(bytes.data());
    auto* thread=reinterpret_cast<SYSTEM_THREAD_INFORMATION*>(info+1);
    constexpr DWORD process=100,caller=200;
    const auto reset=[&]() {
        bytes.fill(std::byte{});info->NumberOfThreads=1;info->UniqueProcessId=Ptr(process);
        thread->ClientId={Ptr(process),Ptr(caller)};thread->Reserved1[2].QuadPart=12345;
    };
    const auto parse=[&](std::size_t size=184+64) {
        return q::testing::ValidateSystemSnapshot(std::span(bytes).first(size),process,caller);
    };
    reset();auto r=parse();CHECK(r.failure==q::Failure::None && r.layoutValidated && r.peerCount==0);
    CHECK(parse(183).failure==q::Failure::ThreadLayout);
    CHECK(parse(184+63).failure==q::Failure::ThreadLayout);
    info->NumberOfThreads=static_cast<ULONG>(q::MaxThreads+1);
    CHECK(parse().failure==q::Failure::ThreadLimit);
    reset();info->NextEntryOffset=4;
    CHECK(parse().failure==q::Failure::ThreadLayout);
    reset();info->NextEntryOffset=static_cast<ULONG>(bytes.size()+8);
    CHECK(parse().failure==q::Failure::ThreadLayout);
    reset();thread->ClientId.UniqueThread=Ptr(caller+1);
    CHECK(parse().failure==q::Failure::ThreadLayout);
    reset();thread->ClientId.UniqueProcess=Ptr(process+1);
    CHECK(parse().failure==q::Failure::ThreadLayout);
    reset();thread->Reserved1[2].QuadPart=0;
    CHECK(parse().failure==q::Failure::ThreadLayout);
    reset();info->NumberOfThreads=2;thread[1]=thread[0];
    CHECK(parse(bytes.size()).failure==q::Failure::ThreadLayout);
    thread[1].ClientId.UniqueThread=Ptr(caller+1);thread[1].Reserved1[2].QuadPart=23456;
    r=parse(bytes.size());CHECK(r.failure==q::Failure::None && r.peerCount==1);
    reset();info->NextEntryOffset=184+64;
    std::memcpy(bytes.data()+184+64,bytes.data(),184+64);
    auto* second=reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(bytes.data()+184+64);second->NextEntryOffset=0;
    CHECK(parse(bytes.size()).failure==q::Failure::ThreadLayout); // Duplicate process.
    return true;
}
bool LateThreadAndUnknownLayout() {
    Fixture f;CHECK(f.valid);Worker worker;CHECK(worker.Ready());
    q::testing::Hooks hooks;hooks.forceThreadSetMismatch=true;
    auto r=q::testing::TransactWithHooks(f.Request(),hooks);Describe(r);
    CHECK(r.failure==q::Failure::ThreadSetChanged && r.suspendedCount>=1 && !r.resumeFailures && !r.closeFailures);
    CHECK(f.Original() && !f.validations && !f.publications);
    hooks.forceThreadSetMismatch=false;hooks.forceLayoutMismatch=true;
    r=q::testing::TransactWithHooks(f.Request(),hooks);Describe(r);
    CHECK(r.failure==q::Failure::ThreadLayout && !r.suspendedCount && !r.resumeFailures && !r.closeFailures);
    CHECK(f.Original() && !f.validations && !f.publications);
    return true;
}
bool SuspendBalanceAndHandles() {
    Fixture f;CHECK(f.valid);Worker worker;CHECK(worker.Ready());
    const auto prior=SuspendThread(worker.handle);CHECK(prior==0);
    auto r=q::Transact(f.Request());Describe(r);
    const auto after=SuspendThread(worker.handle);
    const auto firstResume=ResumeThread(worker.handle),secondResume=ResumeThread(worker.handle);
    CHECK(after==1 && firstResume==2 && secondResume==1);
    CHECK(r && r.suspendedCount>=1 && !r.resumeFailures && !r.closeFailures);
    for(auto& write:f.writes){const auto old=write.expected;write.expected=write.desired;write.desired=old;}
    q::testing::Hooks hooks;hooks.failContextIndex=0;
    r=q::testing::TransactWithHooks(f.Request(),hooks);Describe(r);
    const auto afterFailure=SuspendThread(worker.handle);const auto resumeFailure=ResumeThread(worker.handle);
    CHECK(afterFailure==0 && resumeFailure==1);
    CHECK(r.failure==q::Failure::ThreadContext && r.suspendedCount==1 && !r.resumeFailures && !r.closeFailures && f.Original());
    DWORD before{},afterHandles{};CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    for(unsigned i=0;i<32;++i) {
        r=q::testing::TransactWithHooks(f.Request(),hooks);
        CHECK(r.failure==q::Failure::ThreadContext && !r.resumeFailures && !r.closeFailures);
        r=q::Transact(f.Request());CHECK(r);
        for(auto& write:f.writes){const auto old=write.expected;write.expected=write.desired;write.desired=old;}
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&afterHandles));
    CHECK(before==afterHandles);
    return true;
}
struct ImageReceipt {uint64_t hash;std::vector<std::pair<void*,DWORD>> protections;};
ImageReceipt Fingerprint() {
    ImageReceipt result{1469598103934665603ull,{}};
    const auto* image=reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(image+dos->e_lfanew);
    const auto* sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
        if(!(sections[i].Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
        const auto* begin=image+sections[i].VirtualAddress;
        for(DWORD j=0;j<sections[i].Misc.VirtualSize;++j){result.hash^=begin[j];result.hash*=1099511628211ull;}
        for(DWORD j=0;j<sections[i].Misc.VirtualSize;j+=4096) {
            MEMORY_BASIC_INFORMATION memory{};VirtualQuery(begin+j,&memory,sizeof(memory));
            result.protections.emplace_back(memory.BaseAddress,memory.Protect);
        }
    }
    return result;
}
}
int main() {
    const auto before=Fingerprint();
    const std::array tests{
        std::pair{"success_two_slots_and_restore",SuccessAndRestore},
        std::pair{"maximum_eight_write_batch",MaximumBatch},
        std::pair{"diagnostic_full_guard_probe_never_writes_or_publishes",ProbeOnlyNoWrites},
        std::pair{"disabled_invalid_misaligned_and_bounds",DisabledAndInvalid},
        std::pair{"foreign_nonprivate_image_executable_readonly_slots",ForeignAndSlotMemory},
        std::pair{"validator_refusal",ValidatorRefuses},
        std::pair{"all_slots_preflighted_after_validator",CompletePreflightAfterValidation},
        std::pair{"conditional_rollback_preserves_foreign_values",ConditionalRollback},
        std::pair{"peer_eip_busy",PeerEipBusy},
        std::pair{"peer_busy_word_3500_bytes_deep",DeepBusyStack},
        std::pair{"full_stack_scan_exact_cap_and_rejections",StackBoundsAndCompleteness},
        std::pair{"sdk_nt_layout_parser_bounds_and_thread_cap",NtLayoutParserBounds},
        std::pair{"late_thread_set_and_unknown_layout_seams",LateThreadAndUnknownLayout},
        std::pair{"prior_suspend_count_context_refusal_and_handle_balance",SuspendBalanceAndHandles}
    };
    unsigned passed{};
    for(const auto& test:tests) {
        std::printf("TEST %s\n",test.first);
        if(!test.second()){std::printf("FAIL %s\n",test.first);return 1;}
        ++passed;std::printf("PASS %s\n",test.first);
    }
    const auto after=Fingerprint();
    if(before.hash!=after.hash || before.protections!=after.protections) {
        std::printf("FAIL executable image bytes/protections changed\n");return 1;
    }
    std::printf("PASS executable image unchanged: fnv1a64=%016llx pages=%zu\n",after.hash,after.protections.size());
    std::printf("PASS %u test groups; x86 SDK SPI=184 STI=64; no game launched/attached\n",passed);
    return 0;
}
