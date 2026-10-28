#include "driving_controls_steering.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace grid2vr::drivingcontrols;

void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
using Controller=std::array<uint8_t,0x6c>;
template<class T> T Get(const Controller& state,size_t offset) {T value;std::memcpy(&value,state.data()+offset,sizeof(value));return value;}
bool Near(float a,float b) {return std::abs(a-b)<.00001f;}
void CheckUnrelated(const Controller& before,const Controller& after) {
    for(size_t i=0;i<before.size();++i)
        if(!(i>=0x28 && i<0x30) && i!=0x38)Check(before[i]==after[i],"unrelated controller byte changed");
    Check(after[0x38]==(before[0x38]&~0x20u),"only pad steering flag may be cleared");
}
int main() {
    WheelCurveOverride patch{0x20000,.4f,0};float restoreValue=.9f;
    Check(!RestoreWheelCurve(patch,0x10000,true,0,restoreValue) && patch.object==0x20000 && patch.native==.4f && patch.applied==0 && restoreValue==.9f,
        "stale controller A cannot consume controller B's curve ownership");
    Check(!RestoreWheelCurve(patch,0x20000,false,0,restoreValue) && patch.object==0x20000 && patch.native==.4f && patch.applied==0 && restoreValue==.9f,
        "temporarily unproven route retains curve ownership and output");
    Check(RestoreWheelCurve(patch,0x20000,true,0,restoreValue) && restoreValue==.4f && !patch.object,
        "verified original controller restores its native curve after route recovery");
    restoreValue=.9f;
    Check(!RestoreWheelCurve(patch,0x20000,true,0,restoreValue) && restoreValue==.9f,"consumed ownership cannot restore twice");
    patch={0x20000,.4f,0};
    Check(!RestoreWheelCurve(patch,0x20000,true,-.2f,restoreValue) && !patch.object && restoreValue==.9f,
        "external native curve change revokes ownership without overwriting the new value");
    Controller native;native.fill(0xff);native[0x40]=1;
    for(float value:{.25f,.5f,1.0f})for(unsigned side=0;side<2;++side) {
        WheelSteeringInput input;input.connected=true;input.wheel[side]=value;
        auto state=native;
        Check(ApplyWheelSteering(state,input,0),"quarter, half and full wheel movements select wheel mode");
        Check(Near(Get<float>(state,0x28),side?value:-value),"signed steering is linear at quarter, half and full travel");
        Check(Get<float>(state,0x2c)==0,"neutral response curve");CheckUnrelated(native,state);
    }
    auto state=native;
    Check(ApplyWheelSteering(state,{{},{},true},0) && Get<float>(state,0x28)==0 && !(state[0x38]&0x20),
        "connected centered wheel retains wheel mode without suppressing an active competitor");
    CheckUnrelated(native,state);
    for(const auto other:std::array<std::array<float,2>,3>{{{0,.8f},{.8f,0},{0,.5f}}}) {
        state=native;
        Check(!ApplyWheelSteering(state,{{0,.5f},other,true},0) && state==native,
            "stronger or equal nonzero keyboard/pad steering preserves the native state");
    }
    state=native;
    Check(ApplyWheelSteering(state,{{.8f,0},{0,.3f},true},0) && Near(Get<float>(state,0x28),-.5f),
        "opposing simultaneous input remains represented when wheel dominates");CheckUnrelated(native,state);
    state=native;
    Check(ApplyWheelSteering(state,{{0,.8f},{0,.3f},true},0) && Near(Get<float>(state,0x28),.8f),
        "same-direction inputs use maximum rather than adding gain");
    state=native;
    Check(ApplyWheelSteering(state,{{.6f,.6f},{},true},0) && Get<float>(state,0x28)==0,
        "equal opposing wheel actions cancel");
    for(int linearity:{-10,-5,0,5,10}) {
        state=native;
        Check(ApplyWheelSteering(state,{{0,.25f},{},true},linearity),"valid response curve accepted");
        const auto curve=Get<float>(state,0x2c);
        Check(Near(curve,-linearity/10.0f),"launcher curve maps to the proven native parameter");
        const auto response=std::pow(.25f,1.0f-curve*.5f);
        Check(linearity<0?response>.25f:linearity>0?response<.25f:response==.25f,"curve direction at quarter travel");
        if(linearity==-10)Check(Near(response,.5f),"quickest curve doubles quarter travel");
        if(linearity==10)Check(Near(response,.125f),"gentlest curve halves quarter travel");
        CheckUnrelated(native,state);
    }
    for(float invalid:{-.01f,1.01f,std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        for(unsigned source=0;source<2;++source)for(unsigned side=0;side<2;++side) {
            WheelSteeringInput input{{0,.5f},{},true};(source?input.other:input.wheel)[side]=invalid;state=native;
            Check(!ApplyWheelSteering(state,input,0) && state==native,"invalid sample refuses all writes");
        }
    for(int invalid:{-11,11,std::numeric_limits<int>::min(),std::numeric_limits<int>::max()}) {
        state=native;Check(!ApplyWheelSteering(state,{{0,.5f},{},true},invalid) && state==native,"invalid curve refuses all writes");
    }
    for(uint8_t enabled:{uint8_t{0},uint8_t{2}}) {
        state=native;state[0x40]=enabled;const auto before=state;
        Check(!ApplyWheelSteering(state,{{0,.5f},{},true},0) && state==before,"inactive or unknown active flag refuses all writes");
    }
    state=native;Check(!ApplyWheelSteering(state,{{0,.5f},{},false},0) && state==native,"disconnected wheel preserves native input");
    Check(!ApplyWheelSteering(std::span(state).first(0x40),{{0,.5f},{},true},0) && state==native,"short controller rejected before reading active flag");
    std::array<uint8_t,0x70> oversized{};const auto before=oversized;
    Check(!ApplyWheelSteering(oversized,{{0,.5f},{},true},0) && oversized==before,"unknown controller size rejected");
    std::cout<<"Wheel steering selection, native field isolation and response curves passed.\n";
}
