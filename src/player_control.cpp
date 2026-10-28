#include "player_control.h"
#include "playback_mode.h"
#include "guarded_hooks.h"
#include "local_controller_identity.h"
#include <MinHook.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>

namespace grid2vr::playercontrol {
namespace {
using Controls=std::array<uint32_t,10>;
using Native=bool (__thiscall*)(void*,unsigned,Controls*,const Controls*,const Controls*);
Native original{};
using VehicleState=std::array<uint32_t,14>;
using VehicleWriter=void (__thiscall*)(void*,const VehicleState*,unsigned,float);
using InputEligibility=bool (__thiscall*)(void*,unsigned);
using AttractFinish=void (__thiscall*)(void*,bool);
VehicleWriter originalWriter{};
InputEligibility originalEligibility{};
AttractFinish originalFinish{};
uintptr_t base{};
std::filesystem::path folder;
bool requestedHuman{},installed{};
std::atomic<bool> stopped{},failed{};
std::mutex mutex;
uint64_t calls{},eligible{},switched{},restoredCalls{},playbackChanged{},refused{};
DWORD ownerThread{};
struct Witness { unsigned mode{},local{},humanEnabled{},selected{};Controls input{},before{},after{};bool nativeResult{},changed{},restored{}; };
std::array<Witness,16> witnesses{};
size_t witnessCount{};
const char* lastReason="not_installed";
ULONGLONG startedAt{},lastObservationWrite{};
struct ReplayWitness {
    uint32_t caller{},vehicle{},frame{},lifecycle{},inputMode{},state{};
    uint64_t milliseconds{};
    bool identity{},readable{};
    VehicleState bits{};
};
std::array<ReplayWitness,8> replayWitnesses{};
ReplayWitness latestReplay{};
size_t replayWitnessCount{};
uint64_t writerCalls{},writerIdentified{},writerUnreadable{},recordedFrameChanges{};
uint64_t inputCalls{},inputAllowed{},inputDenied{};
uint32_t lastInputPlayer{};
struct CallerCount { uintptr_t rva{};uint64_t calls{}; };
std::array<CallerCount,8> writerCallers{};
uint64_t otherWriterCallers{};
struct FinishWitness {
    uint64_t calls{},milliseconds{};
    uintptr_t caller{};
    uint32_t mode{};
    uint8_t active{},flag94{};
    bool identity{},readable{},argument{};
} finish;

template<class T> bool Read(uintptr_t address,T& value) { return ReadMemory(reinterpret_cast<void*>(address),&value,sizeof(value)); }
bool Word(uintptr_t address,uint32_t expected) { uint32_t actual{};return Read(address,actual) && actual==expected; }
bool Pointer(uintptr_t address,uintptr_t& value) { return Read(address,value) && value>=0x10000 && value<=UINTPTR_MAX-0x1000 && value%4==0; }

void WriteReplayWitness(std::ostream& file,const ReplayWitness& w) {
    file << "{\"callerRva\":" << w.caller << ",\"vehicle\":" << w.vehicle << ",\"recordedFrame\":" << w.frame
         << ",\"lifecycle\":" << w.lifecycle << ",\"inputMode\":" << w.inputMode << ",\"stateByte\":" << w.state
         << ",\"milliseconds\":" << w.milliseconds << ",\"soloIdentity\":" << (w.identity?"true":"false")
         << ",\"readable\":" << (w.readable?"true":"false") << ",\"stateBits\":[";
    for(size_t i=0;i<w.bits.size();++i) { if(i)file<<',';file<<w.bits[i]; }file << "]}";
}

void WriteStatus() {
    std::ofstream file(folder/L"player-control.json",std::ios::trunc);
    file << "{\"requested\":\"" << (requestedHuman?"human":"observe") << "\",\"installed\":" << (installed?"true":"false")
         << ",\"stopped\":" << (stopped?"true":"false") << ",\"failed\":" << (failed?"true":"false")
         << ",\"reason\":\"" << lastReason << "\",\"calls\":" << calls << ",\"eligible\":" << eligible
         << ",\"switched\":" << switched << ",\"restored\":" << restoredCalls << ",\"playbackChangedInput\":" << playbackChanged
         << ",\"refused\":" << refused << ",\"playerControlValidated\":false,\"samples\":[";
    for (size_t i=0;i<witnessCount;++i) {
        const auto& w=witnesses[i];if (i) file << ',';
        file << "{\"mode\":" << w.mode << ",\"local\":" << w.local << ",\"humanEnabled\":" << w.humanEnabled
             << ",\"selected\":" << w.selected << ",\"nativeResult\":" << (w.nativeResult?"true":"false")
             << ",\"changed\":" << (w.changed?"true":"false") << ",\"restored\":" << (w.restored?"true":"false");
        const Controls* values[]{&w.input,&w.before,&w.after};const char* names[]{"inputBits","beforeBits","afterBits"};
        for (size_t j=0;j<3;++j) { file << ",\"" << names[j] << "\":[";for (size_t k=0;k<10;++k) { if(k)file<<',';file<<(*values[j])[k]; }file<<']'; }
        file << '}';
    }
    file << "],\"replayObservation\":{\"writerCalls\":" << writerCalls << ",\"writerIdentified\":" << writerIdentified
         << ",\"writerUnreadable\":" << writerUnreadable << ",\"recordedFrameChanges\":" << recordedFrameChanges
         << ",\"inputEligibilityCalls\":" << inputCalls << ",\"inputAllowed\":" << inputAllowed << ",\"inputDenied\":" << inputDenied
         << ",\"lastInputPlayer\":" << lastInputPlayer << ",\"otherWriterCallers\":" << otherWriterCallers << ",\"writerCallers\":[";
    for(size_t i=0;i<writerCallers.size();++i) {
        if(i)file<<',';file << "{\"rva\":" << writerCallers[i].rva << ",\"calls\":" << writerCallers[i].calls << '}';
    }
    file << "],\"firstWrites\":[";
    for(size_t i=0;i<replayWitnessCount;++i) { if(i)file<<',';WriteReplayWitness(file,replayWitnesses[i]); }
    file << "],\"latestWrite\":";WriteReplayWitness(file,latestReplay);
    file << ",\"finish\":{\"calls\":" << finish.calls << ",\"callerRva\":" << finish.caller
         << ",\"milliseconds\":" << finish.milliseconds << ",\"mode\":" << finish.mode
         << ",\"active\":" << unsigned(finish.active) << ",\"flag94\":" << unsigned(finish.flag94)
         << ",\"singletonIdentity\":" << (finish.identity?"true":"false") << ",\"readable\":" << (finish.readable?"true":"false")
         << ",\"argument\":" << (finish.argument?"true":"false") << "}}}\n";
}

// Observers always forward the native call and never change its arguments or
// result. Identity counts are evidence only; a matching address is not a handoff.
void __fastcall WriteVehicle(void* self,void*,const VehicleState* state,unsigned vehicle,float blend) {
    if (!stopped && !failed) {
        const auto playback=reinterpret_cast<uintptr_t>(self);
        uintptr_t current{},parent{},begin{},end{},car{};
        uint8_t forceLocal{};
        ReplayWitness w{};w.caller=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())-base);
        w.vehicle=vehicle;w.milliseconds=GetTickCount64()-startedAt;
        w.identity=Pointer(base+0xe61010,current) && current==playback && vehicle==0 &&
            Pointer(playback+0xa8,parent) && Word(parent,static_cast<uint32_t>(base+0xd1266c)) &&
            Pointer(parent+0xa8,begin) && Pointer(parent+0xac,end) && end-begin==4 && Pointer(begin,car) &&
            Word(car+0xb0,0) && Read(playback+0x9e,forceLocal) && forceLocal==0;
        w.readable=Read(playback,w.frame) && Read(playback+0xac,w.lifecycle) &&
            Read(playback+0xb0,w.inputMode) && Read(reinterpret_cast<uintptr_t>(state),w.bits);
        w.state=w.bits[13]>>24;
        std::lock_guard lock(mutex);
        ++writerCalls;writerIdentified+=w.identity;writerUnreadable+=!w.readable;
        recordedFrameChanges+=latestReplay.readable && w.readable && latestReplay.frame!=w.frame;
        latestReplay=w;
        if(replayWitnessCount<replayWitnesses.size()) replayWitnesses[replayWitnessCount++]=w;
        bool counted=false;
        for(auto& c:writerCallers) if(!c.calls || c.rva==w.caller) { c.rva=w.caller;++c.calls;counted=true;break; }
        otherWriterCallers+=!counted;
        if(w.milliseconds-lastObservationWrite>=1000) { lastObservationWrite=w.milliseconds;WriteStatus(); }
    }
    originalWriter(self,state,vehicle,blend);
}

