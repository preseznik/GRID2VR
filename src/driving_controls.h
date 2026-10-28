#pragma once
#include <cstdint>
#include <filesystem>
namespace grid2vr::drivingcontrols {
// Call after the owner verifies the exact AVX executable fingerprint. This
// prepares private Input2/driver tables without changing any game image bytes
// or saved bindings. Driver attachment waits for a proven live local controller.
// It does not require a race/world: native menu input is supported too.
void Install(uintptr_t gameBase,const std::filesystem::path& output);
// Present-thread discovery/reattachment; bounded scan slices, only while no
// validated instance is attached. All polling runs in native Input2 Update.
void Tick();
// Actual process shutdown only: suspending VR must not change driving controls.
void Stop();
#if defined(GRID2VR_DRIVING_CONTROLS_TEST)
// Owned callbacks only, compiled into the offline image probe, never the DLL.
namespace testing { void DriverUpdate(void* object,void* nativeUpdate); }
#endif
}
