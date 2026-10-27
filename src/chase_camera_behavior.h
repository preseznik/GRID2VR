#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace grid2vr {
// Behavior ported from DiRT2VR artifacts/prize-cars/src/chase_{orbit,zoom,mouse,
// transform}.h and chase_camera.cpp::Poll. No game ownership, axis convention,
// device polling, recorded data or headset motion is inferred by these helpers.
enum class ChaseMouseMode : int { Left=0, Right=1, Always=2 };

struct ChaseCameraConfig {
    bool enabled{},keepAngle{};
    ChaseMouseMode mouseMode{ChaseMouseMode::Right};
    int maxZoomOutPercent{180},mouseSensitivityPercent{100},stickSensitivityPercent{100};
    bool invertVertical{};
    bool Valid() const {
        return (mouseMode==ChaseMouseMode::Right || mouseMode==ChaseMouseMode::Left || mouseMode==ChaseMouseMode::Always) &&
            maxZoomOutPercent>=180 && maxZoomOutPercent<=1000 && maxZoomOutPercent%10==0 &&
            mouseSensitivityPercent>=25 && mouseSensitivityPercent<=300 &&
            stickSensitivityPercent>=25 && stickSensitivityPercent<=300;
    }
};

struct ChaseOrbit {
    float yaw{},pitch{},idle{};
    bool returning{},rearView{};
    static constexpr float pi=3.14159265358979323846f;
    void Reset() { *this={}; }
    bool Step(float dt,float speedKmh,float horizontal,float vertical,bool inputHeld,bool rearHeld=false,bool keepAngle=false) {
        if(!std::isfinite(dt) || dt<=0 || dt>.25f || !std::isfinite(speedKmh) ||
            !std::isfinite(horizontal) || !std::isfinite(vertical))return false;
        if(rearHeld) { yaw=pi;pitch=0;idle=0;rearView=true;return true; }
        // R3 release deliberately discards the preceding orbit, also with Keep angle.
        if(rearView) { Reset();return true; }
        const float speed=std::abs(speedKmh);
        if(speed>=3)returning=true;
        else if(speed<=1)returning=false;
        if(inputHeld || horizontal!=0 || vertical!=0) {
            idle=0;
            yaw=std::remainder(yaw+horizontal,2*pi);
            pitch=std::clamp(pitch+vertical,-pi/6,pi/3);
        } else {
            const float previous=idle;
            idle=std::min(idle+dt,1.f);
            if(!keepAngle && returning && idle>.75f) {
                const float active=previous>=.75f?dt:idle-.75f;
                const float decay=std::exp(-6.f*active);
                yaw*=decay;pitch*=decay;
                if(std::abs(yaw)<.0001f)yaw=0;
                if(std::abs(pitch)<.0001f)pitch=0;
            }
        }
        return true;
    }
};

struct ChaseZoom {
    float scale{1},target{1};
    void Reset() { *this={}; }
    bool Step(float dt,float wheel,float maximum=1.8f) {
        if(!std::isfinite(dt) || dt<=0 || dt>.25f || !std::isfinite(wheel) ||
            !std::isfinite(maximum) || maximum<1.8f || maximum>10.f)return false;
        // 120 units per notch, independent of mouse-look activation. The native
        // camera-to-pivot distance changes; native FOV and head scale do not.
        target=std::clamp(target*std::pow(1.1f,-std::clamp(wheel/120.f,-16.f,16.f)),.6f,maximum);
        scale=target+(scale-target)*std::exp(-12.f*dt);
        if(std::abs(scale-target)<.00001f)scale=target;
        return true;
    }
};

struct ChaseMouseState { bool active{},held{},neutral{}; };
inline ChaseMouseState ChaseMouseActivation(ChaseMouseMode mode,bool left,bool right,bool moved,bool available) {
    const bool always=mode==ChaseMouseMode::Always;
    const bool button=mode==ChaseMouseMode::Left?left:right;
    const bool active=available && (always || button);
    return {active,active && (always?moved:button),always?!left && !right && !moved:!button};
}

