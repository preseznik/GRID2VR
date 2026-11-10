#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <istream>

namespace grid2vr::forcefeedback {
// Launcher Force Feedback (ProJict 80ae223a). GRID 2 drives a wheel with one
// DirectInput constant-force effect and updates its magnitude about once per
// frame (IDirectInputEffect::SetParameters, live run 20261110-103514-498): the
// aligning torque, kerbs, bumps and impacts arrive already mixed. The launcher
// shapes that magnitude on its way into dinput8; no game code or data is written.
//
// The signal is split by a one-pole low-pass: the slow part is the wheel's
// weight (aligning torque, load), the fast remainder is vibration (kerbs, road
// surface, impacts). Output = strength x (weight x slow + vibration x fast),
// clamped to DirectInput's nominal range. 100/100/100 passes the game's force
// through unchanged.
struct Settings {
    int strength=100; // percent, 0..100
    int weight=100;   // percent, 1..100
    int effects=100;  // percent, 0..100 (launcher "Vibration")
    bool operator==(const Settings&) const=default;
};
inline bool ValidSettings(const Settings& value) {
    return value.strength>=0 && value.strength<=100 && value.weight>=1 && value.weight<=100 && value.effects>=0 && value.effects<=100;
}
// force-feedback.request: "<strength> <weight> <effects>" whole percentages.
inline bool ReadRequest(std::istream& input,Settings& result) {
    Settings candidate;
    if(!(input>>candidate.strength>>candidate.weight>>candidate.effects))return false;
    input>>std::ws;
    if(!input.eof() || !ValidSettings(candidate))return false;
    result=candidate;return true;
}

inline constexpr long nominalMax=10000;     // DI_FFNOMINALMAX
inline constexpr double splitHz=3.0;        // weight below, vibration above
inline constexpr double minStep=0.001,maxStep=0.1,restartGap=0.25; // seconds

class Shaper {
public:
    explicit Shaper(Settings value={}):settings(value) {}
    const Settings& Current() const {return settings;}
    bool Neutral() const {return settings==Settings{};}
    void Reset() {primed=false;}
    // magnitude: the game's DICONSTANTFORCE.lMagnitude; seconds: time since the
    // previous update of this effect. A first update, or one after a pause
    // (menus, loading), restarts the split at the current value.
    long Apply(long magnitude,double seconds) {
        const double x=static_cast<double>(magnitude);
        if(!primed || !(seconds>0) || seconds>restartGap) {slow=x;primed=true;}
        else {
            const double step=std::clamp(seconds,minStep,maxStep);
            slow+=(1.0-std::exp(-2.0*3.14159265358979323846*splitHz*step))*(x-slow);
        }
        if(Neutral()) return magnitude;
        const double shaped=settings.strength/100.0*(settings.weight/100.0*slow+settings.effects/100.0*(x-slow));
        return static_cast<long>(std::lround(std::clamp(shaped,-static_cast<double>(nominalMax),static_cast<double>(nominalMax))));
    }
private:
    Settings settings;
    double slow{};
    bool primed{};
};
}
