#pragma once
#include "benchmark_mode.h"
#include "chase_camera_state.h"
#include "flashback_observer_guards.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace grid2vr::flashbackobserver {
inline constexpr int32_t unread=std::numeric_limits<int32_t>::min();

enum class Found : uint8_t {NoRoute,NoMonitor,Monitor};
// One data-only reading of the local car's FlashbackMonitor and the gate inputs
// that are plain data. The gate's three helper calls are not evaluated.
struct Sample {
    Found found=Found::NoRoute;
    int32_t remaining=unread,maximum=unread,used=unread,mode=unread;
    int32_t raceState=unread,vehicleFlag=unread,blocked=unread;
    // Owner +0x10 is the car the monitor was found on; the reset callback is in the event-15 slot.
    int8_t ownerIsCar=-1,resetRegistered=-1;
    // World not paused (guarded chase-camera reader) and replay playback active (image+0xe61010).
    int8_t worldRunning=-1,replayActive=-1;
    // Diagnostic only: local race readiness and the local controller's enable byte (+0x40).
    int8_t raceReady=-1,inputEnabled=-1;
    bool operator==(const Sample&) const=default;
};
inline constexpr uintptr_t replayRva=0xe61010;
// A paused world is the pause menu unless a replay (the flashback rewind) is playing.
template<class Reader> void ReadWorld(uintptr_t base,Reader read,Sample& sample) {
    sample.worldRunning=chasecamera::WorldRunning(base,read)?1:0;
    uint32_t replay{};
    sample.replayActive=read(base+replayRva,&replay,4)?replay!=0:-1;
}

// Reader: bool(uintptr_t address,void* output,size_t bytes). Reads only; a
// failed or implausible read leaves the field unread rather than guessing.
template<class Reader> Sample ReadSample(uintptr_t base,uintptr_t car,Reader read,uintptr_t* monitorOut=nullptr) {
    Sample sample;
    const auto word=[&](uintptr_t at,uint32_t& value){return at>=0x10000 && read(at,&value,4);};
    const auto field=[&](uintptr_t at){uint32_t value{};return word(at,value)?static_cast<int32_t>(value):unread;};
    sample.blocked=field(base+blockFlagRva);
    if(!car) return sample;
    sample.found=Found::NoMonitor;
    uint32_t components{},table{},index{},monitor{},vtable{};
    if(!word(car+componentsOffset,components) || !word(components+componentTableOffset,table) ||
       !word(base+monitorIndexRva,index) || index>0x10000 || !word(table+index*4,monitor) ||
       !word(monitor,vtable) || vtable!=static_cast<uint32_t>(base+monitorVtableRva)) return sample;
    sample.found=Found::Monitor;
    if(monitorOut) *monitorOut=monitor;
    sample.remaining=field(monitor+remainingOffset);sample.maximum=field(monitor+maximumOffset);sample.used=field(monitor+usedOffset);
    uint32_t owner{};
    if(word(monitor+ownerOffset,owner)) sample.ownerIsCar=owner==car;
    uint32_t state{};
    if(word(monitor+raceStateOffset,state)) sample.raceState=state?field(state+raceStateField):-1;
    uint32_t vehicle{};uint8_t vehicleByte{};
    if(word(monitor+vehicleOffset,vehicle)) sample.vehicleFlag=!vehicle?-1:(vehicle>=0x10000 && read(vehicle+vehicleFlagOffset,&vehicleByte,1))?vehicleByte:unread;
    uint32_t context{},contextTable{};
    if(!word(monitor+contextOffset,context) || !word(context+4,contextTable)) return sample;
    uint32_t modeIndex{},modeComponent{};
    if(word(base+modeIndexRva,modeIndex) && modeIndex<=0x10000 && word(contextTable+modeIndex*4,modeComponent) && modeComponent)
        sample.mode=field(modeComponent+modeOffset);
    uint32_t dispatcherIndex{},dispatcher{},slot{},begin{},end{};
    if(!word(base+dispatcherIndexRva,dispatcherIndex) || dispatcherIndex>0x10000 ||
       !word(contextTable+dispatcherIndex*4,dispatcher) || !dispatcher ||
       !word(dispatcher+eventSlotsOffset+resetEvent*4,slot)) return sample;
    sample.resetRegistered=0;
    if(!slot || !word(slot+4,begin) || !word(slot+8,end) || end<begin || (end-begin)%delegateBytes ||
       (end-begin)/delegateBytes>256) return sample;
    for(uint32_t entry=begin;entry<end;entry+=delegateBytes) {
        uint32_t delegate[3]{};
        if(!read(entry,delegate,sizeof(delegate))) {sample.resetRegistered=-1;return sample;}
        if(delegate[0]==static_cast<uint32_t>(base+resetCallbackRva) && delegate[1]==monitor) {sample.resetRegistered=1;break;}
    }
    return sample;
}

