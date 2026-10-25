#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>

namespace grid2vr::processexit {
// Called only on the thread that installs the handler, before the game's own
// ExitProcess import. The caller must already have verified the AVX image hash.
using BeforeExit = const char* (*)();
bool Install(uintptr_t gameBase, const std::filesystem::path& output, BeforeExit callback);
// Pump only at a restored render boundary. Foreign exit callers wait briefly
// for this owner; timeout preserves native exit without cross-thread XR calls.
void Service();
}
