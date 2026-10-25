#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <array>
#include <cstdint>

namespace grid2vr::drivingcontrols {
// The shadow table is fully prepared before this transaction. Peers are paused
// only for final lifetime checks and one atomic write to the heap object. No
// game image page is writable; no allocator, logger or CRT lock runs suspended.
inline const char* AttachInputInstance(void* object,void* expected,void* shadow,
    uintptr_t base,bool (*validate)(uintptr_t)) {
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
        if((rva>=0xc6530 && rva<0xc68b0) || (rva>=0x485cf0 && rva<0x485e4f) ||
           (rva>=0x333530 && rva<0x333551) || (rva>=0x3b5dd0 && rva<0x3b5e7a)) {
            failure="input_instance_busy";break;
        }
    }
    if(failure)return failure;
    if(!validate(reinterpret_cast<uintptr_t>(object)))return "input_instance_changed";
    if(InterlockedCompareExchangePointer(static_cast<void**>(object),shadow,expected)!=expected)return "input_instance_changed";
    return nullptr;
}
}
