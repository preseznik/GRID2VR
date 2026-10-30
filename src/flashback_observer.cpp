#include "flashback_observer.h"
#include "flashback_observer_guards.h"
#include "flashback_observer_model.h"
#include "driving_controls_route.h"
#include "solo_race.h"
#include <cstdio>
#include <fstream>
#include <mutex>

namespace grid2vr::flashbackobserver {
namespace {
constexpr ULONGLONG writeEveryMs=2000;

uintptr_t base{};
std::filesystem::path folder;
// Tick and Stop both run on the owning Present thread; the lock keeps a late
// Stop from interleaving with a sample, flag change or receipt write regardless.
std::mutex mutex;
bool installed{},stopped{},dirty{};
const char* reason="not_installed";
uint64_t samples{},readySamples{},monitorChanges{},usedIncrements{};
uintptr_t lastMonitor{};
int32_t lastUsed=unread;
ULONGLONG lastWrite{};
Timeline<128> timeline;
BenchmarkFlagPolicy flag;

bool Read(uintptr_t at,void* output,size_t bytes) {return ReadMemory(reinterpret_cast<void*>(at),output,bytes);}
// The only game write: the benchmark mode word in .data, never game code.
bool ExchangeFlag(uint32_t expected,uint32_t value) {
    return ExchangeBenchmarkMode(reinterpret_cast<volatile LONG*>(base+blockFlagRva),base,expected,value);
}

void Number(std::ostream& file,const char* name,int32_t value) {
    file<<'"'<<name<<"\":";
    if(value==unread) file<<"null";else file<<value;
}
void Flag(std::ostream& file,const char* name,int8_t value) {
    file<<'"'<<name<<"\":"<<(value<0?"null":value?"true":"false");
}
const char* FoundName(Found found) {
    return found==Found::Monitor?"monitor":found==Found::NoMonitor?"no_monitor":"no_local_car";
}
void WriteStatus() {
    std::ofstream file(folder/L"flashback-observer.json",std::ios::trunc);
    file<<"{\"schema\":3,\"gameCodeWrites\":0,\"installed\":"<<(installed?"true":"false")
        <<",\"stopped\":"<<(stopped?"true":"false")<<",\"reason\":\""<<reason<<"\",\"benchmarkFlag\":{\"cleared\":"
        <<(flag.Cleared()?"true":"false")<<",\"clears\":"<<flag.clears<<",\"restores\":"<<flag.restores
        <<",\"refused\":"<<flag.refused<<",\"foreignChanges\":"<<flag.foreign<<"},\"samples\":"<<samples<<",\"raceReadySamples\":"<<readySamples
        <<",\"monitorChanges\":"<<monitorChanges<<",\"usedIncrements\":"<<usedIncrements<<",\"timeline\":[";
    for(size_t i=0;i<timeline.Size();++i) {
        const auto& entry=timeline.Value(i);const auto& s=entry.sample;if(i)file<<',';
        file<<"{\"sample\":"<<entry.firstTick<<",\"samples\":"<<entry.ticks<<",\"found\":\""<<FoundName(s.found)<<"\",";
        Number(file,"remaining",s.remaining);file<<',';Number(file,"maximum",s.maximum);file<<',';Number(file,"used",s.used);
        file<<',';Number(file,"mode",s.mode);file<<',';Number(file,"raceState",s.raceState);file<<',';Number(file,"vehicleFlag",s.vehicleFlag);
        file<<',';Number(file,"blocked",s.blocked);file<<',';Flag(file,"ownerIsCar",s.ownerIsCar);file<<',';Flag(file,"resetRegistered",s.resetRegistered);
        file<<',';Flag(file,"worldRunning",s.worldRunning);file<<',';Flag(file,"replayActive",s.replayActive);
        file<<'}';
    }
    file<<"],\"overflow\":"<<timeline.Overflow()<<"}\n";
}
}

void Install(uintptr_t gameBase,const std::filesystem::path& output) {
    static bool attempted{};if(attempted)return;attempted=true;
    base=gameBase;folder=output;
    if(!InstructionsMatch(base) || !chasecamera::PauseGuardsMatch(base)) {reason="instruction_guard";WriteStatus();return;}
    installed=true;reason="observing";
    WriteStatus();lastWrite=GetTickCount64();
}
void Tick() {
    std::lock_guard lock(mutex);
    if(!installed || stopped)return;
    // Every frame: the flag window must close on the frame the race state leaves it.
    const auto route=drivingcontrols::ReadInputRoute(base,Read);
    uintptr_t monitor{};
    auto sample=ReadSample(base,route.valid?route.car:0,Read,&monitor);
    ReadWorld(base,Read,sample);
    const bool raceReady=sample.found==Found::Monitor && (NormalLocalRaceReady(base) || NormalSoloRaceReady(base));
    readySamples+=raceReady;
    flag.Apply(flag.Next(sample,raceReady,monitor),ExchangeFlag);
    if(monitor && monitor!=lastMonitor) {++monitorChanges;lastUsed=unread;}
    if(monitor) lastMonitor=monitor;
    if(sample.found==Found::Monitor && lastUsed!=unread && sample.used!=unread && sample.used>lastUsed) usedIncrements+=sample.used-lastUsed;
    if(sample.found==Found::Monitor) lastUsed=sample.used;
    timeline.Record(sample,samples++);dirty=true;
    const auto now=GetTickCount64();
    if(now-lastWrite<writeEveryMs)return;
    lastWrite=now;dirty=false;WriteStatus();
}
void Stop() {
    std::lock_guard lock(mutex);
    if(!installed || stopped)return;
    if(flag.Cleared()) flag.Apply(FlagAction::Restore,ExchangeFlag);
    stopped=true;reason="stopped";WriteStatus();
}
}
