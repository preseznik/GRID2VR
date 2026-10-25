#pragma once
#include <windows.h>
#include <cstdint>
#include <span>

namespace grid2vr {
bool ReadMemory(const void* address, void* destination, size_t bytes);
bool MatchesInstructions(const void* address, std::span<const uint8_t> expected);
// Swap only the expected pointer. Neither function's instructions are modified.
bool SwapGuardedPointer(void** slot, void* expected, void* replacement);
struct VtablePatch { size_t slot; void* expected; void* replacement; };
// Copies the table (including its preceding RTTI word), then changes one
// object's table pointer. The original table is never made writable.
// A successful shadow remains alive until the object no longer uses it.
void** AttachShadowVtable(void* object,void** expected,size_t entries,std::span<const VtablePatch> patches);
}
