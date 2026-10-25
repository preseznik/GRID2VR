#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace grid2vr {
inline constexpr uintptr_t visibilityEventOffset=0x4f4;
inline constexpr uintptr_t visibilityEventTableRva=0xcfeac0;
inline constexpr std::array<uintptr_t,7> visibilityEventMethods{
    0x1f42f0,0x2c2fc0,0x45f860,0x1ea300,0x2d0900,0x72d3c0,0x3701c0};

// The scheduler saves EBP/EBX/ESI/EDI, then calls its embedded event's Reset
// without arguments. Its caller supplied main+160 as the job. Validate both
// return addresses; unrelated event resets must only forward to the original.
inline bool VisibilityDispatchStack(std::span<const uintptr_t> stack,uintptr_t base,uintptr_t main) {
    return base && main && stack.size()>=7 && stack[0]==base+0x78d778 &&
        stack[2]==main && stack[5]==base+0x117711 && stack[6]==main+0x160;
}

template<class Read> bool VisibilityDispatchCode(uintptr_t base,Read read) {
    const auto matches=[&](uintptr_t rva,std::span<const uint8_t> expected) {
        std::array<uint8_t,160> actual{};
        return expected.size()<=actual.size() && read(base+rva,actual.data(),expected.size()) &&
            !memcmp(actual.data(),expected.data(),expected.size());
    };
    // Both branches reach the same dispatch only after the native volume has
    // been built. The alternate branch skips the private visibility consumer.
    constexpr uint8_t branch[]{0x80,0xbe,0x9d,0,0,0,0,0x0f,0x85,0x86,0,0,0};
    constexpr uint8_t submit[]{0x80,0xbe,0x38,0x01,0,0,0,0x74,0x20,0x8b,0x4e,0x24,
        0x8d,0x86,0x60,0x01,0,0,0x50,0xe8,0x3f,0x60,0x67,0};
    constexpr uint8_t dispatch[]{0x55,0x8b,0xec,0x53,0x8b,0x59,0x08,0x56,0x8b,0x75,0x08,
        0x57,0x8d,0x8e,0x64,0x03,0,0,0xe8,0x19,0x4f,0xc0,0xff,
        0x8b,0x86,0x94,0x03,0,0,0x8b,0x50,0x18,0x8d,0x8e,0x94,0x03,0,0,0xff,0xd2};
    constexpr uint8_t enqueue[]{0x56,0x8b,0xcb,0xe8,0x66,0x5f,0x9d,0xff,
        0x8d,0x8e,0x64,0x03,0,0,0xe8,0x6b,0xb1,0xeb,0xff,0x5f,0x5e,0x5b,0x5d,0xc2,0x04,0};
    uint8_t pending[]{0x80,0x3d,0,0,0,0,0,0xc6,0x86,0x9c,0x03,0,0,1,0x75,0x0a,
        0xc7,0x86,0xc4,0x02,0,0,0,0,0,0};
    const uint32_t pendingFlag=static_cast<uint32_t>(base+0xdad878);
    memcpy(pending+2,&pendingFlag,4);
    uint8_t reset[]{0x8b,0x41,0x04,0x50,0xff,0x15,0,0,0,0,0x85,0xc0,0x75,0x1e,
        0xff,0x15,0,0,0,0,0x85,0xc0,0x74,0x14,0x83,0xf8,0x06,0x74,0x07,
        0x50,0xe8,0xed,0x59,0xd9,0xff,0xc3,0x6a,0x03,0xe8,0xf5,0xdf,0xdf,0xff,0xc3,0x33,0xc0,0xc3};
    const uint32_t resetImport=static_cast<uint32_t>(base+0xb16270);
    const uint32_t errorImport=static_cast<uint32_t>(base+0xb16190);
    memcpy(reset+6,&resetImport,4);
    memcpy(reset+16,&errorImport,4);
    std::array<uintptr_t,7> methods{};
    if (!matches(0x117666,branch) || !matches(0x1176f9,submit) ||
        !matches(0x78d750,dispatch) || !matches(0x78d778,pending) ||
        !matches(0x78d792,enqueue) || !matches(0x3701c0,reset) ||
        !read(base+visibilityEventTableRva,methods.data(),sizeof(methods))) return false;
    for(size_t i=0;i<methods.size();++i)if(methods[i]!=base+visibilityEventMethods[i])return false;
    return true;
}
}
