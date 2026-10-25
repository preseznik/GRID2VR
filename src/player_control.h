#pragma once
#include <cstdint>
#include <filesystem>
namespace grid2vr::playercontrol {
// Opt-in, exact-host probe. Initialization follows the host hash and MinHook setup.
void Install(uintptr_t base,const std::filesystem::path& output);
void Stop();
}
