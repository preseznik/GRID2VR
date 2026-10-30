#pragma once
#include <atomic>
#include <cstdint>

namespace grid2vr {
// Direct Practice/Race launches run as -benchmarkAttract: mode 2 in the word at
// image+0xe5917c. The flashback module clears that word only while the local car
// is racing, and registers the exact image here first, so readiness checks keep
// recognising the direct launch. A Normal launch (0, no registration) never matches.
inline constexpr uintptr_t benchmarkModeWordRva=0xe5917c;
inline std::atomic<uintptr_t> benchmarkModeClearedImage{};
static_assert(std::atomic<uintptr_t>::is_always_lock_free);
inline bool IsBenchmarkAttractMode(uintptr_t image,uint32_t value) {
    return value==2 || (value==0 && image && benchmarkModeClearedImage.load()==image);
}
}
