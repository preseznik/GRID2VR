#include "background_play.h"
#include "background_play_patch.h"
#include <array>
#include <cstdio>

using namespace grid2vr::backgroundplay;
int main() {
    auto* allocation=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x600000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!allocation)return 1;
    const auto base=reinterpret_cast<uintptr_t>(allocation);
    std::memcpy(allocation+focusRoutineRva,focusPrologue,sizeof(focusPrologue));
    std::memcpy(allocation+0x288e6f,focusCaller,sizeof(focusCaller));
    std::memcpy(allocation+0x593492,pauseMenu,sizeof(pauseMenu));
    std::memcpy(allocation+0x593537,pauseReplay,sizeof(pauseReplay));
    // A changed guard rejects the entire patch, including the other valid site.
    allocation[0x593537]^=1;
    DWORD previous{};
    if(!VirtualProtect(allocation,0x600000,PAGE_EXECUTE_READ,&previous))return 2;
    if(SuppressPauseRequests(base) || !grid2vr::MatchesInstructions(allocation+0x593492,pauseMenu))return 3;
    if(!VirtualProtect(allocation,0x600000,PAGE_READWRITE,&previous))return 4;
    allocation[0x593537]^=1;
    std::array<uint8_t,0x1000> before{};
    std::memcpy(before.data(),allocation+0x593000,before.size());
    if(!VirtualProtect(allocation,0x600000,PAGE_EXECUTE_READ,&previous) || !SuppressPauseRequests(base))return 5;
    for(size_t i=0;i<before.size();++i) {
        const bool patched=(i>=0x492 && i<0x499) || (i>=0x537 && i<0x53e);
        if(allocation[0x593000+i]!=(patched?0x90:before[i]))return 6;
    }
    MEMORY_BASIC_INFORMATION region{};
    if(!VirtualQuery(allocation+0x593492,&region,sizeof(region)) || region.Protect!=PAGE_EXECUTE_READ)return 7;
    if(SuppressPauseRequests(base) || SuppressPauseRequests(0))return 8; // No foreign/already-patched code.
    for(unsigned state=0;state<8;++state) {
        if(WindowEligible(state,false)!=(state==5) || WindowEligible(state,true)!=(state==4 || state==5))return 9;
    }
    if(WindowEligible(~0u,true))return 10;
    VirtualFree(allocation,0,MEM_RELEASE);
    std::puts("Two guarded focus-pause sites only; other instructions and page protections preserved; hidden/minimized windows remain gated.");
    return 0;
}
