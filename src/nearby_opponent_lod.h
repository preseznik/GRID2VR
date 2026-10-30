#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace grid2vr::opponentlod {
inline constexpr std::size_t MaxCars=12;
inline constexpr std::int32_t MaxLod=4;
// Policy in native world units, NOT a measured metres claim for GRID.
inline constexpr double EnterRadius=30.0,LeaveRadius=40.0;

struct Position {
    double x{},y{},z{};
    bool operator==(const Position&) const=default;
};

// All identities are opaque caller-owned tokens, never dereferenced addresses.
// Epochs must change on lifetime reuse, including reuse of the same address.
// sample must identify the exact native selection batch (not just a VR eye).
struct Context {
    std::uint64_t sessionEpoch{},manager{},managerEpoch{},sample{};
    std::uint64_t player{},currentTarget{}; // zero target means proven absent
    std::int32_t profile{-1},forcedLod{-1}; // only -1 means unforced
    unsigned mainViewCount{};
    bool requested{},sessionActive{},vrActive{},mainView{},race{};
    bool identitiesValidated{},stopped{};
    Position camera{};
    bool operator==(const Context&) const=default;
};

struct Car {
    std::uint64_t identity{},epoch{},bestMesh{};
    Position position{};
    std::int32_t best{},worst{},selected{};
    bool opponent{},bestMeshLoaded{};
    bool operator==(const Car&) const=default;
};

struct Frame {
    Context context{};
    std::span<const Car> cars{};
};

enum class Refusal {
    None,Disabled,Stopped,InactiveSession,InactiveVr,WrongView,NotRace,
    UnsupportedProfile,ForcedSelection,InvalidIdentity,TooManyCars,
    InvalidCamera,InvalidPosition,DuplicateCar
};
enum class Result : std::size_t {
    Adjusted,NativeBestOrBetter,Distant,Excluded,InvalidIdentity,
    InvalidBounds,InvalidBestMesh,UnsupportedSelection,InvalidDistance,Count
};
struct Statistics {
    std::size_t inspected{},nearby{};
    std::array<std::size_t,static_cast<std::size_t>(Result::Count)> results{};
    std::size_t Count(Result result) const {
        const auto index=static_cast<std::size_t>(result);
        return index<results.size()?results[index]:0;
    }
};
struct Change {
    std::size_t slot{};
    std::int32_t expected{},selected{};
};

class Plan;
class State;
inline Plan Evaluate(const Frame&,const State&);

// Fixed native slots: moving/replacing a car in a slot deliberately loses its
// hysteresis. Missing/invalid/excluded slots and any refused frame reset state.
class State {
    friend Plan Evaluate(const Frame&,const State&);
    struct Slot {
        std::uint64_t car{},epoch{},mesh{};
        std::int32_t best{},worst{};
        bool nearby{};
    };
    std::uint64_t sessionEpoch_{},manager_{},managerEpoch_{};
    std::int32_t profile_{-1};
    std::array<Slot,MaxCars> slots_{};
};

// Immutable through its public API. Contains only values, no native pointers,
// callbacks, allocations, memory writes, installation or hook assumptions.
class Plan {
public:
    Refusal Refused() const {return refusal_;}
    const Statistics& Stats() const {return stats_;}
    const State& NextState() const {return next_;}
    std::span<const Change> Changes() const {return {changes_.data(),changeCount_};}

    // A freshness/value check, NOT authorization to touch native memory. The
    // adapter must independently prove native body safety/ownership, synchronize,
    // recapture this exact batch, and preflight every write before applying any.
    bool Matches(const Frame& current) const {
        if(refusal_!=Refusal::None || current.context!=context_ ||
           current.cars.size()!=carCount_)return false;
        for(std::size_t i=0;i<carCount_;++i)
            if(current.cars[i]!=cars_[i])return false;
        return true;
    }
private:
    friend Plan Evaluate(const Frame&,const State&);
    Plan()=default;
    Refusal refusal_{Refusal::None};
    Statistics stats_{};
    State next_{};
    Context context_{};
    std::array<Car,MaxCars> cars_{};
    std::array<Change,MaxCars> changes_{};
    std::size_t carCount_{},changeCount_{};
};

inline bool Finite(const Position& position) {
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
}

