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
std::atomic<HWND> closeWindow{};
std::atomic<WNDPROC> previousWindowProc{};
UINT_PTR closeTimer{}; // The following close-message fields belong to the UI thread.
UINT closeMessage{};
WPARAM closeWparam{};
LPARAM closeLparam{};
ULONGLONG closeDeadline{};
bool closeReleased{},closeRunningTimeout{};

void CloseLog(const char* event) {
    std::ofstream(receipt.parent_path()/L"window-close.txt",std::ios::app)
        << event << " tid=" << GetCurrentThreadId() << " owner=" << owner << '\n';
}

void CALLBACK CloseTimer(HWND window,UINT,UINT_PTR timer,DWORD) {
    if(window!=closeWindow.load() || timer!=closeTimer) return;
    auto state=handoff.load();
    if(state==Handoff::Pending && GetTickCount64()>=closeDeadline) {
        auto expected=Handoff::Pending;
        if(handoff.compare_exchange_strong(expected,Handoff::Expired)) {
            CloseLog("close cleanup=owner_unavailable forwarding=1");state=Handoff::Expired;
        } else state=expected;
    }
    if(state==Handoff::Running && GetTickCount64()>=closeDeadline && !closeRunningTimeout) {
        // Do not tear the device down underneath an in-progress owner callback.
        // The message pump stays responsive while that callback finishes.
        closeRunningTimeout=true;CloseLog("close cleanup=still_running forwarding=0");
    }
    if(state!=Handoff::Complete && state!=Handoff::Expired) return;
    KillTimer(window,timer);closeTimer=0;closeReleased=true;
    if(state==Handoff::Complete) {
        CloseLog(cleanupResult?cleanupResult:"callback_unknown");
        CloseLog("close owner_serviced=1 forwarding=1");
    }
    // Preserve the exact native message and chain, on its owning UI thread.
    CallWindowProcW(previousWindowProc.load(),window,closeMessage,closeWparam,closeLparam);
}

LRESULT CALLBACK CloseWindowProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    const auto previous=previousWindowProc.load();
    const bool closing=message==WM_CLOSE || (message==WM_SYSCOMMAND && (wparam&0xfff0)==SC_CLOSE);
    if(window==closeWindow.load() && closing && !closeReleased && !exiting.load()) {
        const auto state=handoff.load();
        if(state==Handoff::Pending || state==Handoff::Running) return 0;
        if(state==Handoff::Idle) {
            closeMessage=message;closeWparam=wparam;closeLparam=lparam;
            closeDeadline=GetTickCount64()+5000;
            closeTimer=SetTimer(window,reinterpret_cast<UINT_PTR>(CloseTimer),15,CloseTimer);
            if(closeTimer) {
                auto expected=Handoff::Idle;
                if(handoff.compare_exchange_strong(expected,Handoff::Pending)) {
                    CloseLog("close requested=1 deferred=1");return 0;
                }
                KillTimer(window,closeTimer);closeTimer=0;
            } else CloseLog("close deferred=0 reason=timer_unavailable");
        }
    }
    if(message==WM_NCDESTROY && window==closeWindow.load()) {
        if(closeTimer) {KillTimer(window,closeTimer);closeTimer=0;}
        auto expected=Handoff::Pending;
        handoff.compare_exchange_strong(expected,Handoff::Expired);
        // Never remove a later subclass or touch a reused HWND.
        if(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window,GWLP_WNDPROC))==CloseWindowProc)
            SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(previous));
        closeWindow=nullptr;
    }
    return CallWindowProcW(previous,window,message,wparam,lparam);
}

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
    const bool prepared=handoff.load()==Handoff::Complete;
    if(first && observerCleanup && !prepared) {
        const char* observerResult{};
        try { observerResult=observerCleanup(); } catch(...) { observerResult="callback_exception"; }
        log << "exit observer_cleanup=" << (observerResult?observerResult:"unknown") << '\n';
        log.flush();
    }
    const char* result="reentrant";
    if(first && prepared) result=cleanupResult;
    else if(first && handoff.load()==Handoff::Expired) result="owner_unavailable";
    else if(first && onOwner) {
        result=cleanupResult=Cleanup();handoff=Handoff::Complete;
    }
    else if(first) {
        auto idle=Handoff::Idle;
        handoff.compare_exchange_strong(idle,Handoff::Pending);
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

bool AttachWindow(HWND window) {
    if(!original || !window || GetCurrentThreadId()!=owner) return false;
    if(closeWindow.load()) return closeWindow.load()==window;
    DWORD process{};
    if(!GetWindowThreadProcessId(window,&process) || process!=GetCurrentProcessId() ||
       GetAncestor(window,GA_ROOT)!=window || exiting.load() || handoff.load()!=Handoff::Idle) return false;
    const auto previous=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window,GWLP_WNDPROC));
    if(!previous || previous==CloseWindowProc) return false;
    previousWindowProc=previous;closeWindow=window;
    SetLastError(0);
    const auto replaced=SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(CloseWindowProc));
    if(!replaced && GetLastError()) {closeWindow=nullptr;return false;}
    if(replaced) previousWindowProc=reinterpret_cast<WNDPROC>(replaced);
    CloseLog("close window_attached=1");return true;
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
