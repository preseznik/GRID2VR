#pragma once
#include <cstdint>
#include <filesystem>

namespace grid2vr::ffbobserver {
// Observe-only: count the game's XInput rumble and DirectInput force-feedback
// calls and results. Every call is forwarded unchanged; nothing is suppressed.
// Called only after the supported-host fingerprint check.
void Install(uintptr_t base,const std::filesystem::path& output);
// Bounded status refresh from the controller thread (Present).
void Tick();
void Stop();
}
