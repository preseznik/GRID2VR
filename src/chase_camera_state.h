#pragma once
#include "guarded_hooks.h"
#include <array>
namespace grid2vr::chasecamera {
struct PauseGuard {uintptr_t rva;std::array<uint8_t,64> bytes;size_t size;};
inline auto PauseGuards(uintptr_t base) {
    std::array<PauseGuard,12> result{{
        {0x7aea66,{0x8b,0x43,0x08,0x83,0xec,0x48,0x56,0x8b,0xf1,0x8a,0x8e,0xf6,0x02,0x00,0x00,0x57,0x8b,0x78,0x08,0x8a,0x40,0x04,0x84,0xc0,0x88,0x86,0xf6,0x02,0x00,0x00},30},
        {0x27e53f,{0xc7,0x86,0xf4,0x02,0x00,0x00,0x00,0x01,0x00,0x00},10},
        {0x8bd5c,{0xa1,0x14,0x80,0x19,0x01,0x80,0xb8,0xf6,0x02,0x00,0x00,0x00},12},
        {0x8bd61,{0x80,0xb8,0xf6,0x02,0x00,0x00,0x00},7},
        {0x716cf6,{0xa1,0x14,0x80,0x19,0x01,0x83,0xec,0x38,0x80,0xb8,0xf6,0x02,0x00,0x00,0x00},15},
        {0x27e423,{0xc7,0x06,0x10,0xaf,0x0c,0x01},6},
        {0x6b506a,{0xc7,0x45,0xe4,0x38,0x89,0x1e,0x01,0xc7,0x45,0xe8,0x50,0xea,0xba,0x00,0x89,0x7d,0xec,0xe8,0x80,0x3a,0xf8,0xff},22},
        {0x7b50f0,{0xb8,0x38,0x89,0x1e,0x01,0xc3},6},
        {0x775d0,{0xb8,0x34,0x7e,0x05,0x01,0xc3},6},
        {0xb1704,{0xb8,0x01,0x00,0x00,0x00,0x88,0x45,0x84,0x89,0x45,0x88,0x8d,0x45,0x80,0xc7,0x45,0x80,0xd4,0xcf,0x0c,0x01},21},
        {0xb1735,{0xc7,0x45,0x80,0xd4,0xcf,0x0c,0x01,0xc6,0x45,0x84,0x01,0xc7,0x45,0x88,0x02,0x00,0x00,0x00},18},
        {0x6373b2,{0x33,0xc0,0x88,0x45,0xa4,0x89,0x45,0xa8,0x8d,0x45,0xa0,0xc7,0x45,0xa0,0xd4,0xcf,0x0c,0x01},18},
    }};
    {const auto value=static_cast<uint32_t>(base+0xd98014);for(unsigned byte=0;byte<4;++byte)result[2].bytes[1+byte]=static_cast<uint8_t>(value>>(8*byte));}
    {const auto value=static_cast<uint32_t>(base+0xd98014);for(unsigned byte=0;byte<4;++byte)result[4].bytes[1+byte]=static_cast<uint8_t>(value>>(8*byte));}
    {const auto value=static_cast<uint32_t>(base+0xccaf10);for(unsigned byte=0;byte<4;++byte)result[5].bytes[2+byte]=static_cast<uint8_t>(value>>(8*byte));}
    {const auto value=static_cast<uint32_t>(base+0xde8938);for(unsigned byte=0;byte<4;++byte)result[6].bytes[3+byte]=static_cast<uint8_t>(value>>(8*byte));}
    {const auto value=static_cast<uint32_t>(base+0x7aea50);for(unsigned byte=0;byte<4;++byte)result[6].bytes[10+byte]=static_cast<uint8_t>(value>>(8*byte));}
    {const auto value=static_cast<uint32_t>(base+0xde8938);for(unsigned byte=0;byte<4;++byte)result[7].bytes[1+byte]=static_cast<uint8_t>(value>>(8*byte));}
    {const auto value=static_cast<uint32_t>(base+0xc57e34);for(unsigned byte=0;byte<4;++byte)result[8].bytes[1+byte]=static_cast<uint8_t>(value>>(8*byte));}
    {const auto value=static_cast<uint32_t>(base+0xcccfd4);for(unsigned byte=0;byte<4;++byte)result[9].bytes[17+byte]=static_cast<uint8_t>(value>>(8*byte));}
    {const auto value=static_cast<uint32_t>(base+0xcccfd4);for(unsigned byte=0;byte<4;++byte)result[10].bytes[3+byte]=static_cast<uint8_t>(value>>(8*byte));}
    {const auto value=static_cast<uint32_t>(base+0xcccfd4);for(unsigned byte=0;byte<4;++byte)result[11].bytes[14+byte]=static_cast<uint8_t>(value>>(8*byte));}
    return result;
}
inline bool PauseGuardsMatch(uintptr_t base) {
    for(const auto& guard:PauseGuards(base))if(!MatchesInstructions(reinterpret_cast<void*>(base+guard.rva),std::span<const uint8_t>(guard.bytes).first(guard.size)))return false;
    return true;
}
// RenderPauseWorld is a sustained read-only exclusion. A clear flag is not
// evidence of driving: the caller must still qualify the local race and camera.
// A separately qualified replay may pause the recorded world; its director
// allowlist must independently exclude manual menu pause before allowing this.
template<class Read> bool WorldRunning(uintptr_t base,Read&& read,bool qualifiedReplay=false) {
    uint32_t owner{},table{},again{};uint8_t pause{},repeat{};
    return read(base+0xd98014,&owner,4) && owner>=0x10000 && owner<=UINT32_MAX-0x10000 && owner%4==0 &&
        read(owner,&table,4) && table==base+0xccaf10 && read(owner+0x2f6,&pause,1) && pause<=(qualifiedReplay?1:0) &&
        read(base+0xd98014,&again,4) && again==owner && read(owner,&again,4) && again==table &&
        read(owner+0x2f6,&repeat,1) && repeat==pause;
}
}