// Bounded change log: a sample is stored only when it differs from the previous
// one, with the observer tick it first appeared at and how long it lasted.
template<size_t N> class Timeline {
public:
    struct Entry {Sample sample;uint64_t firstTick{},ticks{};};
    void Record(const Sample& sample,uint64_t tick) {
        if(size_ && last_==sample) {if(!overflow_) ++entries_[size_-1].ticks;return;}
        last_=sample;
        if(size_<N) {entries_[size_++]={sample,tick,1};return;}
        ++overflow_;
    }
    size_t Size() const {return size_;}
    const Entry& Value(size_t index) const {return entries_[index];}
    uint64_t Overflow() const {return overflow_;}
private:
    std::array<Entry,N> entries_{};
    Sample last_{};
    size_t size_{};
    uint64_t overflow_{};
};

// Direct Practice/Race launch with -benchmarkAttract, and the gate refuses every
// flashback while the benchmark mode is set. Clear it only inside the gate's own
// racing window and restore it as soon as that window closes, so the
// benchmark-end exit and every non-racing reader keep the launch value:
// - monitor on the local car, race state 0 or 9, a nonzero allowance;
// - the local race ready: never during loading, and it ends at the finish line
//   (race state does not), while it holds through the flashback rewind;
// - world running or a replay playing: race state stays 0 in the pause menu, and
//   in-game Quit enters the benchmark exit (mode 2) before teardown.
// Normal launches (mode 0) and plain -benchmark (mode 1) are never touched.
enum class FlagAction : uint8_t {None,Clear,Restore};
class BenchmarkFlagPolicy {
public:
    static bool Racing(const Sample& s,bool raceReady) {
        return raceReady && s.found==Found::Monitor && s.ownerIsCar==1 && (s.raceState==0 || s.raceState==9) && s.maximum>0 &&
            (s.worldRunning==1 || s.replayActive==1);
    }
    FlagAction Next(const Sample& s,bool raceReady) const {
        const bool racing=Racing(s,raceReady);
        if(cleared_) return racing?FlagAction::None:FlagAction::Restore;
        return racing && s.blocked==static_cast<int32_t>(benchmarkAttract)?FlagAction::Clear:FlagAction::None;
    }
    // Exchange: bool(uint32_t expected,uint32_t value), a compare-exchange on the
    // flag, so a value the game changed meanwhile is never overwritten.
    template<class Exchange> void Apply(FlagAction action,Exchange exchange) {
        if(action==FlagAction::Clear) {if(exchange(benchmarkAttract,0u)) {cleared_=true;++clears;} else ++refused;}
        else if(action==FlagAction::Restore) {if(exchange(0u,benchmarkAttract)) ++restores; else ++foreign;cleared_=false;}
    }
    bool Cleared() const {return cleared_;}
    uint64_t clears{},restores{},refused{},foreign{};
private:
    bool cleared_{};
};
static_assert(benchmarkModeWordRva==blockFlagRva);
// The one game write. Readiness checks accept 0 only for the registered image, so
// register before clearing and retire the registration once the launch value is
// back (or the clear did not happen).
inline bool ExchangeBenchmarkMode(volatile LONG* word,uintptr_t image,uint32_t expected,uint32_t value) {
    if(!value) benchmarkModeClearedImage.store(image);
    const bool exchanged=static_cast<uint32_t>(InterlockedCompareExchange(word,static_cast<LONG>(value),static_cast<LONG>(expected)))==expected;
    if(value || !exchanged) benchmarkModeClearedImage.store(0);
    return exchanged;
}
}
