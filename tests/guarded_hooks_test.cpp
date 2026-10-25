#include "guarded_hooks.h"
#include "scene_call.h"
#include <array>
#include <cstdio>
#include <cstring>

using Function=void (__thiscall*)(void*,uintptr_t);
Function original{};
unsigned detours{};
using SceneFunction=void (__thiscall*)(void*,uintptr_t,uintptr_t,uintptr_t);
SceneFunction originalScene{};
unsigned sceneDetours{};
void __fastcall SceneDetour(void* self,void*,uintptr_t a,uintptr_t b,uintptr_t c) {
    ++sceneDetours; originalScene(self,a,b,c);
}
struct ShadowObject { void** table; unsigned calls{}; uintptr_t value{}; };
void __fastcall ShadowOriginal(ShadowObject* self,void*,uintptr_t value) { ++self->calls; self->value=value; }
void __fastcall ShadowDetour(ShadowObject* self,void*,uintptr_t value) { ShadowOriginal(self,nullptr,value+1); }
bool TestShadow() {
    auto** allocation=static_cast<void**>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if (!allocation) return false;
    allocation[0]=reinterpret_cast<void*>(0x12345678); // RTTI-prefix sentinel.
    allocation[1]=allocation[2]=reinterpret_cast<void*>(ShadowOriginal);
    const std::array<void*,3> before{allocation[0],allocation[1],allocation[2]};
    DWORD previous{};
    if (!VirtualProtect(allocation,4096,PAGE_READONLY,&previous)) return false;
    ShadowObject first{allocation+1},second{allocation+1};
    grid2vr::VtablePatch patch{0,reinterpret_cast<void*>(ShadowDetour),reinterpret_cast<void*>(ShadowDetour)};
    if (grid2vr::AttachShadowVtable(&first,allocation+1,2,{&patch,1}) || first.table!=allocation+1) return false;
    patch.expected=reinterpret_cast<void*>(ShadowOriginal);
    auto** shadow=grid2vr::AttachShadowVtable(&first,allocation+1,2,{&patch,1});
    if (!shadow || first.table!=shadow || second.table!=allocation+1 || shadow[-1]!=allocation[0] || shadow[1]!=allocation[2]) return false;
    using Method=void (__thiscall*)(ShadowObject*,uintptr_t);
    reinterpret_cast<Method>(first.table[0])(&first,40);
    reinterpret_cast<Method>(second.table[0])(&second,40);
    if (first.calls!=1 || first.value!=41 || second.calls!=1 || second.value!=40 || memcmp(before.data(),allocation,sizeof(before))) return false;
    MEMORY_BASIC_INFORMATION region{}; VirtualQuery(allocation,&region,sizeof(region));
    if (region.Protect!=PAGE_READONLY || !grid2vr::SwapGuardedPointer(reinterpret_cast<void**>(&first),shadow,allocation+1)) return false;
    VirtualFree(shadow-1,0,MEM_RELEASE); VirtualFree(allocation,0,MEM_RELEASE);
    return true;
}
void __fastcall Detour(void* self,void*,uintptr_t value) {
    ++detours; original(self,value);
}
int main() {
    // Synthetic thiscall: increment self[0], store its argument, ret 4.
    const uint8_t code[]{0x55,0x8b,0xec,0xff,0x01,0x8b,0x45,0x08,0x89,0x41,0x04,0x5d,0xc2,0x04,0x00};
    auto* function=VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    auto** slot=static_cast<void**>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if (!function || !slot) return 1;
    memcpy(function,code,sizeof(code)); *slot=function;
    DWORD previous{};
    if (!VirtualProtect(function,4096,PAGE_EXECUTE_READ,&previous) || !VirtualProtect(slot,4096,PAGE_READONLY,&previous)) return 2;
    FlushInstructionCache(GetCurrentProcess(),function,sizeof(code));
    if (!grid2vr::MatchesInstructions(function,code) || grid2vr::MatchesInstructions(reinterpret_cast<void*>(1),code)) return 3;
    const uint8_t wrong[]{0xcc,0xcc};
    if (grid2vr::MatchesInstructions(function,wrong)) return 4;
    auto* detour=reinterpret_cast<void*>(Detour);
    if (grid2vr::SwapGuardedPointer(slot,detour,function) || *slot!=function) return 5;
    original=reinterpret_cast<Function>(function);
    if (!grid2vr::SwapGuardedPointer(slot,function,detour)) return 6;
    std::array<uintptr_t,2> object{};
    for (unsigned i=0;i<3;++i) {
        reinterpret_cast<Function>(*slot)(object.data(),42+i);
        if (object[1]!=42+i) return 7;
    }
    if (object[0]!=3 || detours!=3 || !grid2vr::MatchesInstructions(function,code)) return 8;
    MEMORY_BASIC_INFORMATION region{}; VirtualQuery(slot,&region,sizeof(region));
    if (region.Protect!=PAGE_READONLY) return 9;
    if (grid2vr::SwapGuardedPointer(slot,function,detour)) return 10;
    if (!grid2vr::SwapGuardedPointer(slot,detour,function)) return 11;
    // Main scene callbacks use three stack arguments and callee cleanup (ret 12).
    const uint8_t sceneCode[]{0x55,0x8b,0xec,0xff,0x01,
        0x8b,0x45,0x08,0x89,0x41,0x04,0x8b,0x45,0x0c,0x89,0x41,0x08,
        0x8b,0x45,0x10,0x89,0x41,0x0c,0x5d,0xc2,0x0c,0x00};
    if (!VirtualProtect(function,4096,PAGE_READWRITE,&previous)) return 12;
    memcpy(function,sceneCode,sizeof(sceneCode));
    if (!VirtualProtect(function,4096,PAGE_EXECUTE_READ,&previous)) return 13;
    FlushInstructionCache(GetCurrentProcess(),function,sizeof(sceneCode));
    originalScene=reinterpret_cast<SceneFunction>(function);
    auto* sceneDetour=reinterpret_cast<void*>(SceneDetour);
    if (!grid2vr::SwapGuardedPointer(slot,function,sceneDetour)) return 14;
    std::array<uintptr_t,4> sceneObject{};
    for (unsigned i=0;i<1000;++i) {
        reinterpret_cast<SceneFunction>(*slot)(sceneObject.data(),i,0xaabbccdd,0x10203040);
        if (sceneObject!=std::array<uintptr_t,4>{i+1,i,0xaabbccdd,0x10203040}) return 15;
    }
    if (sceneDetours!=1000 || !grid2vr::MatchesInstructions(function,sceneCode)) return 16;
    if (!grid2vr::SwapGuardedPointer(slot,sceneDetour,function)) return 17;
    // Complete scene uses six full stack words (including boolean padding), ret 24.
    const uint8_t completeCode[]{0x55,0x8b,0xec,0xff,0x01,
        0x8b,0x45,0x08,0x89,0x41,0x04,0x8b,0x45,0x0c,0x89,0x41,0x08,
        0x8b,0x45,0x10,0x89,0x41,0x0c,0x8b,0x45,0x14,0x89,0x41,0x10,
        0x8b,0x45,0x18,0x89,0x41,0x14,0x8b,0x45,0x1c,0x89,0x41,0x18,
        0x5d,0xc2,0x18,0x00};
    if (!VirtualProtect(function,4096,PAGE_READWRITE,&previous)) return 19;
    memcpy(function,completeCode,sizeof(completeCode));
    if (!VirtualProtect(function,4096,PAGE_EXECUTE_READ,&previous)) return 20;
    FlushInstructionCache(GetCurrentProcess(),function,sizeof(completeCode));
    std::array<uintptr_t,7> completeObject{};
    for (unsigned i=0;i<1000;++i) {
        grid2vr::SceneCall call{{i,0x11223344,0x55667788,0x99aabbcc,0x10203040,0xaabbcc01}};
        grid2vr::InvokeCompleteScene(reinterpret_cast<grid2vr::CompleteSceneFn>(function),completeObject.data(),call);
        if (completeObject[0]!=i+1 || memcmp(completeObject.data()+1,call.args.data(),sizeof(call.args))) return 21;
    }
    VirtualFree(slot,0,MEM_RELEASE); VirtualFree(function,0,MEM_RELEASE);
    if (!TestShadow()) return 18;
    puts("Pointer/instruction guards, unchanged code, protection and one/three/six-argument thiscall forwarding passed.");
}