bool __fastcall EligibleInput(void* self,void*,unsigned player) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool result=originalEligibility(self,player);
    // This exact call is the gate in the local-controller update, not other
    // consumers of the same general input-manager membership query.
    if(!stopped && !failed && caller==base+0x1aab77) {
        std::lock_guard lock(mutex);
        ++inputCalls;inputAllowed+=result;inputDenied+=!result;lastInputPlayer=player;
        const auto elapsed=GetTickCount64()-startedAt;
        if(elapsed-lastObservationWrite>=1000) { lastObservationWrite=elapsed;WriteStatus(); }
    }
    return result;
}

void __fastcall FinishAttract(void* self,void*,bool argument) {
    if(!stopped && !failed) {
        const auto address=reinterpret_cast<uintptr_t>(self);
        std::lock_guard lock(mutex);
        ++finish.calls;finish.caller=reinterpret_cast<uintptr_t>(_ReturnAddress())-base;
        finish.milliseconds=GetTickCount64()-startedAt;finish.argument=argument;
        finish.identity=address==base+0xe590f8;
        finish.readable=Read(address+0x84,finish.mode) && Read(address+0x88,finish.active) && Read(address+0x94,finish.flag94);
        WriteStatus(); // Flush before the original routine can call ExitProcess.
    }
    originalFinish(self,argument);
}

