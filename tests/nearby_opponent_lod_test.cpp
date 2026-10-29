#include "nearby_opponent_lod.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>

#define CHECK(x) do {if(!(x)){std::printf("line %d: %s\n",__LINE__,#x);return false;}} while(false)
namespace lod=grid2vr::opponentlod;

namespace {
struct Fixture {
    lod::Context context{};
    std::array<lod::Car,lod::MaxCars> cars{};
    std::size_t count{1};
    Fixture() {
        context.sessionEpoch=1;context.manager=10;context.managerEpoch=1;context.sample=1;
        context.player=100;context.currentTarget=101;
        context.profile=2;context.mainViewCount=1;
        context.requested=context.sessionActive=context.vrActive=context.mainView=context.race=true;
        context.identitiesValidated=true;
        for(std::size_t i=0;i<cars.size();++i) {
            auto& car=cars[i];
            car.identity=200+i;car.epoch=1;car.bestMesh=300+i;
            car.position={0,0,20};car.best=0;car.worst=4;car.selected=4;
            car.opponent=car.bestMeshLoaded=true;
        }
    }
    lod::Frame Frame() const {return {context,{cars.data(),count}};}
    lod::Plan Evaluate(const lod::State& state={}) const {return lod::Evaluate(Frame(),state);}
};

bool GeometryAndThresholds() {
    Fixture f;
    for(const lod::Position position:std::array<lod::Position,7>{{
        {0,0,30},{30,0,0},{0,0,-30},{-30,0,0},{0,30,0},{0,-30,0},{18,24,0}}}) {
        f.cars[0].position=position;
        const auto plan=f.Evaluate();
        CHECK(plan.Refused()==lod::Refusal::None);
        CHECK(plan.Changes().size()==1 && plan.Changes()[0].selected==0);
        CHECK(plan.Stats().nearby==1);
    }
    f.cars[0].position={0,0,std::nextafter(30.0,31.0)};
    CHECK(f.Evaluate().Changes().empty());
    f.cars[0].position={0,0,30};
    auto state=f.Evaluate().NextState();
    for(const double distance:{35.0,40.0}) {
        f.cars[0].position={0,0,distance};
        const auto plan=f.Evaluate(state);state=plan.NextState();
        CHECK(plan.Changes().size()==1);
    }
    f.cars[0].position={0,0,std::nextafter(40.0,41.0)};
    auto plan=f.Evaluate(state);state=plan.NextState();
    CHECK(plan.Changes().empty());
    CHECK(plan.Stats().Count(lod::Result::Distant)==1);
    f.cars[0].position={0,0,40};
    CHECK(f.Evaluate(state).Changes().empty());
    f.cars[0].position={0,0,30};
    CHECK(f.Evaluate(state).Changes().size()==1);
    // The camera translation, not the world origin, is the distance reference.
    f.context.camera={100,-100,200};f.cars[0].position={118,-76,200};
    CHECK(f.Evaluate().Changes().size()==1);
    // All three components matter: individually <=30 does not mean near.
    f.context.camera={};f.cars[0].position={30,30,30};
    CHECK(f.Evaluate().Changes().empty());
    return true;
}

bool GatesAndReset() {
    const Fixture base;
    const auto near=base.Evaluate().NextState();
    auto reject=[&](const Fixture& invalid,lod::Refusal reason) {
        const auto plan=invalid.Evaluate(near);
        if(plan.Refused()!=reason || !plan.Changes().empty() || plan.Matches(invalid.Frame()))return false;
        if(plan.Stats().inspected || plan.Stats().nearby)return false;
        Fixture distant=base;distant.cars[0].position={0,0,35};
        return distant.Evaluate(plan.NextState()).Changes().empty();
    };
    Fixture f=base;f.context.requested=false;CHECK(reject(f,lod::Refusal::Disabled));
    f=base;f.context.stopped=true;CHECK(reject(f,lod::Refusal::Stopped));
    f=base;f.context.sessionActive=false;CHECK(reject(f,lod::Refusal::InactiveSession));
    f=base;f.context.vrActive=false;CHECK(reject(f,lod::Refusal::InactiveVr));
    f=base;f.context.mainView=false;CHECK(reject(f,lod::Refusal::WrongView));
    for(unsigned count:{0u,2u,12u}) {
        f=base;f.context.mainViewCount=count;CHECK(reject(f,lod::Refusal::WrongView));
    }
    f=base;f.context.race=false;CHECK(reject(f,lod::Refusal::NotRace));
    for(int profile:{-1,3,4,5,6,7}) {
        f=base;f.context.profile=profile;CHECK(reject(f,lod::Refusal::UnsupportedProfile));
    }
    for(int profile=0;profile<=2;++profile) {
        f=base;f.context.profile=profile;CHECK(f.Evaluate().Changes().size()==1);
    }
    for(int forced:{-2,0,1,2,3,4,5}) {
        f=base;f.context.forcedLod=forced;CHECK(reject(f,lod::Refusal::ForcedSelection));
    }
    f=base;f.context.identitiesValidated=false;CHECK(reject(f,lod::Refusal::InvalidIdentity));
    for(auto field:{&lod::Context::sessionEpoch,&lod::Context::manager,
        &lod::Context::managerEpoch,&lod::Context::sample,&lod::Context::player}) {
        f=base;f.context.*field=0;CHECK(reject(f,lod::Refusal::InvalidIdentity));
    }
    std::array<lod::Car,13> tooMany{};
    const auto oversized=lod::Evaluate({base.context,tooMany},near);
    CHECK(oversized.Refused()==lod::Refusal::TooManyCars && oversized.Changes().empty());
    f=base;f.count=2;f.cars[1].identity=f.cars[0].identity;
    CHECK(reject(f,lod::Refusal::DuplicateCar));
    // Different epochs do not authorize two records referring to the same car.
    f.cars[1].epoch=2;CHECK(reject(f,lod::Refusal::DuplicateCar));
    f=base;f.cars[0].position={0,0,35};
    CHECK(f.Evaluate(lod::State{}).Changes().empty());
    f.count=0;const auto empty=f.Evaluate(near);
    CHECK(empty.Refused()==lod::Refusal::None && empty.Changes().empty());
    f.count=1;CHECK(f.Evaluate(empty.NextState()).Changes().empty());
    return true;
}

bool FloatingPoint() {
    Fixture f;
    const auto near=f.Evaluate().NextState();
    for(double bad:{std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()}) {
        for(auto component:{&lod::Position::x,&lod::Position::y,&lod::Position::z}) {
            f=Fixture{};f.context.camera.*component=bad;
            const auto camera=f.Evaluate(near);
            CHECK(camera.Refused()==lod::Refusal::InvalidCamera && camera.Changes().empty());
            f=Fixture{};f.cars[0].position.*component=bad;
            const auto car=f.Evaluate(near);
            CHECK(car.Refused()==lod::Refusal::InvalidPosition && car.Changes().empty());
            f=Fixture{};f.cars[0].position={0,0,35};
            CHECK(f.Evaluate(camera.NextState()).Changes().empty());
            CHECK(f.Evaluate(car.NextState()).Changes().empty());
        }
    }
    const double floatMax=std::numeric_limits<float>::max();
    f=Fixture{};f.context.camera={-floatMax,0,0};f.cars[0].position={floatMax,floatMax,floatMax};
    auto plan=f.Evaluate();
    CHECK(plan.Changes().empty() && plan.Stats().Count(lod::Result::Distant)==1);
    // Squaring would overflow; hypot still produces a finite, distant norm.
    f.context.camera={};f.cars[0].position={1e200,1e200,1e200};
    plan=f.Evaluate();CHECK(plan.Stats().Count(lod::Result::Distant)==1);
    const double max=std::numeric_limits<double>::max();
    f.context.camera={-max,0,0};f.cars[0].position={max,0,0};
    plan=f.Evaluate(near);
    CHECK(plan.Changes().empty() && plan.Stats().Count(lod::Result::InvalidDistance)==1);
    f.context.camera={};f.cars[0].position={max,max,max};
    plan=f.Evaluate(near);
    CHECK(plan.Changes().empty() && plan.Stats().Count(lod::Result::InvalidDistance)==1);
    f.cars[0].position={0,0,35};CHECK(f.Evaluate(plan.NextState()).Changes().empty());
    f.cars[0].position={-0.0,0,0};CHECK(f.Evaluate().Changes().size()==1);
    return true;
}

bool BoundsAndExclusions() {
    Fixture f;
    // All five indices and every valid available range; never force an
    // unavailable higher-detail mesh or change a better/special native choice.
    for(int best=0;best<=4;++best)for(int worst=best;worst<=4;++worst)
        for(int selected=-2;selected<=6;++selected) {
            f=Fixture{};f.cars[0].best=best;f.cars[0].worst=worst;f.cars[0].selected=selected;
            const auto plan=f.Evaluate();
            const bool change=selected>best && selected<=worst;
            CHECK(plan.Changes().size()==static_cast<std::size_t>(change));
            CHECK(plan.Stats().inspected==1);
            if(change)CHECK(plan.Changes()[0].selected==best && plan.Changes()[0].expected==selected);
            else CHECK(plan.Stats().Count(selected<0 || selected>worst?
                lod::Result::UnsupportedSelection:lod::Result::NativeBestOrBetter)==1);
        }
    for(const auto bounds:std::array<std::array<int,2>,7>{{
        {-1,4},{0,5},{3,2},{5,5},{-1,-1},{4,3},{0,-1}}}) {
        f=Fixture{};f.cars[0].best=bounds[0];f.cars[0].worst=bounds[1];
        const auto plan=f.Evaluate();
        CHECK(plan.Changes().empty() && plan.Stats().Count(lod::Result::InvalidBounds)==1);
    }
    f=Fixture{};f.count=5;
    f.cars[0].identity=f.context.player;
    f.cars[1].identity=f.context.currentTarget;
    f.cars[2].opponent=false;
    f.cars[3].bestMeshLoaded=false;
    f.cars[4].bestMesh=0;
    auto plan=f.Evaluate();
    CHECK(plan.Changes().empty() && plan.Stats().Count(lod::Result::Excluded)==3);
    CHECK(plan.Stats().Count(lod::Result::InvalidBestMesh)==2);
    f=Fixture{};f.count=2;f.cars[0].identity=0;f.cars[1].epoch=0;
    plan=f.Evaluate();
    CHECK(plan.Changes().empty() && plan.Stats().Count(lod::Result::InvalidIdentity)==2);
    // No target is an explicitly representable valid state, not a wildcard.
    f=Fixture{};f.context.currentTarget=0;CHECK(f.Evaluate().Changes().size()==1);
    // Already-best native output must still seed proximity hysteresis.
    f.cars[0].selected=0;plan=f.Evaluate();CHECK(plan.Changes().empty());
    f.cars[0].selected=4;f.cars[0].position={0,0,35};
    CHECK(f.Evaluate(plan.NextState()).Changes().size()==1);
    return true;
}

bool IdentityAndLifecycle() {
    Fixture base;
    const auto near=base.Evaluate().NextState();
    base.cars[0].position={0,0,35};
    CHECK(base.Evaluate(near).Changes().size()==1);
    for(auto field:{&lod::Context::sessionEpoch,&lod::Context::manager,&lod::Context::managerEpoch}) {
        Fixture f=base;++(f.context.*field);CHECK(f.Evaluate(near).Changes().empty());
    }
    for(auto field:{&lod::Car::identity,&lod::Car::epoch,&lod::Car::bestMesh}) {
        Fixture f=base;++(f.cars[0].*field);CHECK(f.Evaluate(near).Changes().empty());
    }
    for(int profile:{0,1}) {
        Fixture f=base;f.context.profile=profile;CHECK(f.Evaluate(near).Changes().empty());
    }
    Fixture f=base;f.cars[0].best=1;CHECK(f.Evaluate(near).Changes().empty());
    f=base;f.cars[0].worst=3;f.cars[0].selected=3;CHECK(f.Evaluate(near).Changes().empty());
    // Becoming ineligible clears only that slot, not its eligible neighbour.
    for(int reason=0;reason<7;++reason) {
        f=Fixture{};f.count=2;const auto both=f.Evaluate().NextState();
        if(reason==0)f.cars[0].opponent=false;
        if(reason==1)f.cars[0].bestMeshLoaded=false;
        if(reason==2)f.cars[0].selected=5;
        if(reason==3)f.cars[0].best=-1;
        if(reason==4)f.cars[0].epoch=0;
        if(reason==5)f.cars[0].identity=f.context.player;
        if(reason==6)f.cars[0].identity=f.context.currentTarget;
        const auto invalid=f.Evaluate(both).NextState();
        f=Fixture{};f.count=2;f.cars[0].position=f.cars[1].position={0,0,35};
        const auto plan=f.Evaluate(invalid);
        CHECK(plan.Changes().size()==1 && plan.Changes()[0].slot==1);
    }
    f=Fixture{};f.count=12;auto all=f.Evaluate();
    CHECK(all.Changes().size()==12 && all.Stats().inspected==12 && all.Stats().nearby==12);
    for(std::size_t i=0;i<12;++i)CHECK(all.Changes()[i].slot==i);
    const auto allState=all.NextState();
    for(auto& car:f.cars)car.position={0,0,35};
    std::swap(f.cars[0],f.cars[11]);all=f.Evaluate(allState);
    CHECK(all.Changes().size()==10); // slot identity swap resets both ends
    f=Fixture{};f.count=11;const auto fewer=f.Evaluate(allState);
    f.count=12;for(auto& car:f.cars)car.position={0,0,35};
    all=f.Evaluate(fewer.NextState());
    CHECK(all.Changes().size()==11 && all.Changes().back().slot==10);
    return true;
}

// Deliberately synthetic, unrelated to GRID's record stride or field offsets.
// Only this test adapter writes bytes; the policy itself never does.
struct SyntheticRecord {
    std::array<unsigned char,13> prefix;
    std::int32_t selected;
    std::array<unsigned char,19> suffix;
};
static_assert(std::is_trivially_copyable_v<SyntheticRecord>);
using Records=std::array<SyntheticRecord,lod::MaxCars>;
using RecordBytes=std::array<unsigned char,sizeof(Records)>;

RecordBytes Bytes(const Records& records) {
    RecordBytes bytes{};
    std::memcpy(bytes.data(),records.data(),bytes.size());
    return bytes;
}

bool ApplySynthetic(const lod::Plan& plan,const lod::Frame& fresh,bool authorized,Records& records) {
    if(!authorized || !plan.Matches(fresh))return false;
    // Complete preflight precedes ALL writes; no partial application on stale
    // selection values. A real adapter still needs its separately audited gate.
    for(const auto& change:plan.Changes())
        if(change.slot>=fresh.cars.size() || records[change.slot].selected!=change.expected)return false;
    for(const auto& change:plan.Changes())
        std::memcpy(&records[change.slot].selected,&change.selected,sizeof(change.selected));
    return true;
}

bool ByteBoundariesAndFreshness() {
    Fixture f;f.count=12;
    f.cars[0].identity=f.context.player;
    f.cars[1].identity=f.context.currentTarget;
    f.cars[2].position={0,0,41};
    f.cars[3].selected=0;
    f.cars[4].selected=5; // native special choice
    f.cars[5].bestMeshLoaded=false;
    f.cars[6].best=1;f.cars[6].selected=0; // native choice better than available best
    const Fixture inputBefore=f;
    const auto plan=f.Evaluate();
    CHECK(plan.Changes().size()==5);
    CHECK(f.context==inputBefore.context && f.cars==inputBefore.cars && f.count==inputBefore.count);
    Records records;
    std::memset(records.data(),0xa5,sizeof(records));
    for(std::size_t i=0;i<records.size();++i)records[i].selected=f.cars[i].selected;
    // Capture object representations explicitly, including any struct padding.
    const auto original=Bytes(records);
    CHECK(plan.Matches(inputBefore.Frame()));
    CHECK(!ApplySynthetic(plan,f.Frame(),false,records));
    CHECK(Bytes(records)==original);
    CHECK(ApplySynthetic(plan,f.Frame(),true,records));
    auto expected=original;
    const std::int32_t best=0;
    for(std::size_t i=7;i<12;++i)
        std::memcpy(expected.data()+i*sizeof(SyntheticRecord)+offsetof(SyntheticRecord,selected),
            &best,sizeof(best));
    const auto after=Bytes(records);
    CHECK(after==expected);
    for(std::size_t byte=0;byte<sizeof(records);++byte) {
        bool allowed=false;
        for(const auto& change:plan.Changes()) {
            const auto start=change.slot*sizeof(SyntheticRecord)+offsetof(SyntheticRecord,selected);
            if(byte>=start && byte<start+sizeof(std::int32_t))allowed=true;
        }
        if(!allowed)CHECK(original[byte]==after[byte]);
    }
    // Last-edit mismatch refuses atomically rather than partly writing the rest.
    std::memcpy(records.data(),original.data(),original.size());
    records[11].selected=3;const auto stale=Bytes(records);
    CHECK(!ApplySynthetic(plan,f.Frame(),true,records));
    CHECK(Bytes(records)==stale);
    auto unchanged=[&](const Fixture& fresh) {
        std::memcpy(records.data(),original.data(),original.size());
        return !ApplySynthetic(plan,fresh.Frame(),true,records) && Bytes(records)==original;
    };
    Fixture changed=f;changed.context.sample++;CHECK(unchanged(changed));
    changed=f;changed.context.stopped=true;CHECK(unchanged(changed));
    changed=f;changed.context.requested=false;CHECK(unchanged(changed));
    changed=f;changed.context.managerEpoch++;CHECK(unchanged(changed));
    changed=f;changed.context.profile=1;CHECK(unchanged(changed));
    changed=f;changed.context.forcedLod=0;CHECK(unchanged(changed));
    changed=f;changed.context.mainViewCount=2;CHECK(unchanged(changed));
    changed=f;changed.context.camera.x=1;CHECK(unchanged(changed));
    changed=f;changed.count=11;CHECK(unchanged(changed));
    changed=f;changed.cars[11].selected=3;CHECK(unchanged(changed));
    changed=f;changed.cars[11].bestMesh++;CHECK(unchanged(changed));
    changed=f;changed.cars[11].epoch++;CHECK(unchanged(changed));
    changed=f;changed.cars[11].identity++;CHECK(unchanged(changed));
    changed=f;changed.cars[11].position.z=25;CHECK(unchanged(changed));
    changed=f;changed.cars[11].bestMeshLoaded=false;CHECK(unchanged(changed));
    changed=f;changed.cars[11].opponent=false;CHECK(unchanged(changed));
    // Refused plans never authorize application, even against their own input.
    changed=f;changed.context.stopped=true;const auto stopped=changed.Evaluate();
    std::memcpy(records.data(),original.data(),original.size());
    CHECK(!ApplySynthetic(stopped,changed.Frame(),true,records));
    CHECK(Bytes(records)==original);
    return true;
}
} // namespace

int main() {
    if(!GeometryAndThresholds() || !GatesAndReset() || !FloatingPoint() ||
       !BoundsAndExclusions() || !IdentityAndLifecycle() || !ByteBoundariesAndFreshness())return 1;
    std::puts("nearby opponent LOD policy tests passed (synthetic only)");
    return 0;
}
