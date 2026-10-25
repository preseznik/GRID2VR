#include "scene_call.h"
#include "eye_view.h"
#include "replay_sequence.h"
#include <cstdio>
namespace {
void* actualSelf{}; uintptr_t actualContext{},actualView{}; float actualFar{}; uint32_t actualFlags{};
void __fastcall CameraStub(void* self,void*,uintptr_t context,uintptr_t view,float farPlane,uint32_t flags) {
    actualSelf=self; actualContext=context; actualView=view; actualFar=farPlane; actualFlags=flags;
}
}
int main() {
    grid2vr::ReplaySequence sequence;
    if (sequence.Configure(0) || sequence.Configure(121) || !sequence.Configure(120)) return 30;
    for (uint64_t frame=10;frame<130;++frame) {
        if (!sequence.Available() || !sequence.Observing() || !sequence.Begin(frame) || sequence.Available() || !sequence.Observing()) return 31;
        sequence.Finish(true);
    }
    if (sequence.Available() || sequence.Observing() || sequence.Begin(130) || sequence.Completed()!=120) return 32;
    grid2vr::ReplaySequence extended;
    if (extended.Configure(900) || extended.Configure(901,true) || !extended.Configure(900,true)) return 35;
    for (uint64_t frame=1;frame<=900;++frame) {
        if (!extended.Begin(frame)) return 36;
        extended.Finish(true);
    }
    if (extended.Available() || extended.Completed()!=900 || extended.Configure(900,true)) return 37;
    grid2vr::ReplaySequence gap; gap.Configure(4); gap.Begin(20); gap.Finish(true);
    if (gap.Begin(22) || gap.Available() || gap.Observing()) return 33;
    grid2vr::ReplaySequence failure; failure.Configure(4); failure.Begin(20); failure.Finish(false);
    if (failure.Available() || failure.Observing() || failure.Completed()) return 34;
    grid2vr::CameraContext camera{}; grid2vr::SceneView original{},offset{};
    const float right[]{.6f,0,.8f},opposite[]{-.6f,0,-.8f},position[]{100,5,-20};
    memcpy(camera.data()+0x70,right,12); memcpy(camera.data()+0xa0,position,12);
    memcpy(original.data()+0x20,opposite,12); memcpy(original.data()+0x40,position,12);
    original[0x8c]=0; original[0x4f]=0x3f;
    for (float distance:{-.064f,0.f,.064f}) {
        if (!grid2vr::OffsetSceneView(camera,original,distance,offset)) return 10;
        for (size_t i=0;i<offset.size();++i) if ((i<0x40 || i>=0x4c) && offset[i]!=original[i]) return 11;
        float shifted[3]{}; memcpy(shifted,offset.data()+0x40,12);
        for (size_t i=0;i<3;++i) if (std::abs(shifted[i]-position[i]-right[i]*distance)>0.00001f) return 12;
    }
    const auto sentinel=offset;
    for (float bad:{.11f,INFINITY,NAN}) {
        if (grid2vr::OffsetSceneView(camera,original,bad,offset) || offset!=sentinel) return 13;
    }
    auto invalid=original; memset(invalid.data()+0x20,0,12);
    if (grid2vr::OffsetSceneView(camera,invalid,.064f,offset)) return 14;
    // A rotated camera and asymmetric projection; history must use the
    // previous camera's projection rather than assuming the current FOV.
    const float previousVp[]{.84f,0,.8f,.8f, 0,1.9f,0,0, -1.12f,0,.6f,.6f, -106.4f,-9.5f,-68.2f,-68.f};
    memcpy(camera.data()+0x190,previousVp,64);
    grid2vr::HistoryState entry{},prepared{}; prepared.fill(0x51);
    memcpy(entry.data()+0x10,previousVp,64);
    const auto untouched=prepared;
    if (!grid2vr::PrepareEyeHistory(camera,original,entry,.064f,prepared)) return 16;
    for (size_t i=0;i<prepared.size();++i)
        if ((i<0x50 || i>=0x90) && prepared[i]!=untouched[i]) return 17;
    float history[16]{}; memcpy(history,prepared.data()+0x50,64);
    if (memcmp(history,previousVp,48)) return 18;
    grid2vr::OffsetSceneView(camera,original,.064f,offset);
    float target[3]{}; memcpy(target,offset.data()+0x40,12);
    for (size_t column=0;column<4;++column) {
        double expected=previousVp[12+column];
        for (size_t row=0;row<3;++row) expected-=(double(target[row])-position[row])*previousVp[row*4+column];
        if (history[12+column]!=static_cast<float>(expected)) return 19;
    }
    const auto goodHistory=prepared;
    entry[0x10]^=1;
    if (grid2vr::PrepareEyeHistory(camera,original,entry,.064f,prepared) || prepared!=goodHistory) return 20;
    memcpy(entry.data()+0x10,previousVp,64);
    const float nonfinite=NAN; memcpy(camera.data()+0x190,&nonfinite,4); memcpy(entry.data()+0x10,&nonfinite,4);
    if (grid2vr::PrepareEyeHistory(camera,original,entry,.064f,prepared) || prepared!=goodHistory) return 21;
    for (int i=0;i<100;++i) {
        grid2vr::BuildNativeCamera(reinterpret_cast<grid2vr::NativeCameraFn>(CameraStub),&camera,0x12345678,original,15000.f);
        if (actualSelf!=&camera || actualContext!=0x12345678 || actualView!=reinterpret_cast<uintptr_t>(original.data()) ||
            actualFar!=15000.f || actualFlags!=3) return 15;
    }
    constexpr uintptr_t base=0x820000,context=0x40004000,view=0x40006000,packet=0x20002000;
    std::array<uintptr_t,15> stack{base+0x3be09e,context,view,packet,1,2,3,4,
        base+0x7a7cd2,packet,0x11223344,view,context,0x55667788,1};
    grid2vr::SceneCall call;
    if (!grid2vr::DecodeSceneCall(stack,base,5,context,view,packet,call) ||
        call.args!=std::array<uintptr_t,6>{packet,0x11223344,view,context,0x55667788,1}) return 1;
    for (unsigned index:{0,1,2,3,8,9,11,12}) {
        auto bad=stack; bad[index]^=0x100;
        if (grid2vr::DecodeSceneCall(bad,base,5,context,view,packet,call)) return 2;
    }
    if (grid2vr::DecodeSceneCall({stack.data(),14},base,5,context,view,packet,call) ||
        grid2vr::DecodeSceneCall(stack,0x400000,5,context,view,packet,call)) return 3;
    stack[0]=base+0x3be1b8;
    if (!grid2vr::DecodeSceneCall(stack,base,6,context,view,packet,call)) return 4;
    stack[14]=0xaabbcc01;
    if (!grid2vr::DecodeSceneCall(stack,base,6,context,view,packet,call) || call.args[5]!=0xaabbcc01) return 5;
    stack[14]=0xaabbcc02;
    if (grid2vr::DecodeSceneCall(stack,base,6,context,view,packet,call)) return 6;
    puts("Scene stack: both callback sites, ASLR base, argument identity and malformed/truncated callers checked.");
}
