#pragma once
#include "guarded_hooks.h"
#include <array>
#include <cstring>
#include <filesystem>

namespace grid2vr::frontendvideo {
// Called only after the host executable fingerprint has been validated. The
// native image stays immutable; one owned event callback record is replaced.
bool Install(uintptr_t gameBase,const std::filesystem::path& output);
void Stop();
#ifdef GRID2VR_FRONTEND_VIDEO_TEST
using TestCallback=int (__thiscall*)(void*,void*);
void SetTestCallback(TestCallback callback);
bool TestEventGuards(uintptr_t gameBase);
#endif
constexpr uintptr_t resizeRva=0x784660,resetReturnRva=0x1b209a;

// The front-end video camera stores actual target dimensions at +408/+40c.
// Native reset passes them back to its size setter, which scales them again.
// Initial creation instead uses these four identical base-size table entries.
inline bool InstructionsMatch(uintptr_t base) {
    constexpr uint8_t entry[]{0x55,0x8b,0xec,0x51,0x53,0x56,0x8b,0xf1,0x57};
    constexpr uint8_t reset[]{0x8b,0x8e,0xb4,0x0f,0,0,0x8b,0x81,0x08,0x04,0,0,0x85,0xc0,0x74,0x13,
        0x8b,0x91,0x0c,0x04,0,0,0x85,0xd2,0x74,0x09,0x6a,0x01,0x52,0x50,0xe8,0xc6,0x25,0x5d,0};
    uint8_t initial[]{0x8b,0x56,0x0c,0x89,0x8a,0x10,0x04,0,0,0x8b,0x86,0x58,0x01,0,0,0x03,0xc0,0x03,0xc0,
        0x8b,0x94,0,0,0,0,0,0x03,0xc0,0x8b,0x80,0,0,0,0,0x0f,0xaf,0xc1,0x8b,0x4e,0x0c,0x6a,0,0x52,0x50,0xe8,0x36,0x87,0x41,0};
    const uint32_t tableHeight=static_cast<uint32_t>(base+0xceb21c),tableWidth=static_cast<uint32_t>(base+0xceb218);
    memcpy(initial+22,&tableHeight,4);memcpy(initial+30,&tableWidth,4);
    constexpr uint8_t scale[]{0x8b,0xc7,0x0f,0xaf,0x45,0x08,0x33,0xd2,0xf7,0xf1,0x33,0xd2,0x8b,0xd8,
        0x8b,0xc7,0x0f,0xaf,0x45,0x0c,0xf7,0xf1};
    constexpr uint8_t store[]{0x83,0xbe,0x10,0x04,0,0,0x01,0x8b,0x8e,0x94,0,0,0,0x89,0x9e,0x08,0x04,0,0,
        0x89,0xbe,0x0c,0x04,0,0};
    constexpr std::array<uint32_t,8> sizes{1024,512,1024,512,1024,512,1024,512};
    std::array<uint32_t,8> actual{};
    return base && MatchesInstructions(reinterpret_cast<void*>(base+resizeRva),entry) &&
        MatchesInstructions(reinterpret_cast<void*>(base+0x1b2077),reset) &&
        MatchesInstructions(reinterpret_cast<void*>(base+0x36bef9),initial) &&
        MatchesInstructions(reinterpret_cast<void*>(base+0x7846f2),scale) &&
        MatchesInstructions(reinterpret_cast<void*>(base+0x784729),store) &&
        ReadMemory(reinterpret_cast<void*>(base+0xceb218),actual.data(),sizeof(actual)) && actual==sizes;
}

// Only arguments change. The original callee still handles scaling, native
// resource invalidation, mip policy and UI shader dimensions. The caller must
// first establish the executable fingerprint, hook guards and render ownership.
inline bool ResetArguments(uintptr_t base,uintptr_t caller,const void* camera,unsigned& width,unsigned& height,bool force) {
    if (!base || caller!=base+resetReturnRva || !force || !camera) return false;
    std::array<uint32_t,0x420/4> object{};
    if (!ReadMemory(camera,object.data(),sizeof(object)) || object[0]!=base+0xcdd1f0 ||
        (object[0x404/4]&0xff00) || object[0x408/4]!=width || object[0x40c/4]!=height ||
        (object[0x410/4]!=1 && object[0x410/4]!=2)) return false;
    const auto count=object[0x410/4];
    // Every native height factor is 1/4, 1/2, 1, 2 or 4. Unknown dimensions
    // cannot authorize modifying a different camera's arguments.
    if (!height || width!=height*2*count ||
        (height!=128 && height!=256 && height!=512 && height!=1024 && height!=2048)) return false;
    std::array<uint32_t,0x90/4> target{};
    if (!ReadMemory(reinterpret_cast<void*>(object[0x94/4]),target.data(),sizeof(target)) ||
        target[0x3c/4]!=width || target[0x40/4]!=height || target[0x44/4]!=1 ||
        target[0x4c/4]!=1 || target[0x50/4]!=0x12 || target[0x68/4]!=0 ||
        !(target[0x30/4]&2) || !(target[0x8c/4]&8)) return false;
    width=1024*count;height=512;
    return true;
}
}