// Exact solo path observed in the native attract run. These checks never call
// virtual methods or infer that arbitrary controllers are human/AI by index.
const char* Check(void* self,unsigned vehicle,Controls* output,const Controls* input,const Controls* ai,Witness& w) {
    const auto playback=reinterpret_cast<uintptr_t>(self);
    uintptr_t current{},parent{},begin{},end{},car{},head{},node{},group{},children{},childrenEnd{},human{},other{};
    uint8_t forceLocal{},enabled{};
    if (!Pointer(base+0xe61010,current) || current!=playback || !Pointer(playback+0xa8,parent) ||
        !Word(parent,static_cast<uint32_t>(base+0xd1266c))) return "playback_identity";
    if (!Read(playback+0xb0,w.mode) || !Read(playback+0x10,w.local) || !Read(playback+0x9e,forceLocal) ||
        w.mode!=0 || forceLocal!=0 || w.local!=0 || vehicle!=0) return "playback_mode_or_vehicle";
    if (!Pointer(parent+0xa8,begin) || !Pointer(parent+0xac,end) || end-begin!=4 || !Pointer(begin,car) ||
        !Word(car+0xb0,0)) return "not_single_local_vehicle";
    const auto manager=parent+0x190;
    if (!Word(manager,static_cast<uint32_t>(base+0xd0bc48)) || !Word(manager+0x18,1) ||
        !Pointer(manager+0x14,head) || !Pointer(head,node) || !Word(node+12,static_cast<uint32_t>(car)) ||
        !Pointer(node+16,group) || !Word(group,static_cast<uint32_t>(base+0xd0bb30))) return "controller_group";
    if (!Read(group+0x18,w.selected) || w.selected!=0 || !Pointer(group+8,children) || !Pointer(group+12,childrenEnd) ||
        childrenEnd-children!=8 || !Pointer(children,human) || !Pointer(children+4,other) ||
        !ReadLocalControllerIdentity(base,human,[](uintptr_t at,void* output,size_t bytes){return ReadMemory(reinterpret_cast<void*>(at),output,bytes);}) ||
        !Word(other,static_cast<uint32_t>(base+0xd039ac))) return "controller_children";
    if (!Read(human+0x40,enabled)) return "input_enabled_unreadable";
    w.humanEnabled=enabled;
    if (reinterpret_cast<uintptr_t>(output)!=human+0x14 || reinterpret_cast<uintptr_t>(input)!=base+0xe59080 ||
        reinterpret_cast<uintptr_t>(ai)!=other+4 || !Read(reinterpret_cast<uintptr_t>(input),w.input) ||
        !Read(reinterpret_cast<uintptr_t>(output),w.before)) return "input_addresses";
    if (enabled!=1) return "local_input_disabled";
    return nullptr;
}
struct Call { void* self;unsigned vehicle;Controls* output;const Controls* input;const Controls* ai; };
bool Invoke(void* context) { const auto& c=*static_cast<Call*>(context);return original(c.self,c.vehicle,c.output,c.input,c.ai); }

