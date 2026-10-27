#pragma once
#include <cstdint>
#include <filesystem>

namespace grid2vr::chaseobserver {
// Diagnostic only: called after the shared supported-executable validation. No input
// is polled and no camera, timing, replay or renderer state is written.
void Prepare(uintptr_t base,const std::filesystem::path& output);
void Install(); // Wait for the existing native-session readiness gate.
void Present(uint64_t frame,unsigned windowState);
void Stop();
// Only static table pointers and observer-owned state; no camera/XR teardown.
// State and receipt-writer mutex acquisition each wait at most 250 ms. OS file
// I/O is not time-bounded. observer_restored_receipt_pending means static hooks
// are restored and an active writer must still publish the final snapshot.
const char* StopForExit();
#ifdef GRID2VR_CHASE_OBSERVER_TEST
void TestWithLock(void (*callback)());
void TestBeforeWrite(void (*callback)());
void TestOriginals(void* update,void* alternate,void* copy,void* stage,void* getter,void* render,void* provider=nullptr);
uintptr_t TestProvider(void* self,void* output,uintptr_t callerRva=0x1f78ed);
uintptr_t TestRender(void* self,uintptr_t context,uintptr_t view,uintptr_t packet);
uintptr_t TestExport(void* self,const void* stack);
void TestRenderCapture(void* self,uintptr_t context,uintptr_t view,uintptr_t packet,const void* stack);
uintptr_t TestCall(unsigned route,void* camera,void* argument,uint32_t flag=0);
#endif
}
