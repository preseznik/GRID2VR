#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>

namespace grid2vr::opponentlod::runtime {
struct PointerChange {void** slot;void* expected;void* replacement;};
struct BusyRange {uintptr_t begin,end;}; // Absolute, half-open addresses.
// Synchronous, all-or-nothing expected-pointer transaction. The implementation
// must exclude this controller, suspend/recheck ALL peers, inspect their EIP and
// entire committed stack to TIB.StackBase (fail above 1 MiB), preflight all slots,
// validate, CAS with rollback, then committed BEFORE resuming peers. A nonnull
// return is a refusal/cleanup notice. The committed callback is authoritative:
// once called, ownership is published even if resume/close cleanup reports a
// notice afterward. Before commit, failures must roll back expected writes;
// rollback conflicts must be reported and storage retained. No weak fallback.
using TransactionFn=const char* (*)(std::span<const PointerChange>,std::span<const BusyRange>,
    bool (*validate)(void*) noexcept,void* context,void (*committed)(void*) noexcept) noexcept;

struct RuntimeReport {
    bool requested{},prepared{},ready{},worldReady{},stopped{},eyeReplay{},cleanupNotice{};
    uint32_t attempts{},attachments{},restorations{},callbackA{},callbackB{},updater{},
        originalRoutes{},privateRoutes{},batches{},floors{},refusals{},compareFailures{},
        activeWrappers{},controllerTicks{};
    // Indices: dispatch A, dispatch B, direct-updater root. Selection-pass
    // counters increment at the actual owned adapter entry, even on floor refusal.
    std::array<uint32_t,3> privateByRoute{},selectionPassesByRoute{};
    uintptr_t cache{},adapter{},manager{},updaterObject{};
    uintptr_t imageBase{};
    uint32_t peImageBase{};
    uint32_t integrityBodyCount{},busyRangeCount{};
    uint32_t updaterMethodCount{};
    bool singleAttemptDiagnostic{},diagnosticComplete{};
    bool controlledActivation{},attachmentAttempted{},controlledDormant{};
    uint32_t attachmentAttempts{},restoreAttempts{};
    uint32_t policyInspected{},policyNearby{},policyLastRefusal{};
    // Evaluate() statistics: plans are not confirmed writes; floors is the CAS
    // success count. Indices follow opponentlod::Result (guarded in the .cpp).
    std::array<uint32_t,9> policyResults{};
    // Per-reason floor refusals (order matches the receipt names). motionRefreshes
    // counts plans re-evaluated because only positions/camera moved between the
    // two captures; retainedPasses counts plans with nothing to raise.
    std::array<uint32_t,13> floorRefusals{};
    uint32_t motionRefreshes{},retainedPasses{};
    // Why a dispatch ran the original native route: eye replay, nested
    // wrapper, inactive VR world, owner/caller mismatch. Sums to originalRoutes.
    std::array<uint32_t,4> originalByReason{};
    // Confirmations that proceeded although ready / world was transiently clear
    // or another thread was replaying the eye pair (counted per observation).
    std::array<uint32_t,3> transientGates{};
    char reason[192]{};
    char floorReason[96]{};
};
// Host must independently approve the AVX executable fingerprint using its
// validated exact/trusted-normalized Steam fingerprint set before Prepare. This
// layer still requires ALL live PE/body/auxiliary/table guards using exact pinned
// HIGHLOW metadata. Fixtures use the native::ImageSha256 baseline, not an override.
// Prepare belongs at FIRST Present (not Attach), binding the controller thread.
// Desktop (any missing request file) is a no-op success. All controller calls,
// including Report, belong to that thread; SetEyeReplay is atomic-only.
bool Prepare(uintptr_t base,const std::filesystem::path& folder,TransactionFn) noexcept;
// worldReady is an affirmative HOST observation of an active runtime AND active
// XR session/projection AND the prepared single-center native world route. It is
// NOT just "game loaded" or existence of request files. Pass false on XR loss,
// shutdown, replay/eye-replay/unsupported phase. No native selection is added.
// Waiting for native objects is retryable. At most 16 attempts per observed graph,
// no more than one attempt every 30 controller ticks; Stop also retries boundedly.
void Tick(bool worldReady) noexcept;
// Immediate host revocation on XR loss/suspension/failure, including callbacks
// before another Present. Atomic-only and safe outside the controller thread.
void RevokeWorld() noexcept;
// Host brackets the entire ReplayOnce eye-pair scope with true/false, restoring
// false in an SEH-safe __finally. No Tick, transaction, logging or native work.
// Suppression applies to the calling (replaying) thread only.
void SetEyeReplay(bool active) noexcept;
void Stop() noexcept;
RuntimeReport Report() noexcept;
// Atomic-only query; after the diagnostic attempt no host-world reads are needed.
bool DiagnosticComplete() noexcept;
// Atomic-only; failed one-shot activation/ownership loss does not keep polling
// native readiness. Stop/restore servicing remains available on the controller.
bool WorldChecksNeeded() noexcept;

#ifdef GRID2VR_OPPONENT_LOD_RUNTIME_TEST
namespace testing {
// Only the native execution boundary/return-address provenance is substituted.
// Qualification, PE/body hashes, allocation, graph and floor checks stay real.
using NativeBoundary=uint32_t (*)(bool copied,bool updater,void* self,uint32_t argument) noexcept;
void Boundary(NativeBoundary) noexcept;
uint32_t Callback(uintptr_t nativeReturn,void* actualDispatchManager=nullptr) noexcept;
uint32_t Update(void* self,uint32_t argument) noexcept;
void Floor(uintptr_t adapterReturn,void* self,const void* views,uint32_t count,
    const void* list,uint32_t flag) noexcept;
// Runs between the floor's first and confirming captures, standing in for a
// concurrent game thread. nullptr disables it.
void BetweenCaptures(void (*hook)() noexcept) noexcept;
}
#endif
}
