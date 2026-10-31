#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define CHECK(value) do {if(!(value)){std::printf("pair guard failure line %d: %s\n",__LINE__,#value);std::exit(1);}}while(false)
namespace {
bool lodSuppressed{};
unsigned changes{};
}
namespace grid2vr::opponentlod::runtime {
void SetEyeReplay(bool value) noexcept {lodSuppressed=value;++changes;}
}
namespace grid2vr::engine {
struct PendingScene {uint64_t frame;};
bool dualEyes=true,replayedEmptyBuilder=true,replace{};
// A native left-eye frame in replace mode leaves only the right eye to replay.
bool NativeLeftReplace() {return replace;}
unsigned replayPass{},calls{},failPass{},faultPass{},badGpuPass{};
const PendingScene* first{};
bool RenderReplay(const PendingScene& scene,bool nativeStereo,bool firstImageSaved,bool& restored) {
    CHECK(lodSuppressed && nativeStereo && firstImageSaved);
    const bool skipLeft=replace && dualEyes;
    CHECK(replayPass==++calls+(skipLeft?1u:0u) && calls<=(skipLeft?1u:2u));
    CHECK(!first || first==&scene);first=&scene;
    CHECK(changes==1); // No policy-enabled gap between the two native eye calls.
    if(replayPass==faultPass)RaiseException(0xe0424c4f,0,0,nullptr);
    restored=replayPass!=badGpuPass;
    return replayPass!=failPass;
}
#include "opponent_lod_pair.inc"

void Reset(unsigned fail=0,unsigned fault=0,unsigned gpu=0,bool stereo=true,bool nativeLeft=false) {
    lodSuppressed=false;changes=0;replayPass=0;calls=0;first=nullptr;replace=nativeLeft;
    failPass=fail;faultPass=fault;badGpuPass=gpu;dualEyes=stereo;
}
void CheckFault(unsigned pass) {
    Reset(0,pass);bool gpu=true,empty=true,caught=false;PendingScene scene{53};
    __try {InvokeReplayPair(scene,true,true,gpu,empty);}
    __except(GetExceptionCode()==0xe0424c4f?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {caught=true;}
    CHECK(caught && calls==pass && !lodSuppressed && changes==2 && replayPass==0);
}
}
int main() {
    using namespace grid2vr::engine;
    PendingScene scene{51};bool gpu=true,empty=true;
    Reset();CHECK(InvokeReplayPair(scene,true,true,gpu,empty));
    CHECK(calls==2 && gpu && empty && !lodSuppressed && changes==2 && replayPass==0);
    Reset(1);gpu=empty=true;CHECK(!InvokeReplayPair(scene,true,true,gpu,empty) && calls==1 && !lodSuppressed && replayPass==0);
    Reset(2);gpu=empty=true;CHECK(!InvokeReplayPair(scene,true,true,gpu,empty) && calls==2 && !lodSuppressed && replayPass==0);
    Reset(0,0,1);gpu=empty=true;CHECK(InvokeReplayPair(scene,true,true,gpu,empty) && !gpu && !lodSuppressed);
    Reset(0,0,0,false);gpu=empty=true;CHECK(InvokeReplayPair(scene,true,true,gpu,empty) && calls==1 && !lodSuppressed);
    CheckFault(1);CheckFault(2);
    Reset(0,0,0,true,true);gpu=empty=true;
    CHECK(InvokeReplayPair(scene,true,true,gpu,empty) && calls==1 && gpu && !lodSuppressed && changes==2 && replayPass==0);
    Reset(2,0,0,true,true);gpu=empty=true;CHECK(!InvokeReplayPair(scene,true,true,gpu,empty) && calls==1 && !lodSuppressed && replayPass==0);
    Reset(0,0,0,false,true);gpu=empty=true;CHECK(InvokeReplayPair(scene,true,true,gpu,empty) && calls==1 && replayPass==0);
    std::puts("PASS: production whole-eye-pair LOD suppression, same source, early refusal and native SEH cleanup");
}
