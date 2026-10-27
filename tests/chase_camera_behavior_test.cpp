#undef NDEBUG
#include "chase_camera_behavior.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace grid2vr;
namespace {
constexpr float nan=std::numeric_limits<float>::quiet_NaN();
constexpr float infinity=std::numeric_limits<float>::infinity();
bool Near(float a,float b,float tolerance=.00002f) { return std::abs(a-b)<=tolerance; }
bool Same(const ChaseOrbit& a,const ChaseOrbit& b) {
    return a.yaw==b.yaw && a.pitch==b.pitch && a.idle==b.idle && a.returning==b.returning && a.rearView==b.rearView;
}
bool Same(const ChaseZoom& a,const ChaseZoom& b) { return a.scale==b.scale && a.target==b.target; }
ChaseCameraConfig Enabled() { ChaseCameraConfig config;config.enabled=true;return config; }
ChaseCameraContext Context() {
    ChaseCameraContext context;
    context.camera=1;context.target=2;context.evaluation=1;context.dt=.1f;context.speedKmh=90;
    context.validOwner=true;context.focused=true;context.supportedState=true;
    return context;
}
ChaseCameraInput Devices() { ChaseCameraInput input;input.mouseAvailable=true;input.stickAvailable=true;return input; }
ChaseCameraStep Next(ChaseCameraState& state,const ChaseCameraConfig& config,ChaseCameraContext& context,const ChaseCameraInput& input) {
    ++context.evaluation;return state.Step(config,context,input);
}
void Arm(ChaseCameraState& state,const ChaseCameraConfig& config,ChaseCameraContext& context) {
    assert(state.Step(config,context,Devices())==ChaseCameraStep::AwaitingNeutral);
    assert(state.armed && !state.active);
}

void ConfigurationAndInput() {
    const ChaseCameraConfig defaults;
    assert(defaults.Valid() && !defaults.enabled && !defaults.keepAngle && !defaults.invertVertical);
    assert(defaults.mouseMode==ChaseMouseMode::Right && defaults.maxZoomOutPercent==180);
    assert(defaults.mouseSensitivityPercent==100 && defaults.stickSensitivityPercent==100);
    assert(static_cast<int>(ChaseMouseMode::Left)==0 && static_cast<int>(ChaseMouseMode::Right)==1 && static_cast<int>(ChaseMouseMode::Always)==2);
    for(int maximum=180;maximum<=1000;maximum+=10) {
        auto config=defaults;config.maxZoomOutPercent=maximum;assert(config.Valid());
    }
    for(int maximum:{-1,0,179,181,185,999,1001,1010}) {
        auto config=defaults;config.maxZoomOutPercent=maximum;assert(!config.Valid());
    }
    for(int sensitivity:{24,301}) {
        auto config=defaults;config.mouseSensitivityPercent=sensitivity;assert(!config.Valid());
        config=defaults;config.stickSensitivityPercent=sensitivity;assert(!config.Valid());
    }
    for(int mode:{-1,3}) { auto config=defaults;config.mouseMode=static_cast<ChaseMouseMode>(mode);assert(!config.Valid()); }

    auto mouse=ChaseMouseActivation(ChaseMouseMode::Right,true,false,true,true);
    assert(!mouse.active && !mouse.held && mouse.neutral); // Ordinary left-click remains free.
    mouse=ChaseMouseActivation(ChaseMouseMode::Right,false,true,false,true);
    assert(mouse.active && mouse.held && !mouse.neutral);
    mouse=ChaseMouseActivation(ChaseMouseMode::Left,true,false,false,true);
    assert(mouse.active && mouse.held && !mouse.neutral);
    mouse=ChaseMouseActivation(ChaseMouseMode::Always,false,false,false,true);
    assert(mouse.active && !mouse.held && mouse.neutral); // Still mouse permits automatic return.
    mouse=ChaseMouseActivation(ChaseMouseMode::Always,false,false,true,true);
    assert(mouse.active && mouse.held && !mouse.neutral);
    mouse=ChaseMouseActivation(ChaseMouseMode::Always,true,false,false,true);
    assert(mouse.active && !mouse.held && !mouse.neutral); // Focus-restoring click is not neutral.
    mouse=ChaseMouseActivation(ChaseMouseMode::Right,false,true,true,false);
    assert(!mouse.active && !mouse.held && !mouse.neutral);

    for(auto mode:{ChaseMouseMode::Left,ChaseMouseMode::Right,ChaseMouseMode::Always}) {
        auto config=defaults;config.mouseMode=mode;
        auto input=Devices();input.mouseX=12;input.mouseY=-8;input.wheel=-120;
        ChaseCameraMotion motion;
        assert(DecodeChaseCameraInput(config,input,.1f,motion));
        assert(motion.wheel==-120 && !motion.neutral);
        if(mode==ChaseMouseMode::Always)assert(Near(motion.horizontal,-.036f) && Near(motion.vertical,.024f));
        else assert(motion.horizontal==0 && motion.vertical==0 && !motion.held);
        input.leftMouse=mode==ChaseMouseMode::Left;input.rightMouse=mode==ChaseMouseMode::Right;
        assert(DecodeChaseCameraInput(config,input,.1f,motion));
        assert(Near(motion.horizontal,-.036f) && Near(motion.vertical,.024f) && motion.held);
    }
    for(int sensitivity:{25,100,300})for(bool invert:{false,true}) {
        auto config=defaults;config.mouseSensitivityPercent=sensitivity;config.stickSensitivityPercent=sensitivity;config.invertVertical=invert;
        auto input=Devices();input.rightMouse=true;input.mouseX=10;input.mouseY=10;
        ChaseCameraMotion motion;
        assert(DecodeChaseCameraInput(config,input,.1f,motion));
        const float factor=sensitivity/100.f;
        assert(Near(motion.horizontal,-.03f*factor) && Near(motion.vertical,(invert?.03f:-.03f)*factor));
        input=Devices();input.stickX=1;
        assert(DecodeChaseCameraInput(config,input,.1f,motion));
        assert(Near(motion.horizontal,-.25f*factor) && motion.vertical==0 && motion.held && !motion.neutral);
        input.stickX=0;input.stickY=1;
        assert(DecodeChaseCameraInput(config,input,.1f,motion));
        assert(motion.horizontal==0 && Near(motion.vertical,(invert?-.15f:.15f)*factor));
    }
    ChaseCameraMotion motion;
    auto input=Devices();input.stickX=.2f;
    assert(DecodeChaseCameraInput(defaults,input,.1f,motion) && motion.neutral && !motion.held && motion.horizontal==0);
    input.stickX=.1f;input.stickY=.1f;
    assert(DecodeChaseCameraInput(defaults,input,.1f,motion) && motion.neutral);
    input.stickX=.6f;input.stickY=0;
    assert(DecodeChaseCameraInput(defaults,input,.1f,motion) && Near(motion.horizontal,-.125f));
    input.stickX=1;input.stickY=1;
    assert(DecodeChaseCameraInput(defaults,input,.1f,motion));
    assert(Near(motion.horizontal,-.25f/std::sqrt(2.f)) && Near(motion.vertical,.15f/std::sqrt(2.f)));
    input=Devices();input.rearHeld=true;
    assert(DecodeChaseCameraInput(defaults,input,.1f,motion) && motion.rearHeld && !motion.neutral);
    input.stickAvailable=false;
    assert(DecodeChaseCameraInput(defaults,input,.1f,motion) && !motion.rearHeld && motion.neutral);
    // Refusal cannot leak partially decoded motion to a caller.
    motion.horizontal=19;motion.wheel=17;
    for(float bad:{nan,infinity,1.01f,-1.01f}) {
        input=Devices();input.stickX=bad;
        assert(!DecodeChaseCameraInput(defaults,input,.1f,motion) && motion.horizontal==19 && motion.wheel==17);
    }
    for(float bad:{nan,infinity}) {
        input=Devices();input.mouseY=bad;
        assert(!DecodeChaseCameraInput(defaults,input,.1f,motion) && motion.horizontal==19 && motion.wheel==17);
        input=Devices();input.wheel=bad;assert(!DecodeChaseCameraInput(defaults,input,.1f,motion));
    }
}

void OrbitAndZoom() {
    // Port of the reference's speed/retention/R3 scenarios, including reverse.
    for(int fps:{30,60,144})for(float speed:{-180.f,-10.f,0.f,.5f,10.f,180.f}) {
        const float dt=1.f/fps;
        ChaseOrbit normal,kept;
        assert(normal.Step(dt,speed,.8f,.3f,true));
        assert(kept.Step(dt,speed,.8f,.3f,true,false,true));
        for(int frame=0;frame<fps/2;++frame)assert(normal.Step(dt,speed,0,0,false));
        assert(normal.yaw==.8f && normal.pitch==.3f);
        for(int frame=0;frame<fps*3;++frame)assert(normal.Step(dt,speed,0,0,false));
        if(std::abs(speed)>=3)assert(normal.yaw==0 && normal.pitch==0);
        else assert(normal.yaw==.8f && normal.pitch==.3f);
        for(int frame=0;frame<fps*30;++frame)assert(kept.Step(dt,speed,0,0,false,false,true));
        assert(kept.yaw==.8f && kept.pitch==.3f);
        assert(kept.Step(dt,speed,-.2f,-.1f,true,false,true));
        assert(Near(kept.yaw,.6f) && Near(kept.pitch,.2f));
        kept.Reset();assert(Same(kept,ChaseOrbit{}));
    }
    for(bool keep:{false,true}) {
        ChaseOrbit orbit;
        assert(orbit.Step(.1f,90,.8f,.3f,true,false,keep));
        for(int frame=0;frame<100;++frame)assert(orbit.Step(.1f,90,0,0,true,false,keep));
        assert(orbit.yaw==.8f && orbit.pitch==.3f);
        assert(orbit.Step(.1f,0,.1f,.1f,true,true,keep));
        assert(orbit.yaw==ChaseOrbit::pi && orbit.pitch==0 && orbit.rearView);
        assert(orbit.Step(.1f,0,1,1,true,false,keep));
        assert(Same(orbit,ChaseOrbit{})); // Release ignores simultaneous orbit input.
        assert(orbit.Step(.1f,90,10,10,true,false,keep));
        assert(orbit.yaw>=-ChaseOrbit::pi && orbit.yaw<=ChaseOrbit::pi && orbit.pitch==ChaseOrbit::pi/3);
        assert(orbit.Step(.1f,90,0,-10,true,false,keep) && orbit.pitch==-ChaseOrbit::pi/6);
    }
    ChaseOrbit hysteresis;
    assert(hysteresis.Step(.1f,2,0,0,false) && !hysteresis.returning);
    assert(hysteresis.Step(.1f,-3,0,0,false) && hysteresis.returning);
    assert(hysteresis.Step(.1f,2,0,0,false) && hysteresis.returning);
    assert(hysteresis.Step(.1f,-1,0,0,false) && !hysteresis.returning);
    assert(hysteresis.Step(.1f,2,0,0,false) && !hysteresis.returning);
    for(int fps:{30,60,144}) {
        ChaseOrbit orbit;
        assert(orbit.Step(1.f/fps,90,.8f,.3f,true));
        for(int frame=0;frame<fps;++frame)assert(orbit.Step(1.f/fps,90,0,0,false));
        assert(Near(orbit.yaw,.8f*std::exp(-6.f*.25f)) && Near(orbit.pitch,.3f*std::exp(-6.f*.25f)));
        ChaseZoom zoom;
        for(int frame=0;frame<fps;++frame)assert(zoom.Step(1.f/fps,frame?0.f:-120.f));
        assert(Near(zoom.target,1.1f) && Near(zoom.scale,1.1f-.1f*std::exp(-12.f)));
    }
    for(int maximum=180;maximum<=1000;maximum+=10) {
        ChaseZoom zoom;
        for(int frame=0;frame<1200;++frame) {
            assert(zoom.Step(1.f/60,-120,maximum/100.f));
            assert(zoom.scale>=.6f && zoom.scale<=maximum/100.f && zoom.target<=maximum/100.f);
        }
        assert(zoom.target==maximum/100.f && zoom.scale==zoom.target);
        for(int frame=0;frame<1200;++frame)assert(zoom.Step(1.f/60,120,maximum/100.f));
        assert(zoom.target==.6f && zoom.scale==.6f);
        zoom.Reset();assert(Same(zoom,ChaseZoom{}));
    }
    ChaseZoom partial,burst,clamped;
    assert(partial.Step(.1f,60) && Near(partial.target,1/std::sqrt(1.1f)));
    assert(burst.Step(.1f,-16*120,10) && clamped.Step(.1f,-1000*120,10) && Same(burst,clamped));
    assert(burst.target>4 && burst.target<5); // Burst cap is independent of the maximum limit.
    ChaseOrbit orbit;assert(orbit.Step(.1f,90,.4f,.2f,true));const auto before=orbit;
    const auto zoomBefore=partial;
    for(float dt:{0.f,-.1f,.2501f,nan,infinity}) {
        assert(!orbit.Step(dt,90,1,1,true) && Same(orbit,before));
        assert(!partial.Step(dt,120) && Same(partial,zoomBefore));
    }
    assert(!orbit.Step(.1f,nan,0,0,false) && Same(orbit,before));
    assert(!orbit.Step(.1f,90,infinity,0,false) && Same(orbit,before));
    assert(!orbit.Step(.1f,90,0,nan,false) && Same(orbit,before));
    for(float maximum:{1.f,10.01f,nan,infinity})assert(!partial.Step(.1f,120,maximum) && Same(partial,zoomBefore));
    assert(!partial.Step(.1f,nan) && Same(partial,zoomBefore));
}

void StateAndRecovery() {
    auto config=Enabled();auto context=Context();auto input=Devices();ChaseCameraState state;
    input.rightMouse=true;input.mouseX=20;input.wheel=-120;
    assert(state.Step(config,context,input)==ChaseCameraStep::AwaitingNeutral && !state.armed && !state.active);
    assert(state.Step(config,context,Devices())==ChaseCameraStep::Duplicate && !state.armed);
    assert(Next(state,config,context,Devices())==ChaseCameraStep::AwaitingNeutral && state.armed && !state.active);
    assert(Next(state,config,context,input)==ChaseCameraStep::Updated && state.active);
    assert(Near(state.orbit.yaw,-.06f) && Near(state.zoom.target,1.1f));
    const auto onceOrbit=state.orbit;const auto onceZoom=state.zoom;
    for(int duplicate=0;duplicate<4;++duplicate) {
        assert(state.Step(config,context,input)==ChaseCameraStep::Duplicate && state.active);
        assert(Same(state.orbit,onceOrbit) && Same(state.zoom,onceZoom));
    }
    context.focused=false;
    assert(state.Step(config,context,input)==ChaseCameraStep::Inactive && !state.armed && !state.active);
    assert(Same(state.orbit,ChaseOrbit{}) && Same(state.zoom,onceZoom));
    context.focused=true;
    assert(state.Step(config,context,Devices())==ChaseCameraStep::Duplicate && !state.active && !state.armed);
    assert(Next(state,config,context,input)==ChaseCameraStep::AwaitingNeutral && !state.armed);
    input=Devices();input.wheel=120;
    assert(Next(state,config,context,input)==ChaseCameraStep::AwaitingNeutral && !state.armed);
    input=Devices();input.rearHeld=true;
    assert(Next(state,config,context,input)==ChaseCameraStep::AwaitingNeutral && !state.armed);
    assert(Next(state,config,context,Devices())==ChaseCameraStep::AwaitingNeutral && state.armed);
    input=Devices();input.stickX=1;
    assert(Next(state,config,context,input)==ChaseCameraStep::Updated && Near(state.orbit.yaw,-.25f));

    // Lifetime identities and explicit seek epochs reset both session values.
    for(int change=0;change<5;++change) {
        ChaseCameraState fresh;auto at=Context();Arm(fresh,config,at);
        input=Devices();input.rightMouse=true;input.mouseX=20;input.wheel=-120;
        assert(Next(fresh,config,at,input)==ChaseCameraStep::Updated);
        if(change==0)++at.camera;
        else if(change==1)++at.target;
        else if(change==2)++at.epoch;
        else if(change==3)at.replay=true;
        else at.evaluation=0; // Next becomes a lower serial, also a discontinuity.
        assert(Next(fresh,config,at,input)==ChaseCameraStep::AwaitingNeutral && !fresh.active && !fresh.armed);
        assert(Same(fresh.orbit,ChaseOrbit{}) && Same(fresh.zoom,ChaseZoom{}));
    }
    for(int reason=0;reason<9;++reason) {
        ChaseCameraState fresh;auto at=Context();Arm(fresh,config,at);
        assert(Next(fresh,config,at,input)==ChaseCameraStep::Updated);
        if(reason==0)at.validOwner=false;
        else if(reason==1)at.supportedState=false;
        else if(reason==2)at.camera=0;
        else if(reason==3)at.target=0;
        else if(reason==4)at.dt=0;
        else if(reason==5)at.dt=.3f;
        else if(reason==6)at.speedKmh=nan;
        else if(reason==7)at.dt=nan;
        else at.evaluation=0;
        const auto result=fresh.Step(config,at,input);
        assert(result==(reason<2?ChaseCameraStep::Inactive:ChaseCameraStep::Invalid));
        assert(!fresh.active && !fresh.armed && Same(fresh.orbit,ChaseOrbit{}));
    }
    ChaseCameraState changing;auto at=Context();Arm(changing,config,at);
    input=Devices();input.stickX=1;assert(Next(changing,config,at,input)==ChaseCameraStep::Updated);
    input.stickAvailable=false;
    assert(Next(changing,config,at,input)==ChaseCameraStep::AwaitingNeutral && !changing.active);
    input.stickAvailable=true;
    assert(Next(changing,config,at,input)==ChaseCameraStep::AwaitingNeutral && !changing.armed);
    assert(Next(changing,config,at,Devices())==ChaseCameraStep::AwaitingNeutral && changing.armed);
    input=Devices();input.mouseX=nan;
    assert(Next(changing,config,at,input)==ChaseCameraStep::Invalid && !changing.armed && !changing.active);
    assert(Next(changing,config,at,Devices())==ChaseCameraStep::AwaitingNeutral && changing.armed);
    changing.Suspend();assert(!changing.armed && !changing.active);
    config.keepAngle=true;config.maxZoomOutPercent=1000;config.mouseMode=ChaseMouseMode::Always;
    config.enabled=false;
    assert(Next(changing,config,at,Devices())==ChaseCameraStep::Inactive);
    assert(Same(changing.orbit,ChaseOrbit{}) && Same(changing.zoom,ChaseZoom{}));
    assert(config.keepAngle && config.maxZoomOutPercent==1000 && config.mouseMode==ChaseMouseMode::Always);
    config.enabled=true;input=Devices();input.leftMouse=true;
    assert(Next(changing,config,at,input)==ChaseCameraStep::AwaitingNeutral && !changing.armed);
    assert(Next(changing,config,at,Devices())==ChaseCameraStep::AwaitingNeutral && changing.armed);
    config.maxZoomOutPercent=181;
    assert(Next(changing,config,at,Devices())==ChaseCameraStep::Invalid && !changing.active && !changing.armed);
}

void PausedReplayAndFrameRates() {
    auto config=Enabled();auto context=Context();context.replay=true;context.replayForward=true;
    ChaseCameraState state;Arm(state,config,context);
    auto input=Devices();input.rightMouse=true;input.mouseX=-100;input.wheel=-120;
    assert(Next(state,config,context,input)==ChaseCameraStep::Updated);
    const float yaw=state.orbit.yaw;
    context.replayForward=false; // Pause and rewind share suppression of automatic return.
    for(int frame=0;frame<50;++frame)assert(Next(state,config,context,Devices())==ChaseCameraStep::Updated);
    assert(state.orbit.yaw==yaw);
    input.wheel=0;
    assert(Next(state,config,context,input)==ChaseCameraStep::Updated && Near(state.orbit.yaw,2*yaw));
    const auto once=state.orbit;
    assert(state.Step(config,context,input)==ChaseCameraStep::Duplicate && Same(state.orbit,once));
    context.replayForward=true;
    for(int frame=0;frame<40;++frame)assert(Next(state,config,context,Devices())==ChaseCameraStep::Updated);
    assert(state.orbit.yaw==0);
    context.replay=false;
    assert(Next(state,config,context,Devices())==ChaseCameraStep::AwaitingNeutral && !state.active);
    assert(Same(state.orbit,ChaseOrbit{}) && Same(state.zoom,ChaseZoom{}));
    // Mouse deltas are displacement, stick is velocity, and wheel is a burst.
    for(int fps:{30,60,144}) {
        ChaseCameraState smooth;auto at=Context();at.dt=1.f/fps;config.keepAngle=true;
        Arm(smooth,config,at);
        input=Devices();input.rightMouse=true;input.mouseX=100.f/fps;input.stickX=.6f;
        for(int frame=0;frame<fps;++frame) {
            input.wheel=frame?0.f:-120.f;
            assert(Next(smooth,config,at,input)==ChaseCameraStep::Updated);
        }
        assert(Near(smooth.orbit.yaw,-1.55f) && Near(smooth.zoom.target,1.1f));
        assert(Near(smooth.zoom.scale,1.1f-.1f*std::exp(-12.f)));
    }
}

void NativePoseMath() {
    constexpr ChaseVector up{0,1,0},right{1,0,0},pivot{10,20,30};
    const std::array<float,8> original{0,0,0,1,10,22,24,7};
    auto pose=original;
    assert(OrbitChasePose(pose,pivot,up,right,0,0));
    assert(!std::memcmp(pose.data(),original.data(),sizeof(pose)));
    pose={-0.f,0,-0.f,1,10,22,24,-0.f};const auto signedZero=pose;
    assert(OrbitChasePose(pose,pivot,up,right,0,0));
    assert(!std::memcmp(pose.data(),signedZero.data(),sizeof(pose)));
    for(float scale:{.6f,1.8f,5.f,10.f}) {
        pose=original;
        assert(OrbitChasePose(pose,pivot,up,right,0,0,scale));
        assert(!std::memcmp(pose.data(),original.data(),sizeof(float)*4));
        assert(pose[4]==10 && Near(pose[5],20+2*scale) && Near(pose[6],30-6*scale) && pose[7]==7);
    }
    pose={0,0,0,1,0,0,-6,7};
    assert(OrbitChasePose(pose,{0,0,0},up,right,ChaseOrbit::pi/2,0));
    assert(Near(pose[4],-6) && Near(pose[5],0) && Near(pose[6],0));
    assert(Near(pose[1],std::sqrt(.5f)) && Near(pose[3],std::sqrt(.5f)) && pose[7]==7);
    // Pitch is about camera-local right, also when the native base already yaws.
    assert(OrbitChasePose(pose,{0,0,0},up,right,0,ChaseOrbit::pi/6));
    assert(Near(pose[4],-3*std::sqrt(3.f)) && Near(pose[5],3) && Near(pose[6],0));
    pose={0,0,0,1,0,-6,0,7};
    assert(OrbitChasePose(pose,{0,0,0},{0,0,1},{0,1,0},ChaseOrbit::pi/2,0));
    assert(Near(pose[4],6) && Near(pose[5],0) && Near(pose[6],0)); // No built-in world-up convention.
    pose={0,0,0,1,0,0,-6,7};
    assert(OrbitChasePose(pose,{0,0,0},{0,-1,0},right,ChaseOrbit::pi/2,0));
    assert(Near(pose[4],6)); // Axis sign belongs to the adapter.
    pose={0,0,0,1.003f,0,0,-6,7};
    assert(OrbitChasePose(pose,{0,0,0},up,right,.8f,.4f,10));
    assert(Near(pose[4]*pose[4]+pose[5]*pose[5]+pose[6]*pose[6],3600,.002f));
    assert(Near(pose[0]*pose[0]+pose[1]*pose[1]+pose[2]*pose[2]+pose[3]*pose[3],1.003f*1.003f));
    // Refusal is transactional, including malformed native data and unsupported radii.
    for(int failure=0;failure<14;++failure) {
        pose=original;ChaseVector at=pivot,yawAxis=up,pitchAxis=right;float yaw=0,pitch=0,scale=1;
        if(failure==0)pose[3]=2;
        else if(failure==1)pose[0]=nan;
        else if(failure==2)at.x=infinity;
        else if(failure==3)yaw=nan;
        else if(failure==4)pitch=infinity;
        else if(failure==5)scale=.59f;
        else if(failure==6)scale=10.01f;
        else if(failure==7)scale=nan;
        else if(failure==8)yawAxis={0,0,0};
        else if(failure==9)pitchAxis={1.01f,0,0};
        else if(failure==10)pose[7]=nan;
        else if(failure==11)pose[4]=100;
        else if(failure==12) { pose[4]=10;pose[5]=20;pose[6]=30; }
        else yawAxis={infinity,0,0};
        const auto before=pose;
        assert(!OrbitChasePose(pose,at,yawAxis,pitchAxis,yaw,pitch,scale));
        assert(!std::memcmp(pose.data(),before.data(),sizeof(pose)));
    }
}
}
int main() {
    ConfigurationAndInput();OrbitAndZoom();StateAndRecovery();PausedReplayAndFrameRates();NativePoseMath();
    std::puts("chase camera behavior: configuration/input, orbit/zoom, recovery/deduplication, replay/timing and native-pose math passed");
}
