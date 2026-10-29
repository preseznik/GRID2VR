#include "quiesced_heap_transaction.h"
#include <tlhelp32.h>
#include <winternl.h>

#if !defined(_MSC_VER) || !defined(_M_IX86)
#error quiesced_heap_transaction requires MSVC Win32/x86.
#endif

namespace grid2vr::quiescence {
namespace {
// Use the SDK's actual structures, not a guessed/truncated process header.
// Their interpretation is ALSO checked against Toolhelp/GetThreadTimes on each
// invocation before suspending anything. An unknown layout is not a fallback.
static_assert(sizeof(void*)==4);
static_assert(sizeof(SYSTEM_PROCESS_INFORMATION)==184);
static_assert(offsetof(SYSTEM_PROCESS_INFORMATION,UniqueProcessId)==68);
static_assert(sizeof(SYSTEM_THREAD_INFORMATION)==64);
static_assert(offsetof(SYSTEM_THREAD_INFORMATION,ClientId)==32);
static_assert(sizeof(NT_TIB)==28 && offsetof(NT_TIB,Self)==24);
struct ThreadBasicInformation {
    NTSTATUS exitStatus;
    void* teb;
    CLIENT_ID clientId;
    ULONG_PTR affinity;
    KPRIORITY priority,basePriority;
};
static_assert(sizeof(ThreadBasicInformation)==28);
using QuerySystem=NTSTATUS (NTAPI*)(SYSTEM_INFORMATION_CLASS,PVOID,ULONG,PULONG);
using QueryThread=NTSTATUS (NTAPI*)(HANDLE,THREADINFOCLASS,PVOID,ULONG,PULONG);
constexpr std::size_t SystemBytes=16*1024*1024;
struct Identity {DWORD id;LONGLONG created;};
struct Peer {
    HANDLE handle;
    DWORD id,priorCount;
    bool suspended;
    void* teb;
    CONTEXT context;
    NT_TIB tib;
};
struct Work {
    Peer peers[MaxThreads];
    Identity baseline[MaxThreads],current[MaxThreads];
    DWORD toolhelp[MaxThreads];
    PointerWrite writes[MaxWrites];
    BusyRange ranges[MaxBusyRanges];
    unsigned char system[SystemBytes];
    uint32_t stack[MaxStackBytes/sizeof(uint32_t)];
};
static_assert(alignof(Work)>=8 && offsetof(Work,system)%8==0);
volatile LONG active{};

bool Busy(uintptr_t value,const BusyRange* ranges,std::size_t count,Result& result) noexcept {
    for(std::size_t i=0;i<count;++i)
        if(value>=ranges[i].begin && value<ranges[i].end) {
            result.busyValue=value;result.busyRangeIndex=i;
            result.busyRangeBegin=ranges[i].begin;result.busyRangeEnd=ranges[i].end;
            return true;
        }
    return false;
}
bool Read(const void* address,void* out,SIZE_T size) noexcept {
    SIZE_T read{};
    return ReadProcessMemory(GetCurrentProcess(),address,out,size,&read) && read==size;
}
bool Readable(DWORD protection) noexcept {
    if(protection&(PAGE_GUARD|PAGE_NOACCESS))return false;
    switch(protection&0xff) {
        case PAGE_READONLY:case PAGE_READWRITE:case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:case PAGE_EXECUTE_READWRITE:case PAGE_EXECUTE_WRITECOPY:return true;
        default:return false;
    }
}
bool WritableSlot(void** slot) noexcept {
    const auto address=reinterpret_cast<uintptr_t>(slot);
    if(!address || (address&3) || address>UINTPTR_MAX-sizeof(void*))return false;
    MEMORY_BASIC_INFORMATION memory;
    if(VirtualQuery(slot,&memory,sizeof(memory))!=sizeof(memory))return false;
    const auto begin=reinterpret_cast<uintptr_t>(memory.BaseAddress);
    // In particular reject writable MEM_IMAGE/.data, mappings, executable pages,
    // copy-on-write and guarded pages. Never call VirtualProtect here.
    return memory.State==MEM_COMMIT && memory.Type==MEM_PRIVATE &&
        memory.Protect==PAGE_READWRITE && address>=begin &&
        address-begin<=memory.RegionSize && memory.RegionSize-(address-begin)>=sizeof(void*);
}
bool Exchange(void** slot,void* expected,void* desired,void*& observed) noexcept {
    // Suspended peers cannot unmap a slot, but fail closed rather than crash if
    // an external writer violates the lifetime contract. No exception logging.
    __try {
        observed=InterlockedCompareExchangePointer(slot,desired,expected);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool SameSet(const Identity* first,std::size_t a,const Identity* second,std::size_t b) noexcept {
    if(a!=b)return false;
    for(std::size_t i=0;i<a;++i) {
        bool found=false;
        for(std::size_t j=0;j<b;++j)
            if(first[i].id==second[j].id && first[i].created==second[j].created){found=true;break;}
        if(!found)return false;
    }
    return true;
}
Failure ParseSystem(const unsigned char* buffer,std::size_t bytes,DWORD process,DWORD caller,
    Identity* identities,std::size_t& count) noexcept {
    if(bytes<sizeof(SYSTEM_PROCESS_INFORMATION) || bytes>SystemBytes)return Failure::ThreadLayout;
    std::size_t offset{};bool found=false;count=0;
    for(;;) {
        if(bytes-offset<sizeof(SYSTEM_PROCESS_INFORMATION))return Failure::ThreadLayout;
        const auto* info=reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(buffer+offset);
        const std::size_t entryBytes=info->NextEntryOffset?info->NextEntryOffset:bytes-offset;
        if(entryBytes<sizeof(*info) || entryBytes>bytes-offset ||
            (info->NextEntryOffset && (info->NextEntryOffset&7)))return Failure::ThreadLayout;
        if(reinterpret_cast<ULONG_PTR>(info->UniqueProcessId)==process) {
            if(found)return Failure::ThreadLayout;
            found=true;
            if(!info->NumberOfThreads)return Failure::ThreadLayout;
            if(info->NumberOfThreads>MaxThreads)return Failure::ThreadLimit;
            if(info->NumberOfThreads>(entryBytes-sizeof(*info))/sizeof(SYSTEM_THREAD_INFORMATION))
                return Failure::ThreadLayout;
            const auto* threads=reinterpret_cast<const SYSTEM_THREAD_INFORMATION*>(info+1);
            bool hasCaller=false;
            for(ULONG i=0;i<info->NumberOfThreads;++i) {
                const auto id=reinterpret_cast<ULONG_PTR>(threads[i].ClientId.UniqueThread);
                // SDK Reserved1[2] is the creation FILETIME. Verify it against
                // each opened handle before relying on it to reject TID reuse.
                const auto created=threads[i].Reserved1[2].QuadPart;
                if(!id || created<=0 || reinterpret_cast<ULONG_PTR>(threads[i].ClientId.UniqueProcess)!=process)
                    return Failure::ThreadLayout;
                for(std::size_t j=0;j<count;++j)if(identities[j].id==id)return Failure::ThreadLayout;
                identities[count++]={static_cast<DWORD>(id),created};
                if(id==caller)hasCaller=true;
            }
            if(!hasCaller)return Failure::ThreadLayout;
        }
        if(!info->NextEntryOffset)break;
        offset+=info->NextEntryOffset;
    }
    return found?Failure::None:Failure::ThreadLayout;
}
Failure Enumerate(QuerySystem query,Work& work,DWORD process,DWORD caller,
    Identity* identities,std::size_t& count,Result& result) noexcept {
    ULONG bytes{};
    const auto status=query(SystemProcessInformation,work.system,static_cast<ULONG>(SystemBytes),&bytes);
    if(status<0){result.ntStatus=status;return Failure::ThreadEnumeration;}
    return ParseSystem(work.system,bytes,process,caller,identities,count);
}
Failure Toolhelp(Work& work,DWORD process,const Identity* identities,std::size_t count,Result& result) noexcept {
    const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if(snapshot==INVALID_HANDLE_VALUE){result.win32Error=GetLastError();return Failure::ThreadSnapshot;}
    THREADENTRY32 entry{};entry.dwSize=sizeof(entry);
    bool more=Thread32First(snapshot,&entry)!=FALSE;
    Failure failure=more?Failure::None:Failure::ThreadSnapshot;
    std::size_t seen{};
    while(more) {
        if(entry.th32OwnerProcessID==process) {
            if(seen==MaxThreads){failure=Failure::ThreadLimit;break;}
            work.toolhelp[seen++]=entry.th32ThreadID;
        }
        entry.dwSize=sizeof(entry);more=Thread32Next(snapshot,&entry)!=FALSE;
    }
    if(failure==Failure::None && GetLastError()!=ERROR_NO_MORE_FILES)failure=Failure::ThreadSnapshot;
    if(failure!=Failure::None)result.win32Error=GetLastError();
    if(!CloseHandle(snapshot)){++result.closeFailures;result.win32Error=GetLastError();}
    if(failure!=Failure::None)return failure;
    if(seen!=count)return Failure::ThreadSetChanged;
    for(std::size_t i=0;i<count;++i) {
        bool found=false;
        for(std::size_t j=0;j<seen;++j)if(identities[i].id==work.toolhelp[j]){found=true;break;}
        if(!found)return Failure::ThreadLayout;
    }
    return Failure::None;
}
Failure Scan(uintptr_t esp,uintptr_t limit,uintptr_t base,const BusyRange* ranges,
    std::size_t rangeCount,uint32_t* scratch,Result& result) noexcept {
    // A complete, bounded scan or NO transaction. Never guess TEB offsets,
    // truncate to a few return addresses, skip unreadable pages or touch guards.
    if(!limit || !esp || (esp&3) || (limit&0xfff) || (base&0xfff) ||
        limit>esp || esp>=base || base-esp>MaxStackBytes)return Failure::StackBounds;
    uintptr_t cursor=esp;
    while(cursor<base) {
        MEMORY_BASIC_INFORMATION memory;
        if(VirtualQuery(reinterpret_cast<void*>(cursor),&memory,sizeof(memory))!=sizeof(memory) ||
            memory.State!=MEM_COMMIT || memory.Type!=MEM_PRIVATE || !Readable(memory.Protect))
            return Failure::StackMemory;
        const auto begin=reinterpret_cast<uintptr_t>(memory.BaseAddress);
        if(begin>cursor || !memory.RegionSize || memory.RegionSize>UINTPTR_MAX-begin)
            return Failure::StackMemory;
        const auto end=begin+memory.RegionSize;
        if(end<=cursor)return Failure::StackMemory;
        cursor=end<base?end:base;
    }
    const auto bytes=base-esp;
    if(!Read(reinterpret_cast<void*>(esp),scratch,bytes))return Failure::StackMemory;
    // ESP and StackBase are aligned: this covers EVERY aligned stack word,
    // including the highest dword immediately below TIB.StackBase.
    result.stackBytesScanned+=bytes;
    for(std::size_t i=0;i<bytes/sizeof(uint32_t);++i)
        if(Busy(scratch[i],ranges,rangeCount,result)) {
            result.busyStackAddress=esp+i*sizeof(uint32_t);return Failure::BusyStack;
        }
    return Failure::None;
}
Failure Preflight(const Work& work,std::size_t count,Result& result) noexcept {
    for(std::size_t i=0;i<count;++i) {
        result.index=i;
        if(!WritableSlot(work.writes[i].slot))return Failure::SlotMemory;
        void* value{};
        if(!Read(work.writes[i].slot,&value,sizeof(value)))return Failure::SlotMemory;
        if(value!=work.writes[i].expected)return Failure::SlotValue;
    }
    return Failure::None;
}
void Cleanup(Work& work,Result& result) noexcept {
    // Exactly one ResumeThread per successful SuspendThread, even on refusal.
    // Do not "resume until zero": preserve a pre-existing suspension count.
    for(std::size_t i=result.peerCount;i>0;--i) {
        auto& peer=work.peers[i-1];
        if(peer.suspended) {
            const auto previous=ResumeThread(peer.handle);
            if(previous==DWORD(-1) || previous!=peer.priorCount+1) {
                ++result.resumeFailures;
                if(previous==DWORD(-1))result.win32Error=GetLastError();
            }
        }
    }
    // Close handles only after every resume attempt; CloseHandle never runs
    // during the critical validation/write interval.
    for(std::size_t i=0;i<result.peerCount;++i)
        if(!CloseHandle(work.peers[i].handle)){++result.closeFailures;result.win32Error=GetLastError();}
}

#ifdef GRID2VR_QUIESCED_HEAP_TRANSACTION_TEST
using TestHooks=testing::Hooks;
#else
struct TestHooks {};
#endif

Failure Execute(const Request& request,Work& work,QuerySystem query,DWORD process,DWORD caller,
    std::size_t baselineCount,Result& result,const TestHooks* hooks) noexcept {
#ifndef GRID2VR_QUIESCED_HEAP_TRANSACTION_TEST
    (void)hooks;
#endif
    // Everything needed below (scratch, handles, contexts, imports and callback
    // addresses) has been prepared. No snapshots, allocators, loggers or waits.
    for(std::size_t i=0;i<result.peerCount;++i) {
        auto& peer=work.peers[i];result.threadId=peer.id;
        peer.priorCount=SuspendThread(peer.handle);
        if(peer.priorCount==DWORD(-1)){result.win32Error=GetLastError();return Failure::ThreadSuspend;}
        peer.suspended=true;++result.suspendedCount;
        // A successful SuspendThread is asynchronous. GetThreadContext forces
        // completion before any quiescence proof or thread-set recheck.
        if(!GetThreadContext(peer.handle,&peer.context)) {
            result.win32Error=GetLastError();return Failure::ThreadContext;
        }
#ifdef GRID2VR_QUIESCED_HEAP_TRANSACTION_TEST
        if(hooks && hooks->failContextIndex==i)return Failure::ThreadContext;
#endif
    }
    std::size_t currentCount{};
    auto failure=Enumerate(query,work,process,caller,work.current,currentCount,result);
    if(failure!=Failure::None)return failure;
#ifdef GRID2VR_QUIESCED_HEAP_TRANSACTION_TEST
    // Inject a late record into the enumerated set, NOT an unconditional failure:
    // the test exercises the same comparison used for a real late/new thread.
    if(hooks && hooks->forceThreadSetMismatch) {
        if(currentCount<MaxThreads)work.current[currentCount++]={MAXDWORD,1};
        else --currentCount;
    }
#endif
    if(!SameSet(work.baseline,baselineCount,work.current,currentCount))return Failure::ThreadSetChanged;
    for(std::size_t i=0;i<result.peerCount;++i) {
        auto& peer=work.peers[i];result.threadId=peer.id;
        result.instruction=peer.context.Eip;result.stackPointer=peer.context.Esp;
        if(Busy(peer.context.Eip,work.ranges,request.busyRanges.size(),result))return Failure::BusyInstruction;
        if(!Read(peer.teb,&peer.tib,sizeof(peer.tib)) || peer.tib.Self!=peer.teb)return Failure::ThreadTeb;
        failure=Scan(peer.context.Esp,reinterpret_cast<uintptr_t>(peer.tib.StackLimit),
            reinterpret_cast<uintptr_t>(peer.tib.StackBase),work.ranges,request.busyRanges.size(),work.stack,result);
        if(failure!=Failure::None)return failure;
    }
    result.threadId=0;result.instruction=0;result.stackPointer=0;
    if(!request.validate(request.context))return Failure::ValidationRefused;
    failure=Preflight(work,request.writes.size(),result);
    if(failure!=Failure::None)return failure;
    // Validator must not create threads; a second no-allocation recheck makes
    // even a set change during validation a refusal before the first write.
    failure=Enumerate(query,work,process,caller,work.current,currentCount,result);
    if(failure!=Failure::None)return failure;
    if(!SameSet(work.baseline,baselineCount,work.current,currentCount))return Failure::ThreadSetChanged;
    if(request.probeOnly) {result.probePassed=true;return Failure::None;}
    for(std::size_t i=0;i<request.writes.size();++i) {
        result.index=i;
#ifdef GRID2VR_QUIESCED_HEAP_TRANSACTION_TEST
        if(hooks && hooks->beforeWrite)hooks->beforeWrite(hooks->context,i);
#endif
        const auto& write=work.writes[i];void* observed{};
        if(!Exchange(write.slot,write.expected,write.desired,observed) || observed!=write.expected) {
            // Reverse-order rollback is conditional too. Do not clobber any
            // foreign values, including ones installed into an earlier slot.
            for(std::size_t j=i;j>0;--j) {
                const auto& undo=work.writes[j-1];void* old{};
                if(WritableSlot(undo.slot) && Exchange(undo.slot,undo.desired,undo.expected,old) && old==undo.desired)
                    result.rolledBackMask|=1u<<(j-1);
                else result.rollbackConflictMask|=1u<<(j-1);
            }
            return Failure::CompareExchange;
        }
        result.writtenMask|=1u<<i;
    }
    result.committed=true;
    if(request.committed)request.committed(request.context);
    return Failure::None;
}

uint64_t Microseconds(LONGLONG ticks,LONGLONG frequency) noexcept {
    const auto t=static_cast<uint64_t>(ticks),f=static_cast<uint64_t>(frequency);
    return (t/f)*1000000+(t%f)*1000000/f;
}
bool Clock(LARGE_INTEGER& frequency,LARGE_INTEGER& begin) noexcept {
    return QueryPerformanceFrequency(&frequency) && frequency.QuadPart>0 &&
        static_cast<uint64_t>(frequency.QuadPart)<=UINT64_MAX/1000000 && QueryPerformanceCounter(&begin);
}
Result Run(const Request& input,const TestHooks* hooks) noexcept {
    Result result;
    if(!input.enabled){result.failure=Failure::Disabled;return result;}
    if(input.writes.empty() || input.writes.size()>MaxWrites || !input.writes.data() ||
        input.busyRanges.empty() || input.busyRanges.size()>MaxBusyRanges || !input.busyRanges.data() || !input.validate) {
        result.failure=Failure::InvalidRequest;return result;
    }
    for(std::size_t i=0;i<input.writes.size();++i) {
        const auto& write=input.writes[i];
        if(!write.slot || (reinterpret_cast<uintptr_t>(write.slot)&3) || write.expected==write.desired) {
            result.failure=Failure::InvalidRequest;return result;
        }
        for(std::size_t j=0;j<i;++j)if(input.writes[j].slot==write.slot) {
            result.failure=Failure::InvalidRequest;return result;
        }
    }
    for(const auto& range:input.busyRanges)if(!range.begin || range.begin>=range.end) {
        result.failure=Failure::InvalidRequest;return result;
    }
    if(InterlockedCompareExchange(&active,1,0)!=0){result.failure=Failure::ConcurrentTransaction;return result;}
    auto* work=static_cast<Work*>(VirtualAlloc(nullptr,sizeof(Work),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!work) {
        result.failure=Failure::Allocation;result.win32Error=GetLastError();
        InterlockedExchange(&active,0);return result;
    }
    // Copy all transaction descriptors before pausing. Do not depend on a
    // caller's container, lazy function resolver or callback initialization.
    for(std::size_t i=0;i<input.writes.size();++i)work->writes[i]=input.writes[i];
    for(std::size_t i=0;i<input.busyRanges.size();++i)work->ranges[i]=input.busyRanges[i];
    const Request request{true,{work->writes,input.writes.size()},{work->ranges,input.busyRanges.size()},
        input.validate,input.committed,input.context,input.probeOnly};
    const auto ntdll=GetModuleHandleW(L"ntdll.dll");
    const auto querySystem=ntdll?reinterpret_cast<QuerySystem>(GetProcAddress(ntdll,"NtQuerySystemInformation")):nullptr;
    const auto queryThread=ntdll?reinterpret_cast<QueryThread>(GetProcAddress(ntdll,"NtQueryInformationThread")):nullptr;
    if(!querySystem || !queryThread)result.failure=Failure::NtUnavailable;
    else result.failure=Preflight(*work,request.writes.size(),result);
    const auto process=GetCurrentProcessId(),caller=GetCurrentThreadId();
    std::size_t baselineCount{};
    if(result.failure==Failure::None)
        result.failure=Enumerate(querySystem,*work,process,caller,work->baseline,baselineCount,result);
    if(result.failure==Failure::None)
        result.failure=Toolhelp(*work,process,work->baseline,baselineCount,result);
#ifdef GRID2VR_QUIESCED_HEAP_TRANSACTION_TEST
    if(result.failure==Failure::None && hooks && hooks->forceLayoutMismatch)result.failure=Failure::ThreadLayout;
#endif
    for(std::size_t i=0;result.failure==Failure::None && i<baselineCount;++i) {
        const auto identity=work->baseline[i];
        const bool self=identity.id==caller;
        const auto handle=self?GetCurrentThread():OpenThread(
            THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,identity.id);
        if(!handle){result.failure=Failure::ThreadOpen;result.win32Error=GetLastError();break;}
        Peer* peer=nullptr;
        if(!self) {
            peer=&work->peers[result.peerCount++];peer->handle=handle;peer->id=identity.id;
            peer->context.ContextFlags=CONTEXT_CONTROL;
        }
        result.threadId=identity.id;
        FILETIME created,exit,kernel,user;
        if(!GetThreadTimes(handle,&created,&exit,&kernel,&user)) {
            result.failure=Failure::ThreadIdentity;result.win32Error=GetLastError();break;
        }
        const auto timestamp=(static_cast<ULONGLONG>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
        if(timestamp!=static_cast<ULONGLONG>(identity.created)){result.failure=Failure::ThreadLayout;break;}
        ThreadBasicInformation info;ULONG length{};
        const auto status=queryThread(handle,static_cast<THREADINFOCLASS>(0),&info,sizeof(info),&length);
        if(status<0) {result.failure=Failure::ThreadTeb;result.ntStatus=status;break;}
        if(length!=sizeof(info) || !info.teb || info.clientId.UniqueProcess!=reinterpret_cast<HANDLE>(process) ||
            info.clientId.UniqueThread!=reinterpret_cast<HANDLE>(identity.id)) {
            result.failure=Failure::ThreadLayout;break;
        }
        NT_TIB tib;
        if(!Read(info.teb,&tib,sizeof(tib)) || tib.Self!=info.teb) {result.failure=Failure::ThreadTeb;break;}
        if(peer)peer->teb=info.teb;
    }
    LARGE_INTEGER pauseFrequency{},pauseBegin{},pauseEnd{};
    bool pauseClock{};
    if(result.failure==Failure::None && !result.closeFailures) {
        result.layoutValidated=true;
        // All clock initialization is BEFORE any peer is paused. No new clock,
        // allocation, formatting or I/O call is added to the suspended interval.
        pauseClock=Clock(pauseFrequency,pauseBegin);
        result.failure=Execute(request,*work,querySystem,process,caller,baselineCount,result,hooks);
    }
    Cleanup(*work,result);
    if(pauseClock && QueryPerformanceCounter(&pauseEnd) && pauseEnd.QuadPart>=pauseBegin.QuadPart) {
        result.pauseScopeMeasured=true;
        result.pauseScopeMicroseconds=Microseconds(pauseEnd.QuadPart-pauseBegin.QuadPart,pauseFrequency.QuadPart);
    }
    if(!VirtualFree(work,0,MEM_RELEASE)){++result.closeFailures;result.win32Error=GetLastError();}
    InterlockedExchange(&active,0);
    return result;
}
Result TimedRun(const Request& request,const TestHooks* hooks) noexcept {
    LARGE_INTEGER frequency{},begin{},end{};
    const bool clock=Clock(frequency,begin);
    auto result=Run(request,hooks);
    if(clock && QueryPerformanceCounter(&end) && end.QuadPart>=begin.QuadPart) {
        result.timingAvailable=true;
        result.totalMicroseconds=Microseconds(end.QuadPart-begin.QuadPart,frequency.QuadPart);
    }
    return result;
}
}

Result Transact(const Request& request) noexcept {return TimedRun(request,nullptr);}
const char* FailureName(Failure failure) noexcept {
    switch(failure) {
#define QHT_NAME(x) case Failure::x:return #x;
        QHT_NAME(None) QHT_NAME(Disabled) QHT_NAME(InvalidRequest) QHT_NAME(ConcurrentTransaction)
        QHT_NAME(Allocation) QHT_NAME(NtUnavailable) QHT_NAME(ThreadSnapshot) QHT_NAME(ThreadEnumeration)
        QHT_NAME(ThreadLayout) QHT_NAME(ThreadLimit) QHT_NAME(ThreadSetChanged) QHT_NAME(ThreadOpen)
        QHT_NAME(ThreadIdentity) QHT_NAME(ThreadTeb) QHT_NAME(ThreadSuspend) QHT_NAME(ThreadContext)
        QHT_NAME(BusyInstruction) QHT_NAME(StackBounds) QHT_NAME(StackMemory) QHT_NAME(BusyStack)
        QHT_NAME(ValidationRefused) QHT_NAME(SlotMemory) QHT_NAME(SlotValue) QHT_NAME(CompareExchange)
#undef QHT_NAME
        default:return "Unknown";
    }
}
#ifdef GRID2VR_QUIESCED_HEAP_TRANSACTION_TEST
namespace testing {
Result TransactWithHooks(const Request& request,const Hooks& hooks) noexcept {return TimedRun(request,&hooks);}
Result ScanStack(uintptr_t esp,uintptr_t limit,uintptr_t base,std::span<const BusyRange> ranges) noexcept {
    Result result;
    if(ranges.empty() || ranges.size()>MaxBusyRanges){result.failure=Failure::InvalidRequest;return result;}
    auto* scratch=static_cast<uint32_t*>(VirtualAlloc(nullptr,MaxStackBytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!scratch){result.failure=Failure::Allocation;return result;}
    result.failure=Scan(esp,limit,base,ranges.data(),ranges.size(),scratch,result);
    if(!VirtualFree(scratch,0,MEM_RELEASE))++result.closeFailures;
    return result;
}
Result ValidateSystemSnapshot(std::span<const std::byte> bytes,DWORD process,DWORD caller) noexcept {
    Result result;Identity identities[MaxThreads];std::size_t count{};
    if(!bytes.data() || (reinterpret_cast<uintptr_t>(bytes.data())&7)) {
        result.failure=Failure::ThreadLayout;return result;
    }
    result.failure=ParseSystem(reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size(),process,caller,identities,count);
    if(result.failure==Failure::None){result.layoutValidated=true;result.peerCount=count-1;}
    return result;
}
}
#endif
}
