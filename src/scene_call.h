#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace grid2vr {
// Supported AVX scene prologue saves EBX/ESI/EDI and has no local allocation.
// At its slot 5/6 call: return, three callback args, three saved registers,
// saved EBP, wrapper return, then the six complete-scene arguments.
struct SceneCall { std::array<uintptr_t,6> args{}; };
using CompleteSceneFn=void (__thiscall*)(void*,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
inline void InvokeCompleteScene(CompleteSceneFn function,void* self,const SceneCall& call) {
    function(self,call.args[0],call.args[1],call.args[2],call.args[3],call.args[4],call.args[5]);
}
inline bool DecodeSceneCall(std::span<const uintptr_t> stack,uintptr_t base,unsigned slot,
                            uintptr_t context,uintptr_t view,uintptr_t packet,SceneCall& call) {
    if (stack.size()<15 || (slot!=5 && slot!=6) || !context || !view || !packet ||
        stack[0]!=base+(slot==5?0x3be09e:0x3be1b8) || stack[8]!=base+0x7a7cd2 ||
        stack[1]!=context || stack[2]!=view || stack[3]!=packet ||
        stack[9]!=packet || stack[11]!=view || stack[12]!=context || (stack[14]&0xff)>1) return false;
    // The wrapper initializes only the low byte of its boolean stack word.
    // Preserve padding bits; the scene reads byte [ebp+0x1c].
    for (size_t i=0;i<6;++i) call.args[i]=stack[i+9];
    return true;
}
}
