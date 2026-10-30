#include "flashback_observer.h"
#include "flashback_observer_guards.h"
#include "flashback_observer_model.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace grid2vr::flashbackobserver;
namespace {
constexpr size_t imageSize=0xe70000;
void WriteGuards(uint8_t* image) {
    for(const auto& guard:codeGuards) std::memcpy(image+guard.rva,guard.bytes.data(),guard.length);
    for(const auto& guard:addressGuards) {
        const auto value=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(image)+guard.target);
        std::memcpy(image+guard.rva,&value,sizeof(value));
    }
}
template<class T> uint32_t Address(T* value) {return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value));}
void Put(void* object,uint32_t offset,uint32_t value) {std::memcpy(static_cast<uint8_t*>(object)+offset,&value,4);}
bool Read(uintptr_t at,void* output,size_t bytes) {return grid2vr::ReadMemory(reinterpret_cast<void*>(at),output,bytes);}
}
int main() {
    // A private image at an allocation base other than 0x400000: absolute
    // operands must hold the relocated address, code bytes must be exact.
    auto* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,imageSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!image) return 1;
    const auto base=reinterpret_cast<uintptr_t>(image);
    WriteGuards(image);
    if(!InstructionsMatch(base) || InstructionsMatch(0)) return 2;
    for(const auto& guard:codeGuards) {
        for(size_t i=0;i<guard.length;++i) {
            image[guard.rva+i]^=0x01;
            const bool accepted=InstructionsMatch(base);
            image[guard.rva+i]^=0x01;
            if(accepted) {std::printf("code guard 0x%x+%zu not enforced\n",static_cast<unsigned>(guard.rva),i);return 3;}
        }
    }
    for(const auto& guard:addressGuards) {
        uint32_t original{};std::memcpy(&original,image+guard.rva,4);
        const uint32_t unrelocated=static_cast<uint32_t>(0x400000+guard.target);
        std::memcpy(image+guard.rva,&unrelocated,4);
        const bool accepted=InstructionsMatch(base);
        std::memcpy(image+guard.rva,&original,4);
        if(accepted) return 4;
    }

    // Object graph as the attach routine builds it: car components -> monitor;
    // monitor context -> mode component and event dispatcher.
    const uint32_t monitorIndex=3,modeIndex=5,dispatcherIndex=7,blocked=0;
    std::memcpy(image+monitorIndexRva,&monitorIndex,4);std::memcpy(image+modeIndexRva,&modeIndex,4);
    std::memcpy(image+dispatcherIndexRva,&dispatcherIndex,4);std::memcpy(image+blockFlagRva,&blocked,4);
    alignas(4) static uint8_t car[0x40]{},components[8]{},vehicle[0xc0]{},raceState[0x40]{},context[8]{},modeComponent[0xc00]{},dispatcher[0x1c00]{},slot[0x14]{};
    alignas(4) static uint32_t carTable[8]{},contextTable[8]{},delegates[6]{};
    alignas(4) static uint8_t monitor[0x38]{};
    Put(car,componentsOffset,Address(components));Put(components,componentTableOffset,Address(carTable));carTable[monitorIndex]=Address(monitor);
    Put(monitor,0,static_cast<uint32_t>(base+monitorVtableRva));Put(monitor,ownerOffset,Address(car));Put(monitor,contextOffset,Address(context));
    Put(monitor,vehicleOffset,Address(vehicle));vehicle[vehicleFlagOffset]=1;Put(monitor,raceStateOffset,Address(raceState));Put(raceState,raceStateField,9);
    Put(monitor,remainingOffset,0);Put(monitor,maximumOffset,5);Put(monitor,usedOffset,0);
    Put(context,4,Address(contextTable));contextTable[modeIndex]=Address(modeComponent);Put(modeComponent,modeOffset,8);
    contextTable[dispatcherIndex]=Address(dispatcher);Put(dispatcher,eventSlotsOffset+resetEvent*4,Address(slot));
    delegates[0]=0x1234;delegates[1]=Address(monitor);delegates[2]=Address(monitor);
    delegates[3]=static_cast<uint32_t>(base+resetCallbackRva);delegates[4]=Address(monitor);delegates[5]=Address(monitor);
    Put(slot,4,Address(delegates));Put(slot,8,Address(delegates+6));
    const auto carAddress=reinterpret_cast<uintptr_t>(car);

    uintptr_t found{};
    auto s=ReadSample(base,carAddress,Read,&found);
    if(s.found!=Found::Monitor || found!=reinterpret_cast<uintptr_t>(monitor) || s.remaining!=0 || s.maximum!=5 || s.used!=0 ||
       s.mode!=8 || s.raceState!=9 || s.vehicleFlag!=1 || s.blocked!=0 || s.ownerIsCar!=1 || s.resetRegistered!=1) return 5;
    // No car: only the global block flag is read.
    s=ReadSample(base,0,Read);
    if(s.found!=Found::NoRoute || s.blocked!=0 || s.remaining!=unread || s.mode!=unread) return 6;
    // A component that is not a FlashbackMonitor is never interpreted.
    Put(monitor,0,static_cast<uint32_t>(0x400000+monitorVtableRva));
    s=ReadSample(base,carAddress,Read);
    if(s.found!=Found::NoMonitor || s.remaining!=unread || s.blocked!=0) return 7;
    Put(monitor,0,static_cast<uint32_t>(base+monitorVtableRva));
    // Reset callback for another object, a missing slot, and no race-state/vehicle objects.
    delegates[4]=Address(car);
    if(ReadSample(base,carAddress,Read).resetRegistered!=0) return 8;
    Put(dispatcher,eventSlotsOffset+resetEvent*4,0);Put(monitor,raceStateOffset,0);Put(monitor,vehicleOffset,0);Put(monitor,ownerOffset,0);
    s=ReadSample(base,carAddress,Read);
    if(s.resetRegistered!=0 || s.raceState!=-1 || s.vehicleFlag!=-1 || s.ownerIsCar!=0) return 9;
    // A malformed delegate vector is not walked.
    Put(dispatcher,eventSlotsOffset+resetEvent*4,Address(slot));Put(slot,8,Address(delegates)+5);
    if(ReadSample(base,carAddress,Read).resetRegistered!=0) return 10;

    // The timeline stores changes only, with durations, and stays bounded.
    Timeline<2> timeline;Sample a{},b{};b.used=1;Sample c=b;c.used=2;
    timeline.Record(a,0);timeline.Record(a,1);timeline.Record(b,2);timeline.Record(c,3);timeline.Record(c,4);timeline.Record(a,5);
    if(timeline.Size()!=2 || timeline.Value(0).ticks!=2 || timeline.Value(1).firstTick!=2 || timeline.Value(1).ticks!=1 || timeline.Overflow()!=2) return 11;

    // Benchmark mode window: cleared only on a ready local race inside the gate's
    // race states, restored when the window closes, never for Normal or -benchmark.
    volatile LONG mode=2;
    const auto exchange=[&](uint32_t expected,uint32_t value){return ExchangeBenchmarkMode(&mode,base,expected,value);};
    Sample racing{};racing.found=Found::Monitor;racing.ownerIsCar=1;racing.raceState=0;racing.maximum=5;racing.blocked=2;
    racing.worldRunning=1;racing.replayActive=0;
    BenchmarkFlagPolicy policy;
    if(policy.Next(racing,false)!=FlagAction::None) return 15;
    using Change=void(*)(Sample&);
    const Change changes[]{[](Sample& s){s.found=Found::NoMonitor;},[](Sample& s){s.ownerIsCar=0;},[](Sample& s){s.raceState=3;},
                           [](Sample& s){s.maximum=0;},[](Sample& s){s.blocked=0;},[](Sample& s){s.blocked=1;},
                           [](Sample& s){s.worldRunning=0;},[](Sample& s){s.worldRunning=-1;s.replayActive=-1;}};
    for(const auto change:changes) {
        Sample s=racing;change(s);
        if(policy.Next(s,true)!=FlagAction::None) return 16;
    }
    if(grid2vr::IsBenchmarkAttractMode(base,0)) return 17;
    policy.Apply(policy.Next(racing,true),exchange);
    if(mode!=0 || !policy.Cleared() || policy.clears!=1 || !grid2vr::IsBenchmarkAttractMode(base,0) || grid2vr::IsBenchmarkAttractMode(base+4,0)) return 18;
    // Race state 9 and a paused world with replay playback stay inside the window.
    Sample cleared=racing;cleared.blocked=0;cleared.raceState=9;
    if(policy.Next(cleared,true)!=FlagAction::None) return 19;
    cleared.worldRunning=0;cleared.replayActive=1;
    if(policy.Next(cleared,true)!=FlagAction::None) return 25;
    // The pause menu (paused world, no replay) restores the launch value for in-game Quit.
    cleared.replayActive=0;
    policy.Apply(policy.Next(cleared,true),exchange);
    if(mode!=2 || policy.Cleared() || policy.restores!=1) return 26;
    policy.Apply(policy.Next(racing,true),exchange);
    if(mode!=0 || policy.clears!=2) return 27;
    // The finish line ends local race readiness while race state stays 0.
    cleared=racing;cleared.blocked=0;
    policy.Apply(policy.Next(cleared,false),exchange);
    if(mode!=2 || policy.Cleared() || policy.restores!=2 || grid2vr::IsBenchmarkAttractMode(base,0)) return 20;
    if(policy.Next(racing,false)!=FlagAction::None) return 23;
    policy.Apply(policy.Next(racing,true),exchange);
    if(mode!=0 || policy.clears!=3) return 24;
    // A value the game changed meanwhile is kept on restore.
    mode=5;cleared.raceState=3;
    policy.Apply(policy.Next(cleared,true),exchange);
    if(mode!=5 || policy.Cleared() || policy.foreign!=1 || grid2vr::IsBenchmarkAttractMode(base,0)) return 21;
    // A clear that loses the race to the game registers nothing.
    mode=1;policy.Apply(FlagAction::Clear,exchange);
    if(mode!=1 || policy.Cleared() || policy.refused!=1 || grid2vr::IsBenchmarkAttractMode(base,0)) return 22;

    // World state: the guarded pause owner's byte and the replay pointer.
    for(const auto& guard:grid2vr::chasecamera::PauseGuards(base)) std::memcpy(image+guard.rva,guard.bytes.data(),guard.size);
    if(!grid2vr::chasecamera::PauseGuardsMatch(base) || !InstructionsMatch(base)) return 28;
    alignas(4) static uint8_t pauseOwner[0x300]{};
    Put(pauseOwner,0,static_cast<uint32_t>(base+0xccaf10));
    const uint32_t ownerAddress=Address(pauseOwner);std::memcpy(image+0xd98014,&ownerAddress,4);
    Sample world{};ReadWorld(base,Read,world);
    if(world.worldRunning!=1 || world.replayActive!=0) return 29;
    pauseOwner[0x2f6]=1;const uint32_t replay=0x1234;std::memcpy(image+replayRva,&replay,4);
    ReadWorld(base,Read,world);
    if(world.worldRunning!=0 || world.replayActive!=1) return 30;
    const uint32_t none=0;std::memcpy(image+0xd98014,&none,4);std::memcpy(image+replayRva,&none,4);
    ReadWorld(base,Read,world);
    if(world.worldRunning!=0 || world.replayActive!=0) return 31;

    // Install checks only instruction bytes; Tick samples without a local car route.
    const auto folder=std::filesystem::temp_directory_path()/("grid2vr-flashback-observer-"+std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    std::vector<uint8_t> before(image+codeGuards[0].rva,image+codeGuards[0].rva+codeGuards[0].length);
    Install(base,folder);
    std::stringstream text;text<<std::ifstream(folder/L"flashback-observer.json").rdbuf();
    if(text.str().find("\"schema\":3,\"gameCodeWrites\":0,\"installed\":true")==std::string::npos ||
       text.str().find("\"reason\":\"observing\"")==std::string::npos) {std::puts(text.str().c_str());return 12;}
    // No local car: the benchmark mode word is read but never written.
    const uint32_t attract=2;std::memcpy(image+blockFlagRva,&attract,4);
    Tick();Tick();Stop();
    std::stringstream stopped;stopped<<std::ifstream(folder/L"flashback-observer.json").rdbuf();
    const auto final=stopped.str();
    uint32_t after{};std::memcpy(&after,image+blockFlagRva,4);
    if(after!=2 || final.find("\"reason\":\"stopped\"")==std::string::npos || final.find("\"samples\":2,\"raceReadySamples\":0,")==std::string::npos ||
       final.find("\"benchmarkFlag\":{\"cleared\":false,\"clears\":0,\"restores\":0,\"refused\":0,\"foreignChanges\":0}")==std::string::npos ||
       final.find("\"timeline\":[{\"sample\":0,\"samples\":2,\"found\":\"no_local_car\",\"remaining\":null")==std::string::npos ||
       final.find("\"blocked\":2,\"ownerIsCar\":null,\"resetRegistered\":null,\"worldRunning\":false,\"replayActive\":false,\"raceReady\":false,\"inputEnabled\":null}")==std::string::npos) {std::puts(final.c_str());return 13;}
    // Observation never changes guarded code.
    if(std::memcmp(before.data(),image+codeGuards[0].rva,before.size()) || !InstructionsMatch(base)) return 14;
    std::filesystem::remove_all(folder);
    VirtualFree(image,0,MEM_RELEASE);
    std::puts("Flashback: no code writes, every code byte and relocated operand guarded, monitor decoded, benchmark mode cleared only in a ready racing window and restored, bounded receipt.");
    return 0;
}