bool __fastcall Playback(void* self,void*,unsigned vehicle,Controls* output,const Controls* input,const Controls* ai) {
    if (stopped || failed) return original(self,vehicle,output,input,ai);
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    Witness w{};
    const char* reason=caller!=base+0x520c9c?"caller":Check(self,vehicle,output,input,ai,w);
    bool changed=false,restored=false;
    {
        std::lock_guard lock(mutex);
        ++calls;
        if (!ownerThread) ownerThread=GetCurrentThreadId();
        if (GetCurrentThreadId()!=ownerThread) reason="foreign_thread";
        if (!reason) ++eligible;else ++refused;
    }
    auto* mode=reinterpret_cast<volatile LONG*>(reinterpret_cast<uintptr_t>(self)+0xb0);
    if (!reason && requestedHuman) {
        changed=ExchangeMode(mode,0,1);
        if (!changed) { failed=true;reason="mode_exchange_failed"; }
    }
    Call call{self,vehicle,output,input,ai};
    const bool result=changed?CallWithLocalInput(mode,Invoke,&call,restored):Invoke(&call);
    if (!reason && !Read(reinterpret_cast<uintptr_t>(output),w.after)) { failed=true;reason="output_unreadable"; }
    if (changed && (!restored || result || w.after!=w.input)) { failed=true;reason="local_copy_or_restore_failed"; }
    w.changed=changed;w.restored=restored;w.nativeResult=result;
    {
        std::lock_guard lock(mutex);
        switched+=changed;restoredCalls+=changed && restored;
        playbackChanged+=!reason && !changed && w.after!=w.input;
        if (!reason && witnessCount<witnesses.size()) witnesses[witnessCount++]=w;
        lastReason=reason?reason:requestedHuman?"local_input_restored":"observing_playback";
        if (calls<=16 || calls%120==0 || failed) WriteStatus();
    }
    return result;
}
}

