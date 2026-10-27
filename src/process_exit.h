#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>

namespace grid2vr::processexit {
// The primary cleanup runs only on the installing thread, before the game's own
// ExitProcess import. The caller must already have verified the AVX image hash.
using BeforeExit = const char* (*)();
// Optional observer cleanup runs once on the exit-calling thread before the
// owner handoff. It must only restore thread-safe static hooks, with bounded
// lock acquisition; it must not touch renderer/XR objects or report their stop.
bool Install(uintptr_t gameBase, const std::filesystem::path& output, BeforeExit callback,
             BeforeExit observerCleanup=nullptr);
// Arm only for the verified runtime's DXGI OutputWindow. Defer window close
// before native teardown stops the render owner; never call XR from the UI.
bool AttachWindow(HWND window);
// Pump only at a restored render boundary. Foreign exit callers wait briefly
// for this owner; timeout preserves native exit without cross-thread XR calls.
void Service();
}
