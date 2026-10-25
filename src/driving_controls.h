#pragma once
#include <cstdint>
#include <filesystem>
namespace grid2vr::drivingcontrols {
// Call after the owner verifies the exact AVX executable fingerprint. This
// prepares a private Input2 table without changing game code or saved bindings.
// It does not require a race/world: native menu input is supported too.
void Install(uintptr_t gameBase,const std::filesystem::path& output);
// Present-thread discovery/reattachment; bounded scan slices, only while no
// validated instance is attached. All polling runs in native Input2 Update.
void Tick();
// Actual process shutdown only: suspending VR must not change driving controls.
void Stop();
}