void Install(uintptr_t gameBase,const std::filesystem::path& output) {
    static bool attempted{};if (attempted) return;attempted=true;
    std::ifstream request(output/L"player-control.request");std::string mode;request>>mode;request>>std::ws;
    if (!request.eof() || (mode!="observe" && mode!="human")) return;
    std::lock_guard lock(mutex);
    base=gameBase;folder=output;requestedHuman=mode=="human";startedAt=GetTickCount64();
    constexpr uint8_t prologue[]{0x55,0x8b,0xec,0x8b,0x55,0x08,0x83,0xec,0x38,0x53,0x56,0x8b,0xf1};
    constexpr uint8_t caller[]{0xe8,0xa4,0xe3,0x15,0x00,0xc5,0xf8,0x57,0xc0,0x84,0xc0,0x74,0x04,0xc6,0x45,0xfe,0x00};
    constexpr uint8_t localCopy[]{0x8b,0x45,0x10,0xc5,0xfa,0x7e,0x00,0x8b,0x4d,0x0c,0xc5,0xf9,0xd6,0x01};
    constexpr uint8_t humanGetter[]{0x8d,0x41,0x14,0xc3};
    constexpr uint8_t aiGetter[]{0x8d,0x41,0x04,0xc3};
    constexpr uint8_t modeBranch[]{0x8b,0x86,0xb0,0x00,0x00,0x00,0x83,0xf8,0x01,0x75,0x0a,0x8b,0x4d,0x08,0x3b,0x4e,0x10,0x75,0x02,0x8b,0xd8};
    constexpr uint8_t writerPrologue[]{0x53,0x8b,0xdc,0x83,0xec,0x08,0x83,0xe4,0xf0,0x83,0xc4,0x04,0x55,0x8b,0x6b,0x04,0x89,0x6c,0x24,0x04};
    constexpr uint8_t eligibilityPrologue[]{0x55,0x8b,0xec,0x83,0xec,0x14,0x8b,0x81,0x80,0x00,0x00,0x00};
    constexpr uint8_t eligibilityCaller[]{0xe8,0xe9,0xf6,0x17,0x00,0x84,0xc0,0x0f,0x84,0x27,0x07,0x00,0x00};
    constexpr uint8_t finishPrologue[]{0x55,0x8b,0xec,0x56,0x8b,0xf1,0x80,0xbe,0x94,0x00,0x00,0x00,0x00};
    if (!MatchesInstructions(reinterpret_cast<void*>(base+0x67f040),prologue) ||
        !MatchesInstructions(reinterpret_cast<void*>(base+0x520c97),caller) ||
        !MatchesInstructions(reinterpret_cast<void*>(base+0x67f1cb),localCopy) ||
        !MatchesInstructions(reinterpret_cast<void*>(base+0x67f0b0),modeBranch) ||
        !MatchesInstructions(reinterpret_cast<void*>(base+0x9845a0),humanGetter) ||
        !MatchesInstructions(reinterpret_cast<void*>(base+0x430f30),aiGetter) ||
        !MatchesInstructions(reinterpret_cast<void*>(base+0x1427a0),writerPrologue) ||
        !MatchesInstructions(reinterpret_cast<void*>(base+0x32a260),eligibilityPrologue) ||
        !MatchesInstructions(reinterpret_cast<void*>(base+0x1aab72),eligibilityCaller) ||
        !MatchesInstructions(reinterpret_cast<void*>(base+0x607750),finishPrologue)) {
        failed=true;lastReason="instruction_guard";WriteStatus();return;
    }
    struct Hook { uintptr_t rva;void* callback;void** trampoline; };
    const Hook hooks[]{
        {0x67f040,reinterpret_cast<void*>(Playback),reinterpret_cast<void**>(&original)},
        {0x1427a0,reinterpret_cast<void*>(WriteVehicle),reinterpret_cast<void**>(&originalWriter)},
        {0x32a260,reinterpret_cast<void*>(EligibleInput),reinterpret_cast<void**>(&originalEligibility)},
        {0x607750,reinterpret_cast<void*>(FinishAttract),reinterpret_cast<void**>(&originalFinish)}};
    // Prepare all trampolines before enabling any callback. On a partial enable
    // failure the atomic failed flag leaves every installed hook forwarding.
    MH_STATUS status=MH_OK;
    for(const auto& hook:hooks) {
        status=MH_CreateHook(reinterpret_cast<void*>(base+hook.rva),hook.callback,hook.trampoline);
        if(status!=MH_OK) break;
    }
    if(status==MH_OK) for(const auto& hook:hooks) {
        status=MH_EnableHook(reinterpret_cast<void*>(base+hook.rva));if(status!=MH_OK) break;
    }
    installed=status==MH_OK;failed=!installed;lastReason=installed?"waiting_for_native_call":"hook_failed";WriteStatus();
}
void Stop() { if (!installed) return;stopped=true;std::lock_guard lock(mutex);WriteStatus(); }
}
