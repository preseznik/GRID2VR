#include "process_exit.h"
#include "guarded_hooks.h"
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <intrin.h>

namespace grid2vr::processexit {
namespace {
using Exit = void (WINAPI*)(UINT);
Exit original{};
BeforeExit beforeExit{};
BeforeExit observerCleanup{};
DWORD owner{};
uintptr_t host{};
std::filesystem::path receipt;
std::atomic<bool> exiting{};
enum class Handoff { Idle, Pending, Running, Complete, Expired };
std::atomic<Handoff> handoff{};
const char* cleanupResult{};

const char* Cleanup() {
    try { return beforeExit(); } catch(...) { return "callback_exception"; }
}

void WINAPI ExitGame(UINT code) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool onOwner=GetCurrentThreadId()==owner;
    const bool first=!exiting.exchange(true);
    // This hook never catches native faults, changes the exit code, or destroys
    // runtime objects from a foreign thread. A second exit cannot run cleanup twice.
    std::ofstream log(receipt,std::ios::app);
    log << "exit requested code=" << code << " tid=" << GetCurrentThreadId()
        << " owner=" << owner << " caller_rva=" << std::hex << caller-host << std::dec
        << " owner_thread=" << onOwner << " first=" << first << '\n';
    log.flush();
    if(first && observerCleanup) {
        const char* observerResult{};
        try { observerResult=observerCleanup(); } catch(...) { observerResult="callback_exception"; }
        log << "exit observer_cleanup=" << (observerResult?observerResult:"unknown") << '\n';
        log.flush();
    }
    const char* result="reentrant";
    if(first && onOwner) result=Cleanup();
    else if(first) {
        handoff=Handoff::Pending;
        const auto deadline=GetTickCount64()+5000;
        while(handoff.load()!=Handoff::Complete && GetTickCount64()<deadline) Sleep(1);
        if(handoff.load()==Handoff::Complete) result=cleanupResult;
        else {
            auto expected=Handoff::Pending;
            result=handoff.compare_exchange_strong(expected,Handoff::Expired)?"owner_unavailable":"owner_shutdown_timeout";
        }
    }
    log << "exit cleanup=" << (result?result:"unknown") << " forwarding=1\n";
    log.flush();
    original(code);
}
}

void Service() {
    if(!original || GetCurrentThreadId()!=owner) return;
    auto expected=Handoff::Pending;
    if(!handoff.compare_exchange_strong(expected,Handoff::Running)) return;
    cleanupResult=Cleanup();
    handoff=Handoff::Complete;
}

bool Install(uintptr_t base,const std::filesystem::path& output,BeforeExit callback,BeforeExit observerCallback) {
    if(original || !base || !callback) return false;
    // Supported AVX finish routine, including its relocated ExitProcess IAT
    // operand. The hashed executable and both runtime guards must agree.
    constexpr std::array<uint8_t,15> entry{0x55,0x8b,0xec,0x56,0x8b,0xf1,0x80,0xbe,0x94,0,0,0,0,0x75,0x1f};
    std::array<uint8_t,6> call{};
    const auto slot=base+0xb16200;
    uint32_t operand{};
    if(!MatchesInstructions(reinterpret_cast<void*>(base+0x607750),entry) ||
       !ReadMemory(reinterpret_cast<void*>(base+0x607778),call.data(),call.size())) return false;
    memcpy(&operand,call.data()+2,sizeof(operand));
    if(call[0]!=0xff || call[1]!=0x15 || operand!=slot) return false;
    auto target=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"ExitProcess");
    if(!target) return false;
    receipt=output/L"process-exit.txt";
    beforeExit=callback;observerCleanup=observerCallback;owner=GetCurrentThreadId();host=base;
    original=reinterpret_cast<Exit>(target);
    if(!SwapGuardedPointer(reinterpret_cast<void**>(slot),reinterpret_cast<void*>(target),reinterpret_cast<void*>(ExitGame))) {
        original=nullptr;return false;
    }
    return true;
}
}
