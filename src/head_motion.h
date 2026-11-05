#pragma once
#include <openxr/openxr.h>
#include <cmath>
#include <istream>
#include <string_view>

namespace grid2vr::headmotion {
// Diagnostic only (ProJict c9c93f6f). SteamVR's virtual headset never moves, so an
// unattended fake-VR run can request a deterministic look-around sweep. The session
// refuses the request unless the headset is SteamVR's built-in null driver.
struct Sweep { float yaw{},pitch{},period{}; }; // peak degrees, peak degrees, seconds

// One line: "<yaw> <pitch> <period>". Yaw 0..60, pitch 0..30, period 2..60, some motion.
inline bool Read(std::istream& input,Sweep& sweep) {
    Sweep candidate;char extra{};
    if (!(input>>candidate.yaw>>candidate.pitch>>candidate.period) || (input>>extra)) return false;
    if (!(candidate.yaw>=0 && candidate.yaw<=60 && candidate.pitch>=0 && candidate.pitch<=30 &&
          candidate.period>=2 && candidate.period<=60 && candidate.yaw+candidate.pitch>0)) return false;
    sweep=candidate;return true;
}

// SteamVR's OpenXR runtime names its system "SteamVR/OpenXR : <tracking system>";
// the built-in null (virtual) driver is "null". Any real headset differs. (Whether the
// null driver names a graphics adapter varies, so the adapter is no marker.)
inline bool VirtualHeadset(std::string_view runtime,std::string_view system) {
    const auto separator=system.rfind(':');
    if (runtime.substr(0,8)!="SteamVR/" || system.substr(0,8)!="SteamVR/" || separator==std::string_view::npos) return false;
    auto name=system.substr(separator+1);
    while (!name.empty() && name.front()==' ') name.remove_prefix(1);
    while (!name.empty() && name.back()==' ') name.remove_suffix(1);
    return name.size()==4 && (name[0]|32)=='n' && (name[1]|32)=='u' && (name[2]|32)=='l' && (name[3]|32)=='l';
}

// Yaw about LOCAL up swings left and right once per period; pitch nods twice, so
// the head passes every direction of the sweep at different speeds.
inline XrQuaternionf Rotation(const Sweep& sweep,double seconds) {
    constexpr double pi=3.14159265358979323846,radians=pi/180;
    const double phase=2*pi*seconds/sweep.period;
    const double yaw=sweep.yaw*radians*std::sin(phase),pitch=sweep.pitch*radians*std::sin(2*phase);
    const double sy=std::sin(yaw/2),cy=std::cos(yaw/2),sp=std::sin(pitch/2),cp=std::cos(pitch/2);
    // yaw(Y) * pitch(X)
    return {static_cast<float>(cy*sp),static_cast<float>(sy*cp),static_cast<float>(-sy*sp),static_cast<float>(cy*cp)};
}

inline XrQuaternionf Multiply(const XrQuaternionf& a,const XrQuaternionf& b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}

inline XrVector3f Rotate(const XrQuaternionf& q,const XrVector3f& v) {
    // v + 2w(q x v) + 2 q x (q x v)
    const XrVector3f t{2*(q.y*v.z-q.z*v.y),2*(q.z*v.x-q.x*v.z),2*(q.x*v.y-q.y*v.x)};
    return {v.x+q.w*t.x+(q.y*t.z-q.z*t.y),v.y+q.w*t.y+(q.z*t.x-q.x*t.z),v.z+q.w*t.z+(q.x*t.y-q.y*t.x)};
}

// Turns a located pose about the head centre, as a real head turn would; both eyes
// and the head get the same rotation, so eye separation and FOVs are unchanged.
inline void Apply(const XrQuaternionf& rotation,const XrVector3f& centre,XrPosef& pose) {
    pose.orientation=Multiply(rotation,pose.orientation);
    const XrVector3f offset{pose.position.x-centre.x,pose.position.y-centre.y,pose.position.z-centre.z};
    const auto turned=Rotate(rotation,offset);
    pose.position={centre.x+turned.x,centre.y+turned.y,centre.z+turned.z};
}
}
