#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <array>
#include <cstdint>
#include <span>

namespace grid2vr::drivingcontrols {
struct InstanceBusyRange { uintptr_t begin,end; };
inline bool InstanceInstructionBusy(uintptr_t rva,std::span<const InstanceBusyRange> extra={}) {
    if((rva>=0xc6530 && rva<0xc68b0) || (rva>=0x485cf0 && rva<0x485e4f) ||
       (rva>=0x333530 && rva<0x333551) || (rva>=0x3b5dd0 && rva<0x3b5e7a))return true;
    for(const auto& range:extra)if(rva>=range.begin && rva<range.end)return true;
    return false;
}
// The shadow table is fully prepared before this transaction. Peers are paused
// only for final lifetime checks and one atomic instance-pointer write. Page
// protections stay unchanged; no allocator, logger or CRT lock runs suspended.
inline const char* AttachInputInstance(void* object,void* expected,void* shadow,
    uintptr_t base,bool (*validate)(uintptr_t),std::span<const InstanceBusyRange> extra={},void (*committed)(uintptr_t) noexcept=nullptr,
    bool checkPeerStack=false) {
    struct Peer { HANDLE handle{};bool suspended{}; };
    struct Peers {
        std::array<Peer,1024> items{};size_t count{};
        ~Peers() {
            for(size_t i=0;i<count;++i)if(items[i].suspended)ResumeThread(items[i].handle);
            for(size_t i=0;i<count;++i)CloseHandle(items[i].handle);
        }
    } peers;
    const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if(snapshot==INVALID_HANDLE_VALUE)return "input_thread_snapshot";
    THREADENTRY32 entry{};entry.dwSize=sizeof(entry);
    bool more=Thread32First(snapshot,&entry)!=FALSE;
    const char* failure=more?nullptr:"input_thread_enumeration";
    while(more) {
        if(entry.th32OwnerProcessID==GetCurrentProcessId() && entry.th32ThreadID!=GetCurrentThreadId()) {
            if(peers.count==peers.items.size()){failure="input_thread_limit";break;}
            const auto thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ThreadID);
            if(thread)peers.items[peers.count++].handle=thread;
            else if(GetLastError()!=ERROR_INVALID_PARAMETER){failure="input_thread_open";break;}
        }
        entry.dwSize=sizeof(entry);more=Thread32Next(snapshot,&entry)!=FALSE;
    }
    if(!failure && GetLastError()!=ERROR_NO_MORE_FILES)failure="input_thread_enumeration";
    CloseHandle(snapshot);if(failure)return failure;
    for(size_t i=0;i<peers.count;++i) {
        auto& peer=peers.items[i];
        if(SuspendThread(peer.handle)==DWORD(-1)) {
            DWORD exit{};if(GetExitCodeThread(peer.handle,&exit) && exit!=STILL_ACTIVE)continue;
            failure="input_thread_suspend";break;
        }
        peer.suspended=true;
        CONTEXT context{};context.ContextFlags=CONTEXT_CONTROL;
        if(!GetThreadContext(peer.handle,&context)){failure="input_thread_context";break;}
        const auto rva=uintptr_t(context.Eip)-base;
        if(InstanceInstructionBusy(rva,extra)) {
            failure="input_instance_busy";break;
        }
        if(checkPeerStack) {
            // An event-registry peer may be inside an allocator called by a
            // lifetime method. Its EIP alone cannot identify that state. This
            // conservative bounded check is opt-in; unrelated users are unchanged.
            std::array<uint32_t,128> stack{};SIZE_T bytes{};
            if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(context.Esp),stack.data(),sizeof(stack),&bytes) || bytes!=sizeof(stack)) {
                failure="input_thread_stack";break;
            }
            for(const auto address:stack)for(const auto& range:extra)
                if(uintptr_t(address)-base>=range.begin && uintptr_t(address)-base<range.end)failure="input_instance_busy_stack";
            if(failure)break;
        }
    }
    if(failure)return failure;
    if(!validate(reinterpret_cast<uintptr_t>(object)))return "input_instance_changed";
    if(InterlockedCompareExchangePointer(static_cast<void**>(object),shadow,expected)!=expected)return "input_instance_changed";
    // Publish ownership only after the swap, before peers resume. This callback
    // must use only lock-free state; failed transactions preserve prior ownership.
    if(committed)committed(reinterpret_cast<uintptr_t>(object));
    return nullptr;
}
}
