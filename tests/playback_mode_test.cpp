#include "playback_mode.h"
#include <cstdio>
using namespace grid2vr::playercontrol;

struct State { volatile LONG* mode; bool entered{}; bool conflict{}; bool raise{}; };
bool NativeFixture(void* context) {
    auto& state=*static_cast<State*>(context);
    state.entered=*state.mode==1;
    if (state.conflict) InterlockedExchange(state.mode,2);
    if (state.raise) RaiseException(0xe0420001,0,0,nullptr);
    return false;
}
bool CatchNativeException(State* state,bool* restored) {
    __try { CallWithLocalInput(state->mode,NativeFixture,state,*restored); }
    __except(GetExceptionCode()==0xe0420001?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return true; }
    return false;
}
int main() {
    auto* page=static_cast<LONG*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if (!page) return 1;
    bool restored{};State state{page};
    if (!ExchangeMode(page,0,1) || CallWithLocalInput(page,NativeFixture,&state,restored) ||
        !state.entered || !restored || *page!=0) return 2;
    state={page,false,true,false};
    if (!ExchangeMode(page,0,1)) return 3;
    CallWithLocalInput(page,NativeFixture,&state,restored);
    if (restored || *page!=2 || ExchangeMode(page,0,1) || *page!=2) return 4;
    *page=0;state={page,false,false,true};
    if (!ExchangeMode(page,0,1) || !CatchNativeException(&state,&restored) || !restored || *page!=0) return 5;
    DWORD old{};
    if (!VirtualProtect(page,4096,PAGE_READONLY,&old) || ExchangeMode(page,0,1) || *page!=0) return 6;
    if (ExchangeMode(nullptr,0,1) || ExchangeMode(reinterpret_cast<LONG*>(reinterpret_cast<uintptr_t>(page)+1),0,1)) return 7;
    VirtualFree(page,0,MEM_RELEASE);
    if (ExchangeMode(page,0,1)) return 8;
    std::puts("Scoped local input restores on normal return and SEH; conflicts and protected memory are retained.");
    return 0;
}
