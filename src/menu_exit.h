#pragma once
#include <cstdint>
#include <filesystem>

namespace grid2vr::menuexit {
// Requires the supported executable fingerprint and the process-exit handoff.
// Yield the two native terminal race states until render-owner cleanup finishes.
bool Install(uintptr_t base,const std::filesystem::path& output);
}
