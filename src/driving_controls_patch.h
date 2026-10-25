#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <array>
#include <cstring>
#include <span>
#include "driving_controls_model.h"

namespace grid2vr::drivingcontrols {
struct CallPatch { uint8_t* site;void* original;void* replacement; };
inline bool CallBytesMatch(const CallPatch& patch,bool replaced=false) {
    const auto expected=RelativeCall(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(patch.site)),
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(replaced?patch.replacement:patch.original)));
    std::array<uint8_t,5> actual{};SIZE_T read{};
    return ReadProcessMemory(GetCurrentProcess(),patch.site,actual.data(),actual.size(),&read) && read==actual.size() && actual==expected;
}

// Bounded, process-local transaction. All handles and writable pages are
// prepared before suspending peers. The suspended interval only uses stack
// buffers, memory copies and kernel calls; no allocation, streams or CRT locks.
inline const char* PatchCalls(std::span<const CallPatch> patches) {
    if(patches.empty() || patches.size()>7)return "callsite_count";
    struct Pages {
        struct Page { void* address{};DWORD protection{};bool writable{}; };
        std::array<Page,7> items{};size_t count{};DWORD size{};
        bool Restore() {
            bool ok=true;
            for(size_t i=count;i--;)if(items[i].writable) {
                DWORD ignored{};
                if(VirtualProtect(items[i].address,size,items[i].protection,&ignored))items[i].writable=false;
                else ok=false;
            }
            return ok;
        }
        bool Writable() {
            bool ok=true;
            for(size_t i=0;i<count;++i)if(!items[i].writable) {
                DWORD ignored{};
                if(VirtualProtect(items[i].address,size,PAGE_EXECUTE_READWRITE,&ignored))items[i].writable=true;
                else ok=false;
            }
            return ok;
        }
        ~Pages() { Restore(); }
    } pages;
    SYSTEM_INFO system{};GetSystemInfo(&system);pages.size=system.dwPageSize;
    if(!pages.size || (pages.size&(pages.size-1)))return "callsite_page_size";
    for(size_t i=0;i<patches.size();++i) {
        const auto& patch=patches[i];
        if(!patch.site || !patch.original || !patch.replacement || !CallBytesMatch(patch))return "callsite_instruction_guard";
        const auto address=reinterpret_cast<uintptr_t>(patch.site);
        for(size_t j=0;j<i;++j) {
            const auto other=reinterpret_cast<uintptr_t>(patches[j].site);
            if(address<other+5 && other<address+5)return "callsite_overlap";
        }
        const auto page=address&~static_cast<uintptr_t>(pages.size-1);
        if(address+4>=page+pages.size)return "callsite_page_crossing";
        bool known{};for(size_t j=0;j<pages.count;++j)known|=pages.items[j].address==reinterpret_cast<void*>(page);
        if(!known)pages.items[pages.count++].address=reinterpret_cast<void*>(page);
    }
    struct Peers {
        struct Peer { HANDLE handle{};bool suspended{}; };
        std::array<Peer,1024> items{};size_t count{};
        ~Peers() {
            for(size_t i=0;i<count;++i)if(items[i].suspended)ResumeThread(items[i].handle);
            for(size_t i=0;i<count;++i)CloseHandle(items[i].handle);
        }
    } peers;
    const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if(snapshot==INVALID_HANDLE_VALUE)return "callsite_thread_snapshot";
    THREADENTRY32 entry{};entry.dwSize=sizeof(entry);
    bool enumerated=Thread32First(snapshot,&entry)!=FALSE;
    const char* failure=enumerated?nullptr:"callsite_thread_enumeration";
    while(enumerated) {
        if(entry.th32OwnerProcessID==GetCurrentProcessId() && entry.th32ThreadID!=GetCurrentThreadId()) {
            if(peers.count==peers.items.size()) { failure="callsite_thread_limit";break; }
            const auto thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ThreadID);
            if(thread)peers.items[peers.count++].handle=thread;
            else if(GetLastError()!=ERROR_INVALID_PARAMETER) { failure="callsite_thread_open";break; }
        }
        entry.dwSize=sizeof(entry);enumerated=Thread32Next(snapshot,&entry)!=FALSE;
    }
    if(!failure && GetLastError()!=ERROR_NO_MORE_FILES)failure="callsite_thread_enumeration";
    CloseHandle(snapshot);if(failure)return failure;
    for(size_t i=0;i<pages.count;++i) {
        if(!VirtualProtect(pages.items[i].address,pages.size,PAGE_EXECUTE_READWRITE,&pages.items[i].protection))return "callsite_page_protection";
        pages.items[i].writable=true;
    }
    for(size_t i=0;i<peers.count;++i) {
        auto& peer=peers.items[i];
        if(SuspendThread(peer.handle)==DWORD(-1)) {
            DWORD exit{};
            if(GetExitCodeThread(peer.handle,&exit) && exit!=STILL_ACTIVE)continue;
            failure="callsite_thread_suspend";break;
        }
        peer.suspended=true;
        CONTEXT context{};context.ContextFlags=CONTEXT_CONTROL;
        if(!GetThreadContext(peer.handle,&context)) { failure="callsite_thread_context";break; }
        for(const auto& patch:patches)if(InCallInstruction(context.Eip,static_cast<uint32_t>(reinterpret_cast<uintptr_t>(patch.site)))) {
            failure="callsite_thread_in_instruction";break;
        }
        if(failure)break;
    }
    if(!failure)for(const auto& patch:patches)if(!CallBytesMatch(patch)) { failure="callsite_instruction_changed";break; }
    if(failure) { return pages.Restore()?failure:"callsite_restore_protection"; }
    for(const auto& patch:patches) {
        const auto bytes=RelativeCall(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(patch.site)),static_cast<uint32_t>(reinterpret_cast<uintptr_t>(patch.replacement)));
        std::memcpy(patch.site+1,bytes.data()+1,4); // Keep the native CALL opcode.
    }
    for(const auto& patch:patches)if(!CallBytesMatch(patch,true) || !FlushInstructionCache(GetCurrentProcess(),patch.site,5))failure="callsite_write_or_flush";
    if(!pages.Restore())failure="callsite_restore_protection";
    if(failure) {
        bool rolledBack=pages.Writable();
        if(rolledBack)for(const auto& patch:patches) {
            const auto bytes=RelativeCall(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(patch.site)),static_cast<uint32_t>(reinterpret_cast<uintptr_t>(patch.original)));
            std::memcpy(patch.site+1,bytes.data()+1,4);
            rolledBack=FlushInstructionCache(GetCurrentProcess(),patch.site,5) && CallBytesMatch(patch) && rolledBack;
        }
        rolledBack=pages.Restore() && rolledBack;
        return rolledBack?failure:"callsite_rollback_failed";
    }
    return nullptr;
}
}
