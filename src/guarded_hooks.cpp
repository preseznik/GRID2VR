#include "guarded_hooks.h"
#include "replay_profile.h"
#include <array>
#include <cstring>

namespace grid2vr {
bool ReadMemory(const void* address, void* destination, size_t bytes) {
    profile::Scope measure(profile::MemoryRead);
    SIZE_T read{};
    return address && ReadProcessMemory(GetCurrentProcess(),address,destination,bytes,&read) && read==bytes;
}

bool MatchesInstructions(const void* address, std::span<const uint8_t> expected) {
    std::array<uint8_t,64> actual{};
    return !expected.empty() && expected.size()<=actual.size() &&
        ReadMemory(address,actual.data(),expected.size()) && !memcmp(actual.data(),expected.data(),expected.size());
}

bool SwapGuardedPointer(void** slot,void* expected,void* replacement) {
    void* current{};
    if (reinterpret_cast<uintptr_t>(slot)%alignof(void*) || !ReadMemory(slot,&current,sizeof(current)) || current!=expected) return false;
    DWORD protection{};
    if (!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&protection)) return false;
    const bool swapped=InterlockedCompareExchangePointer(slot,replacement,expected)==expected;
    DWORD ignored{};
    if (!VirtualProtect(slot,sizeof(void*),protection,&ignored)) {
        if (swapped) InterlockedCompareExchangePointer(slot,expected,replacement);
        return false;
    }
    return swapped;
}

void** AttachShadowVtable(void* object,void** expected,size_t entries,std::span<const VtablePatch> patches) {
    void* current{};
    if (!object || !expected || !entries || entries>256 || patches.empty() ||
        !ReadMemory(object,&current,sizeof(current)) || current!=expected) return nullptr;
    const auto bytes=(entries+1)*sizeof(void*);
    auto** allocation=static_cast<void**>(VirtualAlloc(nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if (!allocation) return nullptr;
    auto** shadow=allocation+1;
    bool valid=ReadMemory(expected-1,allocation,bytes);
    for (const auto& patch:patches) {
        if (!valid || patch.slot>=entries || shadow[patch.slot]!=patch.expected || !patch.replacement) { valid=false; break; }
        shadow[patch.slot]=patch.replacement;
    }
    DWORD previous{};
    if (valid && VirtualProtect(allocation,bytes,PAGE_READONLY,&previous) &&
        SwapGuardedPointer(static_cast<void**>(object),expected,shadow)) return shadow;
    VirtualFree(allocation,0,MEM_RELEASE);
    return nullptr;
}
}