struct ChaseCameraInput {
    bool mouseAvailable{},leftMouse{},rightMouse{};
    float mouseX{},mouseY{},wheel{}; // Device deltas, wheel in 120-unit notches.
    bool stickAvailable{};
    float stickX{},stickY{}; // Adapter-selected controller, normalized to [-1,1].
    bool rearHeld{};
};
struct ChaseCameraMotion {
    float horizontal{},vertical{},wheel{};
    bool held{},rearHeld{},neutral{true};
};
inline bool DecodeChaseCameraInput(const ChaseCameraConfig& config,const ChaseCameraInput& input,float dt,ChaseCameraMotion& output) {
    if(!config.Valid() || !std::isfinite(dt) || dt<=0 || dt>.25f)return false;
    if(input.mouseAvailable && (!std::isfinite(input.mouseX) || !std::isfinite(input.mouseY) || !std::isfinite(input.wheel)))return false;
    if(input.stickAvailable && (!std::isfinite(input.stickX) || !std::isfinite(input.stickY) ||
        std::abs(input.stickX)>1 || std::abs(input.stickY)>1))return false;
    ChaseCameraMotion result;
    const auto mouse=ChaseMouseActivation(config.mouseMode,input.leftMouse,input.rightMouse,
        input.mouseAvailable && (input.mouseX!=0 || input.mouseY!=0),input.mouseAvailable);
    result.held=mouse.held;result.neutral=mouse.neutral;
    if(input.mouseAvailable)result.wheel=input.wheel;
    if(result.wheel!=0)result.neutral=false;
    if(mouse.active) {
        const float sensitivity=config.mouseSensitivityPercent/100.f;
        result.horizontal=-input.mouseX*.003f*sensitivity;
        result.vertical=-input.mouseY*.003f*sensitivity;
    }
    if(input.stickAvailable) {
        const float length=std::sqrt(input.stickX*input.stickX+input.stickY*input.stickY);
        result.rearHeld=input.rearHeld;
        if(length>.2f || input.rearHeld)result.neutral=false;
        if(length>.2f) {
            const float amount=std::min((length-.2f)/.8f,1.f)/length;
            const float sensitivity=config.stickSensitivityPercent/100.f;
            result.horizontal-=input.stickX*amount*2.5f*dt*sensitivity;
            result.vertical+=input.stickY*amount*1.5f*dt*sensitivity;
            result.held=true;
        }
    }
    if(config.invertVertical)result.vertical=-result.vertical;
    if(!std::isfinite(result.horizontal) || !std::isfinite(result.vertical))return false;
    output=result;
    return true;
}

struct ChaseCameraContext {
    std::uint64_t camera{},target{},epoch{},evaluation{};
    float dt{},speedKmh{};
    bool validOwner{},focused{},supportedState{},replay{},replayForward{};
    // camera/target are validated lifetime identities, not unqualified addresses.
    // epoch advances on seek, replay discontinuity or same-address replacement.
    // evaluation is one monotonic input-update serial, shared by repeated camera
    // evaluations and both eyes. It must advance during paused replay. Replay dt
    // is elapsed monotonic input time, never a signed/paused recording timestep.
};
enum class ChaseCameraStep { Inactive, Invalid, AwaitingNeutral, Duplicate, Updated };

struct ChaseCameraState {
    ChaseOrbit orbit;
    ChaseZoom zoom;
    bool armed{},active{};
    void Reset() { *this={}; }
    // Call also when device acquisition or the selected controller changes.
    // Preserve zoom on temporary suspension; a new camera/car resets it below.
    void Suspend() { armed=false;active=false;orbit.Reset(); }
    ChaseCameraStep Step(const ChaseCameraConfig& config,const ChaseCameraContext& context,const ChaseCameraInput& input) {
        if(!config.Valid()) { Suspend();return ChaseCameraStep::Invalid; }
        if(!config.enabled) { Reset();return ChaseCameraStep::Inactive; }
        if(!context.validOwner || !context.focused || !context.supportedState) {
            Suspend();return ChaseCameraStep::Inactive;
        }
        if(!context.camera || !context.target || !context.evaluation || !std::isfinite(context.dt) ||
            context.dt<=0 || context.dt>.25f || !std::isfinite(context.speedKmh)) {
            Suspend();return ChaseCameraStep::Invalid;
        }
        if(!haveIdentity || camera!=context.camera || target!=context.target || epoch!=context.epoch || replay!=context.replay ||
            (haveEvaluation && context.evaluation<evaluation)) {
            Reset();
            haveIdentity=true;camera=context.camera;target=context.target;epoch=context.epoch;replay=context.replay;
        }
        if(haveEvaluation && evaluation==context.evaluation)return ChaseCameraStep::Duplicate;
        evaluation=context.evaluation;haveEvaluation=true;
        if(haveDevices && (mouseAvailable!=input.mouseAvailable || stickAvailable!=input.stickAvailable))Suspend();
        mouseAvailable=input.mouseAvailable;stickAvailable=input.stickAvailable;haveDevices=true;
        ChaseCameraMotion motion;
        if(!DecodeChaseCameraInput(config,input,context.dt,motion)) { Suspend();return ChaseCameraStep::Invalid; }
        if(!armed) {
            // The neutral sample itself is discarded. A focus-restoring click,
            // wheel burst, held stick or R3 cannot become camera movement.
            armed=motion.neutral;active=false;
            return ChaseCameraStep::AwaitingNeutral;
        }
        const float speed=context.replay && !context.replayForward?0:context.speedKmh;
        orbit.Step(context.dt,speed,motion.horizontal,motion.vertical,motion.held,motion.rearHeld,config.keepAngle);
        zoom.Step(context.dt,motion.wheel,config.maxZoomOutPercent/100.f);
        active=true;
        return ChaseCameraStep::Updated;
    }
private:
    std::uint64_t camera{},target{},epoch{},evaluation{};
    bool haveIdentity{},haveEvaluation{},replay{},haveDevices{},mouseAvailable{},stickAvailable{};
};

