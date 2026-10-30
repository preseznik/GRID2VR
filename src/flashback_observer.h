#pragma once
#include <cstdint>
#include <filesystem>

namespace grid2vr::flashbackobserver {
// Direct-launch flashback: sample the local car's FlashbackMonitor and the
// gate's plain-data inputs, and clear the -benchmarkAttract mode word only
// while the local car is racing (BenchmarkFlagPolicy). No game code is hooked
// or patched (the game hashes its code). Called only after the supported-host
// fingerprint check; every read and the one write are instruction guarded.
void Install(uintptr_t base,const std::filesystem::path& output);
// Per-frame sampling, flag window and bounded status refresh from the controller thread (Present).
void Tick();
// Restores the launch benchmark mode if it is cleared.
void Stop();
}
