#include "opponent_lod_bridge.h"
#include "opponent_lod_build_mode.h"
#include <array>
#include <fstream>
#include <locale>

namespace grid2vr::opponentlod::bridge {
namespace {
quiescence::Result last;
std::filesystem::path output;
unsigned events{};
constexpr bool ProbeOnly=mode::Diagnostic;
void Write(std::ostream& log) {
    const auto& r=last;
    log<<std::boolalpha<<"{\"failure\":\""<<quiescence::FailureName(r.failure)
        <<"\",\"probeOnly\":"<<ProbeOnly<<",\"probePassed\":"<<r.probePassed
        <<",\"controlledActivation\":"<<mode::ControlledActivation
        <<",\"committed\":"<<r.committed<<",\"layoutValidated\":"<<r.layoutValidated
        <<",\"threadId\":"<<r.threadId<<",\"instruction\":"<<r.instruction<<",\"stackPointer\":"<<r.stackPointer
        <<",\"busyValue\":"<<r.busyValue<<",\"busyStackAddress\":"<<r.busyStackAddress
        <<",\"busyRangeIndex\":"<<r.busyRangeIndex<<",\"busyRangeBegin\":"<<r.busyRangeBegin<<",\"busyRangeEnd\":"<<r.busyRangeEnd
        <<",\"peerCount\":"<<r.peerCount<<",\"suspendedCount\":"<<r.suspendedCount
        <<",\"stackBytesScanned\":"<<r.stackBytesScanned<<",\"writtenMask\":"<<r.writtenMask
        <<",\"rolledBackMask\":"<<r.rolledBackMask<<",\"rollbackConflictMask\":"<<r.rollbackConflictMask
        <<",\"resumeFailures\":"<<r.resumeFailures<<",\"closeFailures\":"<<r.closeFailures
        <<",\"win32Error\":"<<r.win32Error<<",\"ntStatus\":"<<r.ntStatus
        <<",\"timingAvailable\":"<<r.timingAvailable<<",\"totalMicroseconds\":"<<r.totalMicroseconds
        <<",\"pauseScopeMeasured\":"<<r.pauseScopeMeasured<<",\"pauseScopeMicroseconds\":"<<r.pauseScopeMicroseconds
        <<",\"pauseScopeIsUpperBound\":true,\"rawStackRetained\":false}\n";
}
void Receipt() noexcept {
    if(output.empty())return;
    try {
        std::ofstream current(output/L"opponent-lod-transaction.json",std::ios::trunc);
        current.imbue(std::locale::classic());Write(current);
        if(events<64) {
            std::ofstream journal(output/L"opponent-lod-transaction.jsonl",events?std::ios::app:std::ios::trunc);
            journal.imbue(std::locale::classic());Write(journal);++events;
        }
    }catch(...) {}
}
}
void Configure(const std::filesystem::path& folder) noexcept {
    try {output=folder;events=0;}catch(...) {output.clear();}
}
const char* Transaction(std::span<const runtime::PointerChange> changes,std::span<const runtime::BusyRange> ranges,
    bool (*validate)(void*) noexcept,void* context,void (*committed)(void*) noexcept) noexcept {
    if(changes.size()>quiescence::MaxWrites || ranges.size()>quiescence::MaxBusyRanges) {
        last={};last.failure=quiescence::Failure::InvalidRequest;
        Receipt();
        return "opponent_lod_transaction_bounds";
    }
    std::array<quiescence::PointerWrite,quiescence::MaxWrites> writes{};
    std::array<quiescence::BusyRange,quiescence::MaxBusyRanges> busy{};
    for(std::size_t i=0;i<changes.size();++i)writes[i]={changes[i].slot,changes[i].expected,changes[i].replacement};
    for(std::size_t i=0;i<ranges.size();++i)busy[i]={ranges[i].begin,ranges[i].end};
    last=quiescence::Transact({true,{writes.data(),changes.size()},{busy.data(),ranges.size()},validate,committed,context,ProbeOnly});
    Receipt();
    if(last.rollbackConflictMask)return "opponent_lod_rollback_conflict_storage_retained";
    if(last.resumeFailures || last.closeFailures)
        return last.committed?"opponent_lod_cleanup_failed_after_commit":"opponent_lod_transaction_cleanup_failed";
    if(last.probePassed && !last.committed)return "diagnostic_probe_safe_no_writes";
    if(!last.committed)
        return last.failure==quiescence::Failure::None?"opponent_lod_transaction_not_committed":quiescence::FailureName(last.failure);
    return nullptr;
}
quiescence::Result LastTransaction() noexcept {return last;}
}
