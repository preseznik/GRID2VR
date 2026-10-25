#pragma once
#include <cstddef>
#include <cstdint>

namespace grid2vr::profilesetup {
enum class Kind : std::uint32_t { ProfileChanged, WaitForSave, BeginSave };
struct Snapshot {
    std::uint32_t readable{}, signedOut{}, operation{}, operationResult{}, status{}, statusMethodRva{};
    std::uint32_t mapEntries{}, mapMatch{}, operationKnown{}, resultKnown{}, statusKnown{};
    std::uint32_t resolverStage{}, context{}, managers{}, managerIndex{}, manager{}, type{}, mapBegin{}, mapEnd{};
    std::uint32_t beginKnown{}, beginReady{}, beginForced{}, beginOptions{}, beginExisting{}, profileKnown{}, profileIndex{}, profile{}, serviceKnown{}, serviceReady{};
    bool operator==(const Snapshot&) const = default;
};
struct Observation {
    Kind kind{};
    std::uint32_t state{}, output{}, outputKnown{}, returnedOutput{}, next{}, nextKnown{}, outputCode{};
    Snapshot before{}, after{};
    bool operator==(const Observation&) const = default;
};
using Step = std::uint32_t* (__thiscall*)(void*, std::uint32_t*);
bool ValidLayout(const std::uint8_t* image, std::size_t bytes, std::uintptr_t base);
Snapshot Inspect(Kind kind, std::uintptr_t base, std::uintptr_t state);
std::uint32_t* Observe(Kind kind, std::uintptr_t base, void* state, std::uint32_t* output,
    Step original, Observation& observation);
// Called after the storage provider has verified the complete AVX executable
// hash, account binding and profile directory. Observation never alters results.
bool Install(std::uint8_t* image, std::size_t bytes, const wchar_t* outputFolder, const char* profileId);
}
