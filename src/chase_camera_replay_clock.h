#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace grid2vr::chasecamera {
struct ReplayClockSample {
    // Native graph identity is qualified separately. This helper only compares
    // opaque identities and read-only clock values; it never writes recorded data.
    std::array<uint32_t,13> identity{};
    double time{},seekTarget{},step{},rate{};
    bool operator==(const ReplayClockSample&) const=default;
};
struct ReplayClockResult {bool valid{},reset{},forward{};uint64_t epoch{};};
class ReplayClock {
public:
    ReplayClockResult Observe(const ReplayClockSample& sample,uint64_t generation,double elapsed) {
        if(!generation || !std::isfinite(elapsed) || elapsed<=0 || elapsed>.25 ||
           !std::isfinite(sample.time) || std::abs(sample.time)>1e9 ||
           !std::isfinite(sample.seekTarget) || std::abs(sample.seekTarget)>1e9 ||
           !std::isfinite(sample.step) || std::abs(sample.step)>.25 ||
           !std::isfinite(sample.rate) || std::abs(sample.rate)>64) {Leave();return {};}
        bool reset=!active_ || sample.identity!=last_.identity || sample.seekTarget!=last_.seekTarget || generation<generation_;
        const double change=sample.time-last_.time;
        if(active_ && !reset) {
            // Two native samples of tolerance plus twice the expected travel.
            // This is a conservative discontinuity policy, not a native constant.
            const double travel=2*std::max(std::abs(sample.step),elapsed)*std::max(std::abs(sample.rate),std::abs(last_.rate))+2./30;
            const bool wrongDirection=(sample.rate>0 && last_.rate>0 && change < -2./30) ||
                (sample.rate<0 && last_.rate<0 && change > 2./30);
            reset=std::abs(change)>travel || wrongDirection;
        }
        if(reset)++epoch_;
        last_=sample;generation_=generation;active_=true;
        // Hold and rewind never trigger automatic return. The adapter also
        // checks supported UI mode and endpoint state when setting rate.
        return {true,reset,sample.rate>0 && sample.step>0,epoch_};
    }
    void Leave() {if(active_)++epoch_;active_=false;last_={};generation_=0;}
    uint64_t Epoch()const {return epoch_;}
private:
    ReplayClockSample last_;
    uint64_t generation_{},epoch_{};
    bool active_{};
};
}
