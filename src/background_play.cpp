#include "background_play.h"
#include "background_play_patch.h"
#include <MinHook.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <fstream>
#include <mutex>

namespace grid2vr::backgroundplay {
namespace {
using FocusLoss=void (__thiscall*)(void*);
FocusLoss original{};
uintptr_t base{};
std::filesystem::path folder;
std::mutex mutex;
std::atomic<bool> installed{},enabled{},stopped{};
bool requestedSuppress{},failed{};
uint64_t calls{},matchedCalls{},foregroundPresents{},backgroundPresents{},hiddenPresents{};
const char* reason="not_installed";
struct Witness { uintptr_t caller{};unsigned before{},after{},replayBefore{},replayAfter{};bool readable{}; };
std::array<Witness,16> samples{};
size_t sampleCount{};

void WriteStatus() {
    std::ofstream file(folder/L"background-play.json",std::ios::trunc);
    file << "{\"requested\":\"" << (requestedSuppress?"suppress":"observe") << "\",\"installed\":" << (installed?"true":"false")
         << ",\"enabled\":" << (enabled?"true":"false") << ",\"failed\":" << (failed?"true":"false")
         << ",\"stopped\":" << (stopped?"true":"false") << ",\"reason\":\"" << reason << "\",\"focusLossCalls\":" << calls
         << ",\"matchedCallerCalls\":" << matchedCalls << ",\"foregroundPresents\":" << foregroundPresents
         << ",\"backgroundPresents\":" << backgroundPresents << ",\"hiddenPresents\":" << hiddenPresents << ",\"samples\":[";
    for(size_t i=0;i<sampleCount;++i) {
        const auto& w=samples[i];if(i)file<<',';
        file << "{\"callerRva\":" << w.caller << ",\"readable\":" << (w.readable?"true":"false")
             << ",\"pauseRequestBefore\":" << w.before << ",\"pauseRequestAfter\":" << w.after
             << ",\"replayStateBefore\":" << w.replayBefore << ",\"replayStateAfter\":" << w.replayAfter << '}';
    }
    file << "],\"manualPauseValidated\":false,\"backgroundGameplayValidated\":false}\n";
}
bool State(void* self,unsigned& pending,unsigned& replayState) {
    uint8_t byte{};uintptr_t replay{};
    const auto address=reinterpret_cast<uintptr_t>(self);
    if(!ReadMemory(reinterpret_cast<void*>(address+0x2159),&byte,1) ||
       !ReadMemory(reinterpret_cast<void*>(address+0x148c),&replay,sizeof(replay))) return false;
    pending=byte;replayState=~0u;
    return !replay || ReadMemory(reinterpret_cast<void*>(replay+0x1d8),&replayState,sizeof(replayState));
}
void __fastcall FocusLost(void* self,void*) {
    if(stopped) { original(self);return; }
    Witness w{};w.caller=reinterpret_cast<uintptr_t>(_ReturnAddress())-base;
    const bool before=State(self,w.before,w.replayBefore);
    original(self);
    w.readable=State(self,w.after,w.replayAfter) && before;
    std::lock_guard lock(mutex);
    ++calls;matchedCalls+=w.caller==0x288e74;
    if(sampleCount<samples.size()) samples[sampleCount++]=w;
    if(calls<=samples.size()) WriteStatus();
}
}
void Install(uintptr_t gameBase,const std::filesystem::path& output) {
    static bool attempted{};if(attempted)return;attempted=true;
    std::ifstream request(output/L"background-play.request");std::string mode;request>>mode;request>>std::ws;
    if(!request.eof() || (mode!="observe" && mode!="suppress"))return;
    std::lock_guard lock(mutex);
    base=gameBase;folder=output;requestedSuppress=mode=="suppress";
    if(!InstructionsMatch(base)) { failed=true;reason="instruction_guard";WriteStatus();return; }
    if(requestedSuppress && !SuppressPauseRequests(base)) { failed=true;reason="patch_failed";WriteStatus();return; }
    enabled=requestedSuppress;
    auto status=MH_CreateHook(reinterpret_cast<void*>(base+focusRoutineRva),reinterpret_cast<void*>(FocusLost),reinterpret_cast<void**>(&original));
    if(status==MH_OK)status=MH_EnableHook(reinterpret_cast<void*>(base+focusRoutineRva));
    installed=status==MH_OK;failed=!installed;
    reason=installed?(enabled?"focus_pause_requests_suppressed":"observing_focus_loss"):"observer_hook_failed";
    WriteStatus();
}
bool Enabled() { return enabled && installed; }
void ObserveWindow(unsigned state) {
    if(!installed || stopped)return;
    std::lock_guard lock(mutex);
    if(state==5)++foregroundPresents;else if(state==4)++backgroundPresents;else ++hiddenPresents;
    if((foregroundPresents+backgroundPresents+hiddenPresents)%120==0)WriteStatus();
}
void Stop() { if(!installed)return;stopped=true;std::lock_guard lock(mutex);WriteStatus(); }
}
