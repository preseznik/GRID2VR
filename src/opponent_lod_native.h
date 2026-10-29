#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace grid2vr::opponentlod::native {
inline constexpr std::size_t BodyCount=5,AdapterBytes=24,StagingBytes=0x1e28,AllocationBytes=8192;
inline constexpr uint32_t PreferredImageBase=0x400000,ApplyRva=0x6a260;
inline constexpr std::string_view ImageSha256="519cd994d2ff92a2a7a2a784856854183f53e359b83f8b9563cc09a0ae91e502";
struct Call {uint32_t offset,targetRva;};
struct BodyGuard {
    std::string_view name;
    uint32_t rva,size;
    std::string_view sha256;
    std::span<const uint32_t> highlow;
    std::span<const Call> calls;
};
using BodyInputs=std::array<std::span<const uint8_t>,BodyCount>;
enum class Failure {
    None,NotQualified,InvalidAddress,InputSize,Capacity,AliasedInput,
    InvalidManifest,CallOperand,BodyHash,HashUnavailable
};
struct StageResult {
    Failure failure{Failure::NotQualified};
    std::array<uint32_t,BodyCount> copies{};
    uint32_t adapter{};
    std::size_t used{};
    explicit operator bool() const noexcept {return failure==Failure::None;}
};

// Offline construction ONLY. No process reads, allocation, executable protection,
// pointer publication, car writes, or native execution. Addresses are symbolic
// x86 addresses; input spans contain live-base HIGHLOW operands. The destination
// stays unchanged on refusal. Success does not qualify any attachment route or
// establish whole-executable identity (the caller must verify that separately).
std::span<const BodyGuard,BodyCount> Guards() noexcept;
StageResult StageCopies(const BodyInputs& inputs,uint32_t imageBase,uint32_t copyBase,
    uint32_t floorHelper,std::span<uint8_t> destination) noexcept;

// Emits the audited EBX-frame fastcall bridge. Helper MUST return normally,
// preserve x86 nonvolatile registers/FPU/MXCSR and pop exactly 16 argument bytes.
// Its sole permitted future effect is a validated floor of record.selected.
// The tail jump preserves exactly one original apply, including on refusal.
Failure BuildAdapter(uint32_t address,uint32_t floorHelper,uint32_t apply,
    std::span<uint8_t> destination) noexcept;

// Intentionally has no opt-in switch. Lifecycle/phase/fallback gates are open.
Failure Install() noexcept;
inline constexpr std::string_view InstallationBlocker=
    "unqualified_manager_car_updater_lifetime_complete_peer_quiescence_and_native_phase";

#ifdef GRID2VR_OPPONENT_LOD_NATIVE_TEST
namespace testing {
// Same construction path, with test-owned full-body guards. Never compiled into
// the game DLL; permits synthetic relocation tests without executing game code.
StageResult StageCopies(std::span<const BodyGuard,BodyCount> guards,const BodyInputs& inputs,
    uint32_t imageBase,uint32_t copyBase,uint32_t floorHelper,std::span<uint8_t> destination) noexcept;
}
#endif
}
