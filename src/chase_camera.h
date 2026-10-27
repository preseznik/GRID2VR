#pragma once
#include <cstdint>
#include <filesystem>
#ifdef GRID2VR_CHASE_CAMERA_TEST
#include "chase_camera_behavior.h"
#endif
namespace grid2vr::chasecamera {
// Prepare is called only after the supported AVX executable fingerprint passes.
void Prepare(uintptr_t base,const std::filesystem::path& output);
void Install();
void Present(uint64_t frame,unsigned windowState);
void Stop();
const char* StopForExit();
#ifdef GRID2VR_CHASE_CAMERA_TEST
void TestOriginals(void*,void*,void*,void*,void*,void* copy=nullptr,void* look=nullptr,void* copyBack=nullptr);
void TestInput(uint64_t,const ChaseCameraInput&,bool focused,float dt=1.f/60);
uintptr_t TestCall(unsigned,void*,void*,uint32_t flag=0);
uintptr_t TestProvider(void*,void*,uintptr_t callerRva=0x1f78ed);
uintptr_t TestGetter(void*,uintptr_t callerRva=0x59a16d);
void TestTargetOriginal(void*);
uintptr_t TestCollisionTarget(void*,void*,uint32_t adjusted=1,uintptr_t callerRva=0x1f84f6);
uintptr_t TestTransaction(unsigned,void*,void*,uintptr_t callerRva,uintptr_t returnSlot);
#endif
}
