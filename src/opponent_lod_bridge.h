#pragma once
#include "opponent_lod_runtime.h"
#include "quiesced_heap_transaction.h"

namespace grid2vr::opponentlod::bridge {
// Controller only, before first transaction. Receipts are bounded and written
// only AFTER every peer-resume/cleanup attempt, never while scanning peers.
void Configure(const std::filesystem::path& output) noexcept;
const char* Transaction(std::span<const runtime::PointerChange>,std::span<const runtime::BusyRange>,
    bool (*validate)(void*) noexcept,void* context,void (*committed)(void*) noexcept) noexcept;
// Controller-thread diagnostics only. Ownership still follows committed(), not
// a successful-looking cleanup result or the absence of an error string.
quiescence::Result LastTransaction() noexcept;
}
