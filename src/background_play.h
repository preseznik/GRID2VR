#pragma once
#include <cstdint>
#include <filesystem>

namespace grid2vr::backgroundplay {
// Called only after the supported-host fingerprint check, during device startup.
void Install(uintptr_t base,const std::filesystem::path& output);
bool Enabled();
void ObserveWindow(unsigned state);
void Stop();
constexpr bool WindowEligible(unsigned state,bool background) {
    return state==5 || (background && state==4); // visible, not minimized
}
}