struct ChaseVector { float x{},y{},z{}; };
struct ChaseQuaternion { float x{},y{},z{},w{1}; };
inline ChaseQuaternion ChaseMultiply(ChaseQuaternion a,ChaseQuaternion b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
inline ChaseVector ChaseRotate(ChaseQuaternion q,ChaseVector v) {
    const auto r=ChaseMultiply(ChaseMultiply(q,{v.x,v.y,v.z,0}),{-q.x,-q.y,-q.z,q.w});
    return {r.x,r.y,r.z};
}
inline bool ChaseUnitAxis(ChaseVector& axis) {
    const float norm=axis.x*axis.x+axis.y*axis.y+axis.z*axis.z;
    if(!std::isfinite(norm) || std::abs(norm-1.f)>.0001f)return false;
    const float inverse=1.f/std::sqrt(norm);
    axis.x*=inverse;axis.y*=inverse;axis.z*=inverse;
    return true;
}
// Hamilton q[x,y,z,w], then position xyz, then one untouched native word.
// The adapter must prove both signed axes and the pivot in native coordinates.
// Apply once to the fresh native pose, before its obstruction/visibility work.
// This function neither identifies that stage nor permits a second-eye write.
inline bool OrbitChasePose(std::array<float,8>& pose,ChaseVector pivot,ChaseVector worldYawAxis,
    ChaseVector localPitchAxis,float yaw,float pitch,float distanceScale=1,ChaseQuaternion* appliedRotation=nullptr) {
    for(float value:pose)if(!std::isfinite(value))return false;
    if(!std::isfinite(pivot.x) || !std::isfinite(pivot.y) || !std::isfinite(pivot.z) || !std::isfinite(yaw) ||
        !std::isfinite(pitch) || !std::isfinite(distanceScale) || distanceScale<.6f || distanceScale>10.f ||
        !ChaseUnitAxis(worldYawAxis) || !ChaseUnitAxis(localPitchAxis))return false;
    ChaseQuaternion q{pose[0],pose[1],pose[2],pose[3]};
    const float norm=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    if(!std::isfinite(norm) || std::abs(norm-1.f)>.01f)return false;
    ChaseVector relative{pose[4]-pivot.x,pose[5]-pivot.y,pose[6]-pivot.z};
    const float radius=relative.x*relative.x+relative.y*relative.y+relative.z*relative.z;
    if(!std::isfinite(radius) || radius<.25f || radius>2500.f)return false;
    if(yaw==0 && pitch==0 && distanceScale==1) {
        if(appliedRotation)*appliedRotation={};
        return true; // Neutral is byte-identical.
    }
    auto result=pose;
    ChaseQuaternion rotation;
    if(yaw!=0 || pitch!=0) {
        // Normalize only the working orientation used to find the pitch axis;
        // retain the native quaternion's accepted small norm error in output.
        const float inverse=1.f/std::sqrt(norm);
        const auto right=ChaseRotate({q.x*inverse,q.y*inverse,q.z*inverse,q.w*inverse},localPitchAxis);
        const float sy=std::sin(yaw*.5f),sp=std::sin(pitch*.5f);
        rotation=ChaseMultiply({worldYawAxis.x*sy,worldYawAxis.y*sy,worldYawAxis.z*sy,std::cos(yaw*.5f)},
            {right.x*sp,right.y*sp,right.z*sp,std::cos(pitch*.5f)});
        q=ChaseMultiply(rotation,q);relative=ChaseRotate(rotation,relative);
        result[0]=q.x;result[1]=q.y;result[2]=q.z;result[3]=q.w;
    }
    result[4]=pivot.x+relative.x*distanceScale;
    result[5]=pivot.y+relative.y*distanceScale;
    result[6]=pivot.z+relative.z*distanceScale;
    for(float value:result)if(!std::isfinite(value))return false;
    pose=result;
    if(appliedRotation)*appliedRotation=rotation;
    return true;
}
}
