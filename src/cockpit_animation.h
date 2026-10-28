#pragma once
#include <cstdint>
#include <filesystem>

namespace grid2vr::cockpitanimation {
constexpr uintptr_t qualityRva=0xe30a38,settingsRva=0x6c7300;
// Read-only qualification after executable validation. No native code hook or
// startup quality write; Tick waits for a completed world frame.
bool Install(uintptr_t base,const std::filesystem::path& folder) noexcept;
void Tick(bool worldReady) noexcept;
void Stop() noexcept;
bool InstructionsMatch(uintptr_t base);

// Owns only the native character-quality value, never the user's settings tree.
// Poll only at the existing restored Present boundary. Native per-frame mode
// propagation consumes the value on the following update.
class QualityOverride {
public:
    bool Bind(uintptr_t base);
    bool Refresh(bool worldReady);
    // Polling cannot distinguish our 1 from a later user choice of Medium (1).
    // Release therefore leaves the current native value untouched.
    void Release() {owned=false;}
    int Requested() const {return requested;}
    uint64_t Changes() const {return changes;}
    uint64_t Refusals() const {return refusals;}
    bool Active() const {return owned;}
private:
    uintptr_t base{};
    int requested{-1};
    uint64_t changes{},refusals{};
    bool owned{};
};
}
