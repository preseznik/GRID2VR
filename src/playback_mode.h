#pragma once
#include <windows.h>
#include <cstdint>
namespace grid2vr::playercontrol {
// The native routine's mode 1 copies local input; mode 0 decodes recorded input.
// Change it only for the duration of that routine, and never overwrite a conflict.
inline bool ExchangeMode(volatile LONG* mode,LONG expected,LONG replacement) {
    if (!mode || reinterpret_cast<uintptr_t>(mode)%alignof(LONG)) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(const_cast<LONG*>(mode),&region,sizeof(region)) ||
        region.State!=MEM_COMMIT || region.Protect!=PAGE_READWRITE) return false;
    __try { return InterlockedCompareExchange(mode,replacement,expected)==expected; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
using PlaybackCall=bool (*)(void* context);
// Keep SEH restoration here, outside functions with C++ objects requiring unwind.
inline bool CallWithLocalInput(volatile LONG* mode,PlaybackCall call,void* context,bool& restored) {
    restored=false;
    bool result{};
    __try { result=call(context); }
    __finally { restored=ExchangeMode(mode,1,0); }
    return result;
}
}
