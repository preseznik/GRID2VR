#include "instance_capacity_policy.h"
#include "instance_observation_policy.h"
#include <cstdio>
#include <limits>

#define CHECK(x) do {if(!(x)){std::printf("line %d: %s\n",__LINE__,#x);return 1;}} while(false)
using namespace grid2vr::instancecapacity;
int main() {
    AllocationObservationPolicy policy{true,true,true,true,true,false,false,false,false};
    CHECK(!policy.Suppression());
    for(auto field:{&AllocationObservationPolicy::requested,&AllocationObservationPolicy::runtime,
        &AllocationObservationPolicy::recordedViews,&AllocationObservationPolicy::unattended,&AllocationObservationPolicy::flicker}) {
        policy.*field=false;CHECK(policy.Suppression());policy.*field=true;
    }
    for(auto field:{&AllocationObservationPolicy::headset,&AllocationObservationPolicy::normal,
        &AllocationObservationPolicy::race,&AllocationObservationPolicy::playerControl}) {
        policy.*field=true;CHECK(policy.Suppression());policy.*field=false;
    }
    unsigned binds{},observations{};bool bindingResult=true,observationResult=true;
    auto bind=[&]{++binds;return bindingResult;};
    auto observe=[&]{++observations;return observationResult;};
    // All ordinary runs still qualify native resources with telemetry disabled.
    CHECK(QualifyReplayResources(true,false,bind,observe));CHECK(binds==1 && observations==0);
    bindingResult=false;
    CHECK(!QualifyReplayResources(true,false,bind,observe));CHECK(binds==2 && observations==0);
    CHECK(!QualifyReplayResources(true,true,bind,observe));CHECK(binds==3 && observations==0);
    bindingResult=true;
    CHECK(QualifyReplayResources(true,true,bind,observe));CHECK(binds==4 && observations==1);
    observationResult=false;
    CHECK(!QualifyReplayResources(true,true,bind,observe));CHECK(binds==5 && observations==2);
    CHECK(QualifyReplayResources(false,false,bind,observe));CHECK(binds==5 && observations==2);
    std::array<uint32_t,LaneCount> expected{};
    for(bool expanded:{false,true}) {
        CHECK(ExpectedCapacities(2,10,expanded,expected));
        CHECK(expected[0]==1800 && expected[14]==1800 && expected[2]==1500 && expected[10]==1600);
        CHECK(ExpectedCapacities(4,10,expanded,expected));
        CHECK(expected[0]==800 && expected[14]==800 && expected[2]==300 && expected[10]==350);
        CHECK(ExpectedCapacities(3,10,expanded,expected));
        CHECK(expected[0]==(expanded?4096u:1200u) && expected[14]==expected[0] && expected[10]==1000);
        CHECK(expected[1]==0 && expected[15]==400);
    }
    for(unsigned style:{0u,1u,5u,6u,7u,8u,9u,10u,UINT32_MAX})CHECK(!ExpectedCapacities(style,10,true,expected));
    CHECK(!ExpectedCapacities(2,9,true,expected));CHECK(!ExpectedCapacities(2,11,true,expected));
    CHECK(ReserveFits(1000,199,1200));CHECK(!ReserveFits(1000,200,1200));
    CHECK(!ReserveFits(1000,201,1200));CHECK(ReserveFits(0,MaxCapacity-1,MaxCapacity));
    CHECK(!ReserveFits(0,MaxCapacity,MaxCapacity));CHECK(!ReserveFits(-1,1,1200));
    CHECK(!ReserveFits(1,-1,1200));CHECK(!ReserveFits(0,0,0));
    CHECK(!ReserveFits(INT32_MAX,INT32_MAX,INT32_MAX));

    static RequestTotals totals;
    CHECK(totals.Add(10,0,1100));
    // A failed 200-instance native reservation still contributes to demand.
    CHECK(!ReserveFits(1100,200,1200));CHECK(totals.Add(10,0,200));
    CHECK(totals.MaxDemand(0)==1300);
    CHECK(totals.Add(11,0,900));CHECK(totals.Add(10,0,75));
    CHECK(totals.MaxDemand(0)==1375 && totals.FrameCount()==2);
    CHECK(totals.Add(11,15,500));CHECK(totals.MaxDemand(15)==500);
    CHECK(!totals.Add(12,-1,10));CHECK(!totals.Add(12,16,10));
    CHECK(!totals.Add(12,0,-1));CHECK(totals.FrameCount()==2);
    CHECK(totals.MaxDemand(0)==1375 && !totals.Truncated());
    CHECK(totals.Add(12,0,INT32_MAX));CHECK(totals.Add(12,0,INT32_MAX));
    CHECK(totals.MaxDemand(0)==2ull*INT32_MAX);CHECK(totals.MaxDemand(16)==0);
    totals.Reset();CHECK(totals.FrameCount()==0 && totals.MaxDemand(0)==0 && !totals.Truncated());
    for(size_t i=0;i<RequestTotals::MaxFrames;++i)CHECK(totals.Add(i,0,1));
    CHECK(totals.FrameCount()==RequestTotals::MaxFrames && !totals.Truncated());
    CHECK(!totals.Add(RequestTotals::MaxFrames,0,1));CHECK(totals.Truncated());
    CHECK(totals.FrameCount()==RequestTotals::MaxFrames && totals.MaxDemand(0)==1);
    CHECK(totals.Add(0,0,4));CHECK(totals.MaxDemand(0)==5 && totals.Truncated());
    totals.Reset();CHECK(totals.Add(0,0,0));CHECK(totals.FrameCount()==1 && !totals.Truncated());
    return 0;
}