// Pure bounded policy. Integrators must capture the independently validated GRID
// normal world position (not a DiRT offset, inflated metric, or replay transform).
// This never alters profile settings, frusta, meshes, or applied native LODs.
// Caller commits NextState after evaluation; explicit reset is State{}.
inline Plan Evaluate(const Frame& frame,const State& previous) {
    Plan plan;
    const auto& context=frame.context;
    auto refuse=[&](Refusal reason) {plan.refusal_=reason;return plan;};
    if(!context.requested)return refuse(Refusal::Disabled);
    if(context.stopped)return refuse(Refusal::Stopped);
    if(!context.sessionActive)return refuse(Refusal::InactiveSession);
    if(!context.vrActive)return refuse(Refusal::InactiveVr);
    if(!context.mainView || context.mainViewCount!=1)return refuse(Refusal::WrongView);
    if(!context.race)return refuse(Refusal::NotRace);
    if(context.profile<0 || context.profile>2)return refuse(Refusal::UnsupportedProfile);
    if(context.forcedLod!=-1)return refuse(Refusal::ForcedSelection);
    if(!context.identitiesValidated || !context.player || !context.sessionEpoch ||
       !context.manager || !context.managerEpoch || !context.sample)
        return refuse(Refusal::InvalidIdentity);
    if(frame.cars.size()>MaxCars)return refuse(Refusal::TooManyCars);
    if(!Finite(context.camera))return refuse(Refusal::InvalidCamera);
    for(std::size_t i=0;i<frame.cars.size();++i) {
        if(!Finite(frame.cars[i].position))return refuse(Refusal::InvalidPosition);
        for(std::size_t j=0;j<i;++j)
            if(frame.cars[i].identity && frame.cars[i].identity==frame.cars[j].identity)
                return refuse(Refusal::DuplicateCar);
    }

    plan.context_=context;
    plan.carCount_=frame.cars.size();
    auto& next=plan.next_;
    next.sessionEpoch_=context.sessionEpoch;
    next.manager_=context.manager;
    next.managerEpoch_=context.managerEpoch;
    next.profile_=context.profile;
    const bool sameManager=previous.sessionEpoch_==context.sessionEpoch &&
        previous.manager_==context.manager && previous.managerEpoch_==context.managerEpoch &&
        previous.profile_==context.profile;
    for(std::size_t i=0;i<plan.carCount_;++i) {
        const auto& car=frame.cars[i];
        plan.cars_[i]=car;
        ++plan.stats_.inspected;
        const auto result=[&]() -> Result {
            if(!car.opponent || car.identity==context.player ||
               (context.currentTarget && car.identity==context.currentTarget))return Result::Excluded;
            if(!car.identity || !car.epoch)return Result::InvalidIdentity;
            if(car.best<0 || car.best>car.worst || car.worst>MaxLod)return Result::InvalidBounds;
            if(!car.bestMeshLoaded || !car.bestMesh)return Result::InvalidBestMesh;
            // Preserve native sentinels/out-of-range choices, including worst+1.
            if(car.selected<0 || car.selected>car.worst)return Result::UnsupportedSelection;

            // hypot avoids overflow from squaring finite distances. Subtraction
            // or a genuinely unrepresentable norm can still overflow: refuse it.
            // Two-argument hypot only: the three-argument overload lives in
            // MSVCP140_2.dll, which older Visual C++ runtimes lack, and the game
            // then silently loads Windows' d3d11.dll instead of this renderer.
            const double distance=std::hypot(std::hypot(car.position.x-context.camera.x,
                car.position.y-context.camera.y),car.position.z-context.camera.z);
            if(!std::isfinite(distance))return Result::InvalidDistance;
            const auto& old=previous.slots_[i];
            const bool wasNear=sameManager && old.car==car.identity && old.epoch==car.epoch &&
                old.mesh==car.bestMesh && old.best==car.best && old.worst==car.worst && old.nearby;
            const bool nearby=distance<=(wasNear?LeaveRadius:EnterRadius);
            next.slots_[i]={car.identity,car.epoch,car.bestMesh,car.best,car.worst,nearby};
            if(nearby)++plan.stats_.nearby;
            if(car.selected<=car.best)return Result::NativeBestOrBetter;
            if(!nearby)return Result::Distant;
            plan.changes_[plan.changeCount_++]={i,car.selected,car.best};
            return Result::Adjusted;
        }();
        ++plan.stats_.results[static_cast<std::size_t>(result)];
    }
    return plan;
}
} // namespace grid2vr::opponentlod
