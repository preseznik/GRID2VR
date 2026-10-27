#undef NDEBUG
#include "chase_camera_replay_clock.h"
#include <cassert>
#include <limits>
using namespace grid2vr::chasecamera;
int main() {
    ReplayClock clock;ReplayClockSample sample;
    sample.identity.fill(0x10000);sample.time=10;sample.step=1./60;sample.rate=1;
    auto result=clock.Observe(sample,1,1./60);assert(result.valid && result.reset && result.forward);
    const auto epoch=result.epoch;
    result=clock.Observe(sample,1,1./60);assert(result.valid && !result.reset && result.epoch==epoch);
    for(uint64_t frame=2;frame<60;++frame) {
        sample.time+=sample.step;result=clock.Observe(sample,frame,1./60);
        assert(result.valid && !result.reset && result.forward && result.epoch==epoch);
    }
    sample.rate=0;sample.step=0;
    for(uint64_t frame=60;frame<120;++frame) {
        result=clock.Observe(sample,frame,1./60);assert(result.valid && !result.reset && !result.forward);
    }
    // Rewind advances input time but preserves the orbit's epoch.
    sample.rate=-4;sample.step=1./60;
    for(uint64_t frame=120;frame<180;++frame) {
        sample.time-=4./60;result=clock.Observe(sample,frame,1./60);
        assert(result.valid && !result.reset && !result.forward && result.epoch==epoch);
    }
    // A seek resets even if its temporary native mode was missed between polls.
    sample.seekTarget=sample.time;
    result=clock.Observe(sample,180,1./60);assert(result.valid && result.reset && result.epoch==epoch+1);
    sample.time+=20;result=clock.Observe(sample,181,1./60);assert(result.valid && result.reset);
    for(size_t field=0;field<sample.identity.size();++field) {
        ++sample.identity[field];result=clock.Observe(sample,182+field,1./60);assert(result.valid && result.reset);
    }
    clock.Leave();result=clock.Observe(sample,200,1./60);assert(result.valid && result.reset);
    result=clock.Observe(sample,199,1./60);assert(result.valid && result.reset);
    for(double invalid:{std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
        auto bad=sample;bad.time=invalid;assert(!clock.Observe(bad,201,1./60).valid);
        bad=sample;bad.rate=invalid;assert(!clock.Observe(bad,201,1./60).valid);
        bad=sample;bad.seekTarget=invalid;assert(!clock.Observe(bad,201,1./60).valid);
        bad=sample;bad.step=invalid;assert(!clock.Observe(bad,201,1./60).valid);
        assert(!clock.Observe(sample,201,invalid).valid);
    }
    return 0;
}
