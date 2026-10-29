#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <span>

namespace grid2vr::quiescence {
inline constexpr std::size_t MaxWrites=8,MaxBusyRanges=64,MaxThreads=1024;
inline constexpr std::size_t MaxStackBytes=1024*1024;
struct BusyRange {uintptr_t begin,end;}; // Absolute, half-open addresses, NOT RVAs.
struct PointerWrite {void** slot;void* expected;void* desired;};
using Validator=bool (*)(void*) noexcept;
using Publisher=void (*)(void*) noexcept;
struct Request {
    bool enabled{};
    std::span<const PointerWrite> writes;
    std::span<const BusyRange> busyRanges;
    Validator validate{};
    Publisher committed{};
    void* context{};
    // Run every guard/preflight/recheck, but never CAS or publish ownership.
    bool probeOnly{};
};
enum class Failure {
    None,Disabled,InvalidRequest,ConcurrentTransaction,Allocation,NtUnavailable,
    ThreadSnapshot,ThreadEnumeration,ThreadLayout,ThreadLimit,ThreadSetChanged,
    ThreadOpen,ThreadIdentity,ThreadTeb,ThreadSuspend,ThreadContext,
    BusyInstruction,StackBounds,StackMemory,BusyStack,ValidationRefused,
    SlotMemory,SlotValue,CompareExchange
};
struct Result {
    Failure failure{Failure::None};
    // Commit and cleanup are independent: NEVER free a published allocation
    // just because cleanup failed. Likewise a rollback conflict retains ownership
    // uncertainty even when a foreign actor has replaced the slot again.
    bool committed{},layoutValidated{},probePassed{};
    uint32_t writtenMask{},rolledBackMask{},rollbackConflictMask{};
    uint32_t resumeFailures{},closeFailures{};
    DWORD threadId{},win32Error{};
    LONG ntStatus{};
    std::size_t index{},peerCount{},suspendedCount{},stackBytesScanned{};
    // First busy witness only; never retain or serialize full peer stacks.
    uintptr_t instruction{},stackPointer{},busyValue{},busyStackAddress{},busyRangeBegin{},busyRangeEnd{};
    std::size_t busyRangeIndex{MaxBusyRanges};
    // Pause scope bounds first SuspendThread through cleanup/resume/handle close;
    // it is an upper bound, not a per-thread exact suspension measurement.
    uint64_t totalMicroseconds{},pauseScopeMicroseconds{};
    bool timingAvailable{},pauseScopeMeasured{};
    explicit operator bool() const noexcept {
        return committed && failure==Failure::None && !resumeFailures && !closeFailures;
    }
};

// Win32/MSVC x86 only. No code/image writes or protection changes. All slots
// must be distinct aligned writable, non-executable MEM_PRIVATE pointers.
// All request memory/slots/objects/code must remain alive until return. Caller
// supplies ALL original lifetime/dispatch and private executable busy ranges;
// this helper cannot qualify game fingerprints or infer missing ranges.
//
// Validator and publisher run with peers suspended: NO allocation, logging,
// loader/CRT locks, waits, exceptions, or calls into arbitrary game code. The
// publisher must use lock-free stores only; it runs once AFTER every CAS succeeds
// and BEFORE peers resume. Validator is mandatory, busyRanges must be nonempty.
//
// Success is atomic relative to the suspended peer set, not remote injection,
// kernel writers or a concurrent external debugger. Caller is excluded. Peers
// are enumerated again after suspension and immediately before writes; unknown
// NT layout, set changes and any incomplete/oversized stack cause refusal.
// A failed CAS conditionally rolls back only desired->expected (foreign values
// are never overwritten). Restore uses the identical primitive with each
// expected/desired reversed. Result::committed is authoritative for ownership.
// probeOnly passes the identical guards but returns before ANY write/publication;
// inspect probePassed explicitly, never treat it as committed ownership.
Result Transact(const Request& request) noexcept;
const char* FailureName(Failure failure) noexcept;

#ifdef GRID2VR_QUIESCED_HEAP_TRANSACTION_TEST
namespace testing {
// Compiled out of production. Hooks obey the same no-lock/no-allocation rules.
struct Hooks {
    bool forceThreadSetMismatch{},forceLayoutMismatch{};
    std::size_t failContextIndex{static_cast<std::size_t>(-1)};
    void (*beforeWrite)(void*,std::size_t) noexcept{};
    void* context{};
};
Result TransactWithHooks(const Request& request,const Hooks& hooks) noexcept;
Result ScanStack(uintptr_t esp,uintptr_t stackLimit,uintptr_t stackBase,
    std::span<const BusyRange> ranges) noexcept;
Result ValidateSystemSnapshot(std::span<const std::byte> bytes,DWORD process,DWORD caller) noexcept;
}
#endif
}
