#pragma once
#include "guarded_hooks.h"
#include <cstring>

namespace grid2vr::backgroundplay {
inline constexpr uintptr_t focusRoutineRva=0x593480;
inline constexpr uint8_t focusPrologue[]{0x56,0x8b,0xf1,0x8b,0x86,0x20,0x20,0x00,0x00};
inline constexpr uint8_t focusCaller[]{0xe8,0x0c,0xa6,0x30,0x00};
inline constexpr uint8_t pauseMenu[]{0xc6,0x86,0x59,0x21,0x00,0x00,0x01};
inline constexpr uint8_t pauseReplay[]{0x6a,0x02,0xe8,0x02,0x94,0xdb,0xff};
inline bool InstructionsMatch(uintptr_t base) {
    return MatchesInstructions(reinterpret_cast<void*>(base+focusRoutineRva),focusPrologue) &&
        MatchesInstructions(reinterpret_cast<void*>(base+0x288e6f),focusCaller) &&
        MatchesInstructions(reinterpret_cast<void*>(base+0x593492),pauseMenu) &&
        MatchesInstructions(reinterpret_cast<void*>(base+0x593537),pauseReplay);
}
// Background throttle: the main loop ends with `cmp byte [esi+20h],bl; je +8; push 50;
// call [Sleep]`, so once the window has lost focus every frame sleeps 50 ms (~16 fps in VR).
// The call's operand is the image's Sleep import slot, relocated with the image.
inline constexpr uintptr_t throttleRva=0x35ef2f;
inline constexpr uintptr_t throttleReturnRva=0x35ef3c;
inline constexpr uintptr_t sleepImportRva=0xb16188;
inline constexpr uint8_t throttle[]{0x38,0x5e,0x20,0x74,0x08,0x6a,0x32,0xff,0x15};
inline bool ThrottleMatches(uintptr_t base) {
    uint32_t slot{};
    return base && MatchesInstructions(reinterpret_cast<void*>(base+throttleRva),throttle) &&
        ReadMemory(reinterpret_cast<void*>(base+throttleRva+sizeof(throttle)),&slot,sizeof(slot)) && slot==base+sleepImportRva;
}
// Only the throttle's own 50 ms call is skipped; every other Sleep keeps its length.
inline constexpr bool SkipThrottle(uintptr_t returnRva,uint32_t milliseconds) {
    return returnRva==throttleReturnRva && milliseconds==50;
}
inline bool SuppressPauseRequests(uintptr_t base) {
    if(!InstructionsMatch(base)) return false;
    auto* menu=reinterpret_cast<uint8_t*>(base+0x593492);
    auto* replay=reinterpret_cast<uint8_t*>(base+0x593537);
    constexpr SIZE_T length=0x593537+sizeof(pauseReplay)-0x593492;
    // Both sites occupy one code page. Only startup installation writes code;
    // the owned process lifetime restores it, never the executable on disk.
    MEMORY_BASIC_INFORMATION region{};
    if(!VirtualQuery(menu,&region,sizeof(region)) || region.State!=MEM_COMMIT || region.Protect!=PAGE_EXECUTE_READ ||
       reinterpret_cast<uintptr_t>(replay)+sizeof(pauseReplay)>reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize) return false;
    DWORD protection{};
    if(!VirtualProtect(menu,length,PAGE_EXECUTE_READWRITE,&protection)) return false;
    std::memset(menu,0x90,sizeof(pauseMenu));std::memset(replay,0x90,sizeof(pauseReplay));
    DWORD ignored{};
    const bool flushed=FlushInstructionCache(GetCurrentProcess(),menu,length)!=FALSE;
    if(flushed && VirtualProtect(menu,length,protection,&ignored)) return true;
    // A failed install must not silently leave a partial behavior change.
    std::memcpy(menu,pauseMenu,sizeof(pauseMenu));std::memcpy(replay,pauseReplay,sizeof(pauseReplay));
    FlushInstructionCache(GetCurrentProcess(),menu,length);
    VirtualProtect(menu,length,protection,&ignored);
    return false;
}
}
