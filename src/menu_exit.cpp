#include "menu_exit.h"
#include "menu_exit_guard.h"
#include "process_exit.h"
#include <fstream>
#include <mutex>
#include <set>

namespace grid2vr::menuexit {
namespace {
using Enter=void (__thiscall*)(void*,void*);
using Evaluate=const char** (__thiscall*)(void*,const char**);
using Leave=bool (__thiscall*)(void*,float);
uintptr_t host{};
std::filesystem::path receipt;
std::mutex mutex;
// Identity keys only: never dereference a saved object or call it from another
// thread. Native evaluation/leave supplies the live instance. Entry clears a
// reused address, and leave removes a canceled transition.
std::set<void*> pending;
ULONGLONG deadline{};
bool installed{};
constexpr std::array<uintptr_t,2> entries{guard::endCurrentRva,guard::retireEventRva};

void Log(const char* event,unsigned kind) {
    std::ofstream(receipt,std::ios::app) << event << " state=" << kind
        << " tid=" << GetCurrentThreadId() << '\n';
}

bool TerminalFinish() {
    std::array<uint8_t,17> flags{};
    if(!ReadMemory(reinterpret_cast<void*>(host+guard::finishObjectRva+0x84),flags.data(),flags.size())) return false;
    uint32_t mode{};memcpy(&mode,flags.data(),4);
    // The guarded finish routine calls ExitProcess(0) iff these are true.
    return mode==2 && flags[4]!=0 && flags[16]==0;
}

template<unsigned Kind> void __fastcall Begin(void* self,void*,void* argument) {
    {
        std::lock_guard lock(mutex);
        pending.erase(self);
        if(TerminalFinish() && processexit::RequestCleanup()) {
            if(!deadline) deadline=GetTickCount64()+5000;
            pending.insert(self);
            // Keep the native state initialized, but defer its terminal side
            // effect. The exact base routine ignores its argument.
            reinterpret_cast<Enter>(host+guard::baseEnterRva)(self,argument);
            Log("menu exit requested=1 deferred=1",Kind);
            return;
        }
    }
    reinterpret_cast<Enter>(host+entries[Kind])(self,argument);
}

template<unsigned Kind> const char** __fastcall Poll(void* self,void*,const char** output) {
    // Preserve base evaluation bookkeeping and its one-word string ABI.
    const auto result=reinterpret_cast<Evaluate>(host+guard::evaluateRva)(self,output);
    bool resume=false;
    {
        std::lock_guard lock(mutex);
        if(pending.contains(self)) {
            if(!processexit::CleanupReady(deadline)) {
                *result=nullptr; // Native frame/time-delay "no transition" result.
                return result;
            }
            pending.erase(self); // Remove before invoking potentially reentrant native code.
            resume=true;
            Log("menu exit deferred_entry_resumed=1",Kind);
        }
    }
    if(resume) {
        // Both guarded entry bodies pass this argument only to baseEnter, which
        // ignores it. They overwrite that stack slot before any subsequent use.
        // Do not retain a caller's possibly temporary argument across frames.
        reinterpret_cast<Enter>(host+entries[Kind])(self,nullptr);
    }
    return result;
}

template<unsigned Kind> bool __fastcall End(void* self,void*,float elapsed) {
    {
        std::lock_guard lock(mutex);
        if(pending.erase(self)) {
            Log("menu exit deferred_state_left=1",Kind);
            if(pending.empty() && processexit::CancelCleanup()) deadline=0;
        }
    }
    return reinterpret_cast<Leave>(host+guard::leaveRva)(self,elapsed);
}
}

bool Install(uintptr_t base,const std::filesystem::path& output) {
    if(installed) return host==base;
    if(!base ||
       !guard::Match(base,guard::endCurrentRva,guard::endCurrent,guard::endCurrentRelocations) ||
       !guard::Match(base,guard::retireEventRva,guard::retireEvent,guard::retireEventRelocations) ||
       !guard::Match(base,guard::baseEnterRva,guard::baseEnter,guard::baseEnterRelocations) ||
       !guard::Match(base,guard::evaluateRva,guard::evaluate,guard::evaluateRelocations) ||
       !guard::Match(base,guard::leaveRva,guard::leave,guard::leaveRelocations) ||
       !guard::Match(base,guard::finishRva,guard::finish,guard::finishRelocations)) return false;
    struct Patch { void** slot;void* expected;void* replacement; };
    std::array<Patch,6> patches{};
    const std::array<void*,2> begin{reinterpret_cast<void*>(Begin<0>),reinterpret_cast<void*>(Begin<1>)};
    const std::array<void*,2> poll{reinterpret_cast<void*>(Poll<0>),reinterpret_cast<void*>(Poll<1>)};
    const std::array<void*,2> end{reinterpret_cast<void*>(End<0>),reinterpret_cast<void*>(End<1>)};
    for(unsigned i=0;i<2;++i) {
        auto** table=reinterpret_cast<void**>(base+guard::tables[i]);
        patches[i*3]={table+3,reinterpret_cast<void*>(base+entries[i]),begin[i]};
        patches[i*3+1]={table+4,reinterpret_cast<void*>(base+guard::leaveRva),end[i]};
        patches[i*3+2]={table+7,reinterpret_cast<void*>(base+guard::evaluateRva),poll[i]};
    }
    for(const auto& patch:patches) {
        void* actual{};
        if(!ReadMemory(patch.slot,&actual,sizeof(actual)) || actual!=patch.expected) return false;
    }
    host=base;receipt=output/L"menu-exit.txt";
    size_t applied{};
    for(;applied<patches.size();++applied) {
        const auto& patch=patches[applied];
        if(!SwapGuardedPointer(patch.slot,patch.expected,patch.replacement)) break;
    }
    if(applied!=patches.size()) {
        while(applied) {
            const auto& patch=patches[--applied];
            SwapGuardedPointer(patch.slot,patch.replacement,patch.expected);
        }
        return false;
    }
    installed=true;Log("menu exit gate installed=1",0);return true;
}
}
