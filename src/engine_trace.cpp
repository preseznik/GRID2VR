#include "engine_trace.h"
#include "guarded_hooks.h"
#include "fault_context.h"
#include "scene_call.h"
#include "replay_state.h"
#include "replay_camera.h"
#include "cockpit_exposure.h"
#include "opponent_lod_runtime.h"
#include "opponent_lod_world_policy.h"
#include "replay_presentation.h"
#include "garage_presentation.h"
#include "eye_view.h"
#include "scene_pose.h"
#include "replay_sequence.h"
#include "scene_visibility_handoff.h"
#include "visibility_dispatch.h"
#include "packet_snapshot.h"
#include "replay_packet.h"
#include "lazy_list_code.h"
#include "lazy_auxiliary_code.h"
#include "visibility_volume.h"
#include "tracked_visibility.h"
#include "eye_visibility_frustum.h"
#include "snapshot_archive.h"
#include "replay_profile.h"
#include "bounded_log.h"
#include "sign_visibility.h"
#include "instance_capacity.h"
#include "instance_observation_policy.h"
#include "headset_visibility_candidate.h"
#include "transient_refusal.h"
#include <intrin.h>
#include <MinHook.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <vector>

namespace grid2vr::engine {
namespace {
uintptr_t gameBase{};
std::filesystem::path output;
std::atomic<bool> installed{};
std::atomic<uint64_t> frame{}, captureEnd{}, serial{}, cameras{};
std::atomic<unsigned> samples{};
thread_local unsigned replayPass{};
std::mutex logMutex;
bool packedSnapshots{};
bool runtimeSession{};
bool diagnosticLogging{true};
bool nativeVisibilityControl{}; // Isolated recorded-view A/B diagnostic only.
bool trackedVisibility{}; // Isolated current-eye-volume candidate; defaults unchanged.
bool visibilityFailureSaved{};
std::atomic<bool> sessionFailureRequested{};
HistoryReset resetHistory{};
HistoryReset cancelHistory{};
SnapshotArchive snapshots;
thread_local CameraStamp currentCamera;
thread_local StageStamp currentStage;
std::atomic<bool> waitForClipTransition{},clipObserved{};
thread_local ClipTransitionTrigger clipTrigger;
BoundaryObserver observeBoundary{};
ViewProvider provideViews{};
PairObserver observePair{};
bool dynamicViews{};
SceneViewFrame frozenViews;
std::atomic<unsigned> stageSamples{};
std::array<std::atomic<uint64_t>,10> stageCalls{};
uintptr_t mainObject{},rendererObject{};
void** mainShadow{};
void** rendererShadow{};
std::array<uintptr_t,88> originalRendererTable{};
std::array<uintptr_t,39> originalMainTable{};
HANDLE faultFile=INVALID_HANDLE_VALUE;
HANDLE faultContextFile=INVALID_HANDLE_VALUE;
std::atomic<unsigned> faultContexts{};
std::array<std::atomic<uintptr_t>,16> faultSites{};
std::array<std::atomic<unsigned>,16> faultHits{};

uintptr_t Rva(const void* address) {
    MEMORY_BASIC_INFORMATION region{};
    return VirtualQuery(address,&region,sizeof(region)) && reinterpret_cast<uintptr_t>(region.AllocationBase)==gameBase
        ? reinterpret_cast<uintptr_t>(address)-gameBase : 0;
}
LONG CALLBACK ObserveException(EXCEPTION_POINTERS* exception) {
    const auto code=exception->ExceptionRecord->ExceptionCode;
    if(code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_ILLEGAL_INSTRUCTION || code==EXCEPTION_STACK_OVERFLOW)
        opponentlod::host::RevokeWorld();
    if ((code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_ILLEGAL_INSTRUCTION || code==EXCEPTION_STACK_OVERFLOW) && faultFile!=INVALID_HANDLE_VALUE) {
        const auto address=reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
        size_t site{};
        for (;site<faultSites.size();++site) {
            uintptr_t empty{};
            if (faultSites[site]==address || faultSites[site].compare_exchange_strong(empty,address)) break;
        }
        // Retain distinct fault locations even when startup repeatedly raises
        // one handled first-chance exception. This never handles an exception.
        if (site==faultSites.size() || faultHits[site].fetch_add(1)>=2) return EXCEPTION_CONTINUE_SEARCH;
        // Independent of the normal logger; continue the game's exception handling.
        const auto* c=exception->ContextRecord;
        char line[512]{};
        const auto length=sprintf_s(line,"code=%08lx address=%p game_rva=%08zx eip=%08lx esp=%08lx game_base=%08zx eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx access=%zx location=%zx frame=%llu engine_installed=%u\r\n",
            code,exception->ExceptionRecord->ExceptionAddress,Rva(exception->ExceptionRecord->ExceptionAddress),c->Eip,c->Esp,gameBase,
            c->Eax,c->Ebx,c->Ecx,c->Edx,c->Esi,c->Edi,c->Ebp,
            exception->ExceptionRecord->NumberParameters>=2?exception->ExceptionRecord->ExceptionInformation[0]:0,
            exception->ExceptionRecord->NumberParameters>=2?exception->ExceptionRecord->ExceptionInformation[1]:0,frame.load(),installed.load());
        DWORD written{};
        if (length>0) WriteFile(faultFile,line,static_cast<DWORD>(length),&written,nullptr);
        FlushFileBuffers(faultFile);
        if(faultContextFile!=INVALID_HANDLE_VALUE) {
            // Same distinct-site/hit bound as the fault log. Keep supplementary
            // attribution separate from the existing strict fault allowlist.
            MEMORY_BASIC_INFORMATION region{};wchar_t module[MAX_PATH]{};
            if(VirtualQuery(exception->ExceptionRecord->ExceptionAddress,&region,sizeof(region)) && region.Type==MEM_IMAGE)
                GetModuleFileNameW(static_cast<HMODULE>(region.AllocationBase),module,MAX_PATH);
            char detail[1024]{};
            const auto bytes=sprintf_s(detail,"eip=%08lx allocation=%p offset=%zx module=%ls\r\n",c->Eip,region.AllocationBase,
                address-reinterpret_cast<uintptr_t>(region.AllocationBase),module);
            if(bytes>0)WriteFile(faultContextFile,detail,static_cast<DWORD>(bytes),&written,nullptr);
            FlushFileBuffers(faultContextFile);
            // Preserve the caller of an invalid indirect target at Quit. Keep
            // the original fault format/allowlist and exception handling intact.
            if (code==EXCEPTION_ACCESS_VIOLATION && (installed || instancecapacity::Enabled()) && faultContexts.fetch_add(1)<4)
                WriteFaultContext(faultContextFile,*c);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void Log(const char* format, ...) {
    profile::Scope measure(profile::EngineLog);
    char text[2048]{};
    va_list args; va_start(args,format); vsnprintf_s(text,sizeof(text),_TRUNCATE,format,args); va_end(args);
    if (runtimeSession && !diagnosticLogging && !strstr(text,"session ") && !strstr(text,"refused") &&
        !strstr(text,"failed") && !strstr(text,"installed") && !strstr(text,"configured") && !strstr(text,"requested")) return;
    std::lock_guard lock(logMutex);
    static BoundedLog file(output/L"engine.log",runtimeSession?sessionLogSegmentBytes:0);
    file << GetTickCount64() << " tid=" << GetCurrentThreadId() << " frame=" << frame.load() << " pass=" << replayPass << ' ' << text << '\n';
    if (runtimeSession && (strstr(text,"session ") || strstr(text,"refused") || strstr(text,"failed"))) file.FlushNow();
    else file.flush();
}
template<class T> T Read(uintptr_t address) {
    T value{};
    return ReadMemory(reinterpret_cast<void*>(address),&value,sizeof(value)) ? value : T{};
}
using InstanceAllocateFn=uintptr_t (__thiscall*)(void*,int,int,uint32_t*);
InstanceAllocateFn realInstanceAllocate{};
instancecapacity::AllocationObservationPolicy instanceObservationPolicy;
constexpr uint8_t instanceAllocateEntry[]{0x55,0x8b,0xec,0x33,0xc0,0x38,0x01,0x74,0x40,0x8b,0x55,0x08};
std::atomic<uint64_t> instanceRequests{},instanceFailures{};
std::mutex instanceLogMutex;
std::ofstream instanceLog;
uintptr_t __fastcall ObserveInstanceAllocate(void* self,void*,int lane,int count,uint32_t* start) {
    const bool caller=reinterpret_cast<uintptr_t>(_ReturnAddress())==gameBase+0x28ba78;
    const auto result=realInstanceAllocate(self,lane,count,start);
    if(!caller)return result;
    const auto manager=Read<uintptr_t>(mainObject+0xd4);
    if(!manager || Read<uintptr_t>(manager+0xf8)!=reinterpret_cast<uintptr_t>(self))return result;
    ++instanceRequests;
    instancecapacity::Demand(frame.load(),lane,count);
    if(result)return result;
    const auto failure=instanceFailures.fetch_add(1);
    if(failure>=64)return result;
    // This native function does not change allocator state on a NULL return.
    // Keep the actual result separate from inferred reasons and pending workers.
    std::array<uint32_t,6> header{};std::array<int32_t,4> state{};
    const bool known=ReadMemory(self,header.data(),sizeof(header));
    const bool laneKnown=known && lane>=0 && lane<static_cast<int32_t>(header[5]) && lane<16 &&
        header[4] && header[4]<=UINT32_MAX-16*(lane+1) &&
        ReadMemory(reinterpret_cast<void*>(header[4]+16*lane),state.data(),sizeof(state));
    std::lock_guard lock(instanceLogMutex);
    instanceLog << "{\"type\":\"allocation_null\",\"index\":" << failure << ",\"frame\":" << frame.load()
        << ",\"thread\":" << GetCurrentThreadId() << ",\"manager\":" << manager << ",\"allocator\":" << uintptr_t(self)
        << ",\"lane\":" << lane << ",\"requested\":" << count << ",\"headerKnown\":" << known
        << ",\"enabled\":" << (header[0]&255) << ",\"stride\":" << header[3] << ",\"laneCount\":" << header[5]
        << ",\"laneKnown\":" << laneKnown << ",\"capacity\":" << state[2] << ",\"used\":" << state[3] << "}\n";
    instanceLog.flush();return result;
}
bool InstallInstanceObservation() {
    // This changes executable bytes. Never enable it implicitly for live play.
    if(instanceObservationPolicy.Suppression())return false;
    constexpr uint8_t limit[]{0x03,0xdf,0x3b,0x5c,0x16,0x08,0x7d,0x19};
    constexpr uint8_t call[]{0xe8,0xc8,0x9e,0xef,0xff,0x85,0xc0,0x74,0x60,0x8b,0x7c,0xbb,0x04};
    auto* target=reinterpret_cast<void*>(gameBase+0x185940);
    if(!MatchesInstructions(target,instanceAllocateEntry) || !MatchesInstructions(reinterpret_cast<void*>(gameBase+0x185965),limit) ||
       !MatchesInstructions(reinterpret_cast<void*>(gameBase+0x28ba73),call))return false;
    instanceLog.open(output/L"instance-allocation.jsonl",std::ios::binary|std::ios::trunc);
    if(!instanceLog)return false;
    instanceLog << "{\"type\":\"begin\",\"maxFailureRecords\":64,\"callerRva\":2669176,\"readOnly\":true,\"nativeCodeModified\":true,\"isolatedAiTelemetry\":true}\n";
    instanceLog.flush();
    if(MH_CreateHook(target,reinterpret_cast<void*>(ObserveInstanceAllocate),reinterpret_cast<void**>(&realInstanceAllocate))!=MH_OK)return false;
    if(MH_EnableHook(target)!=MH_OK){MH_RemoveHook(target);realInstanceAllocate=nullptr;return false;}
    return true;
}
uint64_t Dump(uint64_t id,const char* phase,uintptr_t context) {
    profile::Scope measure(profile::Snapshot);
    std::array<uint8_t,512> data{};
    if (!ReadMemory(reinterpret_cast<void*>(context),data.data(),data.size())) return 0;
    uint64_t hash=14695981039346656037ull;
    for (auto byte:data) { hash^=byte; hash*=1099511628211ull; }
    if (runtimeSession) return hash;
    char name[96]{}; sprintf_s(name,"engine-%llu-camera-%s.bin",id,phase);
    if (packedSnapshots) return snapshots.Write(output,name,data.data(),data.size())?hash:0;
    std::ofstream file(output/name,std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()),data.size());
    return file.good() ? hash : 0;
}
using CameraFn=void (__thiscall*)(void*,uintptr_t);
CameraFn realCamera{};
void ApplyReplayProjection(void* self,uintptr_t context,uintptr_t caller);
void __fastcall Camera(void* self,void*,uintptr_t context) {
    ++cameras;
    const auto id=++serial;
    currentCamera={id,context};
    ApplyReplayProjection(self,context,Rva(_ReturnAddress()));
    const bool record=!runtimeSession && frame.load()<captureEnd.load() && samples.fetch_add(1)<96;
    uint64_t before{};
    if (record) {
        before=Dump(id,"before",context);
        const auto renderer=reinterpret_cast<uintptr_t>(self);
        const bool valid=renderer==rendererObject && Read<void**>(renderer)==rendererShadow;
        Log("camera id=%llu self=%p context=%p caller_rva=0x%zx renderer_valid=%u stereo=%u left=%u stereo_width=%u vendor_handle=%p before=%016llx",
            id,self,reinterpret_cast<void*>(context),Rva(_ReturnAddress()),valid,
            valid?Read<uint8_t>(renderer+0x512e):255,valid?Read<uint8_t>(renderer+0x512c):255,
            valid?Read<uint32_t>(renderer+0x5118):0,reinterpret_cast<void*>(valid?Read<uintptr_t>(renderer+0x5130):0),before);
    }
    realCamera(self,context); // Exactly once, with the engine instructions intact.
    if (record) Log("camera exit id=%llu before=%016llx after=%016llx",id,before,Dump(id,"after",context));
}

// The constructor installs this table at RVA 0x8104d6. Its original entries
// stay intact; only the confirmed live object's table pointer changes.
constexpr uintptr_t mainTable=0xcd9da8;
constexpr std::array<uintptr_t,10> mainMethods{
    0x70c920,0xa06d0,0x2c2950,0x689da0,0x1f4070,
    0x77bfc0,0x56fe00,0x64e4b0,0xf65c0,0x5afd80};
using SingleFn=void (__thiscall*)(void*,uintptr_t);
using SceneFn=void (__thiscall*)(void*,uintptr_t,uintptr_t,uintptr_t);
SingleFn realPrepare{},realPacket{};
SceneFn realStage5{},realStage6{};
using PredicateFn=bool (__thiscall*)(void*);
PredicateFn realPredicate{};
bool sceneBoundaryEnabled{};
bool replayEnabled{};
bool traceReplayWork{};
bool traceVisibility{};
bool expandVisibility{};
std::atomic<bool> visibilityHealthy{true};
std::atomic<uint64_t> visibilityReadyFrame{};
std::atomic<uint64_t> visibilityConsumerFrame{};
std::atomic<uint64_t> opponentVisibilityFrame{};
uint64_t visibilityGapSuspensions{};
// Pre-write camera/visibility refusals that suspended instead of failing.
const char* transientRefusal{};
const char* lastTransientRefusal="none";
const char* visibilityRefusedReason{};
uint64_t visibilityRefusedFrame{},transientRefusalFrame{},transientSuspensions{};
uintptr_t visibilityConsumer{};
DWORD renderThread{};
void** visibilityShadow{};
std::array<uintptr_t,30> originalVisibilityTable{};
using VisibilityConsumerFn=void (__thiscall*)(void*,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
using FrustumBuildFn=void* (__thiscall*)(void*,const void*);
VisibilityConsumerFn realVisibilityConsumer{};
using VisibilityResetFn=uintptr_t (__thiscall*)(void*);
VisibilityResetFn realVisibilityReset{};
void** visibilityEventShadow{};
uint64_t alternateVisibilityCalls{},alternateVisibilityPairs{};
struct PendingVisibility {
    uint64_t id{},frame{};
    DWORD thread{};
    bool active{};
    VisibilityVolume original{},expanded{};
};
thread_local PendingVisibility pendingVisibility;
bool eyeOffsetEnabled{};
bool headsetProjection{};
std::atomic<bool> eyeHistoryPrimed{};
ReplaySequence replaySequence;
thread_local bool inReplay{};
struct PendingScene {
    uint64_t id{},frame{};
    SceneCall call;
    bool valid{},stage6Seen{};
    uintptr_t manager{},renderData{};
    uint32_t counter{};
    uint32_t nearPlane{},farPlane{};
    HistoryState history{};
    SceneView view{};
    CameraContext context{};
    bool replayInputsSaved{};
};
thread_local PendingScene pendingScene;
bool lazyListsVerified{};
bool deferredNativeHooks{},instanceObservationRequested{};
thread_local EmptyListWitness emptyListWitness;
thread_local EmptyListReplay emptyListReplay;
thread_local bool replayedEmptyBuilder{};
uint64_t emptyBuilderPairs{},lastEmptyBuilderFrame{};
bool WritableHeap(uintptr_t address,size_t bytes);
bool ReadLazyMemory(uintptr_t address,void* bytes,size_t size) {
    return ReadMemory(reinterpret_cast<void*>(address),bytes,size);
}
LazyListIdentity LazyIdentity(uintptr_t packet,uintptr_t builder) {
    return {pendingScene.id,frame.load(),GetCurrentThreadId(),packet,builder};
}
bool LazyInstructionsMatch() {
    if (!LazyListCodeMatches(gameBase,ReadLazyMemory) || !LazyAuxiliaryCodeMatches(gameBase,ReadLazyMemory)) return false;
    for(const auto& site:nativeLazyListCallSites)
        if(!LazyListCallerMatches(gameBase,site.returnRva,ReadLazyMemory)) return false;
    return true;
}
bool VerifyLazyLists() {
    // These bytes feed a native executable-memory seed during event loading.
    // Keep the routine and all four call gates original for the entire process.
    lazyListsVerified=LazyInstructionsMatch();
    Log("lazy lists configured=%u mode=completed_scene_heap_gate native_code_unchanged=1",lazyListsVerified);
    return lazyListsVerified;
}
bool ExchangeLazyByte(uintptr_t address,uint8_t expected,uint8_t desired) {
    __try {
        return static_cast<uint8_t>(_InterlockedCompareExchange8(reinterpret_cast<volatile char*>(address),
            static_cast<char>(desired),static_cast<char>(expected)))==expected;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ExchangeLazyEnabled(uintptr_t address,uint8_t expected,uint8_t desired) {
    const auto& witness=emptyListWitness;
    const auto& identity=witness.identity;
    if(!witness.healthy || !identity.builder || address!=identity.builder+0x68 ||
        GetCurrentThreadId()!=renderThread || identity.thread!=renderThread ||
        Read<void**>(mainObject)!=mainShadow || Read<uintptr_t>(mainObject+0xe4)!=identity.builder ||
        !WritableHeap(identity.builder,witness.state.object.size())) return false;
    std::array<uint8_t,0x6c> actual{};
    if(!ReadLazyMemory(identity.builder,actual.data(),actual.size())) return false;
    // Acquisition requires the entire witnessed object. Restoration still
    // restores our byte if another field changed, but must not touch a replaced
    // builder or tree-owner identity. The full postcheck then rejects the eye.
    if(desired==0) {
        if(actual!=witness.state.object || frame.load()!=identity.frame || !pendingScene.valid ||
            pendingScene.id!=identity.scene || pendingScene.frame!=identity.frame) return false;
        LazyListGate gate{};LazyListState state{};ScenePacket packet{};
        if(ReadLazyListGate(gameBase,mainObject,ReadLazyMemory,gate) ||
            !ReadLazyListState(identity.builder,ReadLazyMemory,state) ||
            !ReadLazyMemory(identity.packet,packet.data(),packet.size()) ||
            !witness.Matches(LazyIdentity(identity.packet,identity.builder),packet,gate,state)) return false;
    } else if(memcmp(actual.data()+0x34,witness.state.object.data()+0x34,12)) return false;
    return ExchangeLazyByte(address,expected,desired);
}
void ObserveCompletedEmptyLists() {
    LazyListGate gate{};LazyListState state{};ScenePacket packet{};
    const bool completed=pendingScene.valid && pendingScene.stage6Seen && pendingScene.replayInputsSaved &&
        pendingScene.frame==frame.load() &&
        GetCurrentThreadId()==renderThread && Read<void**>(mainObject)==mainShadow;
    const bool readable=lazyListsVerified && LazyInstructionsMatch() &&
        !ReadLazyListGate(gameBase,mainObject,ReadLazyMemory,gate) &&
        ReadLazyMemory(pendingScene.call.args[0],packet.data(),packet.size()) &&
        (!gate.Open() || ReadLazyListState(gate.builder,ReadLazyMemory,state));
    emptyListWitness.Observe(LazyIdentity(pendingScene.call.args[0],gate.builder),completed,readable,packet,gate,state);
}
// A destructor alone would not run for a native SEH fault with /EHsc. Keep this
// small finally block free of C++ objects, restore the byte before any postcheck,
// and preserve the game's exception handling rather than swallowing the fault.
bool InvokeReplayScene(const SceneCall& call,bool emptyPacket,const ScenePacket& packet,
                       const LazyListGate& gate,const LazyListState& state) {
    if(emptyPacket && !emptyListReplay.Begin(LazyIdentity(call.args[0],gate.builder),packet,gate,state,
                                           emptyListWitness,ExchangeLazyEnabled)) return false;
    __try {
        inReplay=true;
        InvokeCompleteScene(reinterpret_cast<CompleteSceneFn>(gameBase+0x3be050),reinterpret_cast<void*>(mainObject),call);
    } __finally {
        emptyListReplay.Release(ExchangeLazyEnabled);
        inReplay=false;
        if(AbnormalTermination()) emptyListReplay.Cancel();
    }
    return true;
}
struct EyeCameraSample {
    uint64_t frame{};
    uintptr_t manager{},context{};
    CameraContext camera{};
    SceneView view{};
    bool valid{};
};
thread_local EyeCameraSample currentEyeCamera,previousEyeCamera;
uint64_t menuSceneFrame{};
bool menuScene{};
bool menuPresentation{};
bool replay3d{true},replaySceneBlocked{},garage3d{true},garageSceneBlocked{},frontendSceneAudit{};
GarageMenuSnapshot garageMenu;
std::atomic<uint64_t> frontendPrepareCalls{},frontendVisibilityCalls{},frontendWorldVisibilityCalls{};
std::mutex frontendAuditMutex;
std::array<uintptr_t,16> frontendVisibilityCallers{};
unsigned frontendVisibilityCallerCount{};
void UpdateGaragePresentation() {
    if(garage3d && !frontendSceneAudit)return;
    const auto previous=garageMenu.state;
    garageMenu=ReadGarageMenu(gameBase,[](uintptr_t at,void* value,size_t size) {
        return ReadMemory(reinterpret_cast<void*>(at),value,size);
    });
    garageSceneBlocked=!GaragePresentationAllowed(garage3d,garageMenu.state);
    if(previous!=garageMenu.state)
        Log("garage presentation frame=%llu state=%u disabled=%u object=%p active_states=%u",
            frame.load(),unsigned(garageMenu.state),garageSceneBlocked,reinterpret_cast<void*>(garageMenu.object),garageMenu.count);
    if(!frontendSceneAudit)return;
    static unsigned count{};static uint64_t previousFrame{};
    if(count>=32 || (count && previous==garageMenu.state && (count>=16 || frame.load()<previousFrame+600)))return;
    std::ofstream out(output/L"frontend-garage.jsonl",count?std::ios::app:std::ios::trunc);
    ++count;previousFrame=frame.load();
    out << "{\"frame\":" << frame.load() << ",\"state\":" << unsigned(garageMenu.state)
        << ",\"disabled\":" << (garageSceneBlocked?"true":"false")
        << ",\"application\":" << garageMenu.application << ",\"frontend\":" << garageMenu.frontend
        << ",\"wrapper\":" << garageMenu.wrapper << ",\"manager\":" << garageMenu.manager
        << ",\"registry\":" << garageMenu.registry << ",\"list\":" << garageMenu.list
        << ",\"data\":" << garageMenu.data << ",\"count\":" << garageMenu.count << ",\"capacity\":" << garageMenu.capacity
        << ",\"object\":" << garageMenu.object << ",\"context\":" << garageMenu.context
        << ",\"readOnly\":true,\"source\":\"active_state_list\"}\n";
}
thread_local std::array<RenderedEyeHistory,2> renderedEyeHistories;
bool ownedEyeHistory{};
float eyeYaw{};
bool fullEyePose{};
ScenePose eyePose;
bool dualEyes{};
std::array<ScenePose,2> stereoPoses;
constexpr EyeFrustum stereoRightFrustum{-.8f,1.f,-.7f,.6f};
const ScenePose& CurrentPose() {
    if (const auto* views=frozenViews.Get(frame.load())) return views->poses[replayPass==2?1:0];
    return dualEyes ? stereoPoses[replayPass==2?1:0] : eyePose;
}
std::array<EyeFrustum,2> CurrentFrusta() {
    if (const auto* views=frozenViews.Get(frame.load())) return views->frusta;
    return {diagnosticFrustum,dualEyes?stereoRightFrustum:diagnosticFrustum};
}
const EyeFrustum& CurrentFrustum() {
    if (const auto* views=frozenViews.Get(frame.load())) return views->frusta[replayPass==2?1:0];
    return dualEyes && replayPass==2 ? stereoRightFrustum : diagnosticFrustum;
}
bool asymmetricEyeFov{};
thread_local bool eyeProjectionHealthy{};
thread_local unsigned eyeProjectionUploads{};
thread_local CameraContext replayMainCamera;
thread_local bool replayMainCameraValid{};
thread_local unsigned replayMainSamples{};

bool DumpStage(uint64_t id,const char* phase,const char* field,uintptr_t address,size_t size) {
    profile::Scope measure(profile::Snapshot);
    const auto namePhase=SnapshotPhase(phase);phase=namePhase.c_str();
    std::array<uint8_t,512> data{};
    if (!address || size>data.size() || !ReadMemory(reinterpret_cast<void*>(address),data.data(),size)) {
        Log("stage dump failed id=%llu phase=%s field=%s",id,phase,field); return false;
    }
    if (runtimeSession) return true; // The memory guard is independent of snapshot I/O.
    char name[96]{}; sprintf_s(name,"stage-%llu-%s-%s.bin",id,phase,field);
    if (packedSnapshots) return snapshots.Write(output,name,data.data(),size);
    std::ofstream file(output/name,std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()),size);
    return file.good();
}

void SnapshotVisibility(uint64_t id,const char* phase) {
    if (runtimeSession) return;
    if (!traceVisibility || Read<void**>(mainObject)!=mainShadow) return;
    const auto context=Read<uintptr_t>(mainObject+0x5e4);
    const auto consumer=Read<uintptr_t>(mainObject+0x30);
    const auto table=consumer?Read<uintptr_t>(consumer):0;
    const auto originalTable=visibilityShadow && table==reinterpret_cast<uintptr_t>(visibilityShadow) ? gameBase+0xcd9bb8 : table;
    const bool saved=DumpStage(id,phase,"visibility-volume",mainObject+0x330,224) &&
        DumpStage(id,phase,"visibility-view",mainObject+0x5f0,0x90) &&
        DumpStage(id,phase,"render-view",mainObject+0x680,0x90) &&
        DumpStage(id,phase,"visibility-context",context,0x200);
    const bool consumerSaved=consumer && DumpStage(id,phase,"visibility-consumer",consumer,0x80) &&
        DumpStage(id,phase,"visibility-consumer-table",table,0x40);
    Log("visibility snapshot id=%llu phase=%s saved=%u context=%p consumer=%p table_rva=0x%zx method9_rva=0x%zx consumer_saved=%u active=%u enabled=%u mode=%u",
        id,phase,saved,reinterpret_cast<void*>(context),reinterpret_cast<void*>(consumer),
        Rva(reinterpret_cast<void*>(originalTable)),Rva(reinterpret_cast<void*>(originalTable?Read<uintptr_t>(originalTable+0x24):0)),
        consumerSaved,Read<uint8_t>(mainObject+0x138),Read<uint8_t>(mainObject+0x51),Read<uint32_t>(mainObject+0x9e4));
}

struct StageScope {
    StageStamp previous;
    uintptr_t self,context,view;
    bool record;
    StageScope(unsigned slot,void* object,uintptr_t a0,uintptr_t a1,uintptr_t a2,void* caller)
        : previous(currentStage),self(reinterpret_cast<uintptr_t>(object)),
          context(slot==5 || slot==6 ? a0:0),view(slot==5 || slot==6 ? a1:0),
          record(!runtimeSession && frame.load()<captureEnd.load() && stageSamples.fetch_add(1)<48) {
        ++stageCalls[slot];
        currentStage={++serial,slot,self};
        if (!record) return;
        const bool valid=self==mainObject && Read<void**>(self)==mainShadow;
        Log("stage enter id=%llu slot=%u self=%p valid=%u type=0x%x caller_rva=0x%zx a0=%p a1=%p a2=%p target_object=%p parent=%llu",
            currentStage.id,slot,object,valid,valid?Read<uint32_t>(self+0x14):0,Rva(caller),
            reinterpret_cast<void*>(a0),reinterpret_cast<void*>(a1),reinterpret_cast<void*>(a2),
            reinterpret_cast<void*>(valid?Read<uintptr_t>(self+0x94):0),previous.id);
        Snapshot("before");
    }
    void Snapshot(const char* phase) {
        // Bounded windows only, not a complete or restorable engine state.
        DumpStage(currentStage.id,phase,"object",self,0x160);
        if (context) DumpStage(currentStage.id,phase,"context",context,0x200);
        if (view) DumpStage(currentStage.id,phase,"view",view,0x90);
        if (observeBoundary) observeBoundary(currentStage,phase);
    }
    ~StageScope() {
        if (record) {
            Snapshot("after");
            Log("stage exit id=%llu slot=%u",currentStage.id,currentStage.slot);
        }
        currentStage=previous;
    }
};
bool SceneSnapshot(const PendingScene& scene,const char* phase) {
    if (runtimeSession) {
        const bool observed=observeBoundary && observeBoundary({scene.id,100,mainObject},phase);
        signvisibility::Snapshot(scene.frame,scene.id,replayPass,phase,mainObject,scene.call.args[0]);
        return observed;
    }
    const auto namePhase=SnapshotPhase(phase);
    DumpStage(scene.id,phase,"object",mainObject,0x160);
    DumpStage(scene.id,phase,"packet",scene.call.args[0],0x128);
    DumpStage(scene.id,phase,"view",scene.call.args[2],0x90);
    DumpStage(scene.id,phase,"context",scene.call.args[3],0x200);
    const auto manager=Read<uintptr_t>(mainObject+0x2c);
    const auto renderData=Read<uintptr_t>(mainObject+0x710);
    const bool managerValid=Read<uintptr_t>(manager)==gameBase+0xcf8108;
    const bool managerSaved=managerValid && DumpStage(scene.id,phase,"manager",manager,0x200);
    const bool renderDataSaved=DumpStage(scene.id,phase,"render-data",renderData,0x200);
    if (traceReplayWork && replaySequence.Completed()==0 && (strcmp(phase,"scene-complete")==0 || strcmp(phase,"replay-complete")==0)) {
        const std::array<uint32_t,3> roots{Read<uint32_t>(scene.call.args[0]+0x84),Read<uint32_t>(scene.call.args[0]+0x88),Read<uint32_t>(scene.call.args[0]+0xe4)};
        const auto snapshot=SnapshotPacketLists(roots,[](uint32_t address,void* bytes,size_t size) { return ReadMemory(reinterpret_cast<void*>(address),bytes,size); });
        char name[96]{}; sprintf_s(name,"stage-%llu-%s-nodes.bin",scene.id,namePhase.c_str());
        std::ofstream file(output/name,std::ios::binary);
        file.write(reinterpret_cast<const char*>(snapshot.nodes.data()),snapshot.nodes.size()*sizeof(PacketNode)); file.close();
        Log("packet nodes id=%llu phase=%s count=%zu complete=%u saved=%u roots=%08x,%08x,%08x",
            scene.id,namePhase.c_str(),snapshot.nodes.size(),snapshot.complete,file.good(),roots[0],roots[1],roots[2]);
    }
    Log("scene state id=%llu phase=%s manager=%p manager_valid=%u manager_saved=%u render_data=%p render_data_saved=%u",
        scene.id,namePhase.c_str(),reinterpret_cast<void*>(manager),managerValid,managerSaved,reinterpret_cast<void*>(renderData),renderDataSaved);
    return observeBoundary && observeBoundary({scene.id,100,mainObject},phase);
}
void RememberScene(unsigned slot,void* self,uintptr_t context,uintptr_t view,uintptr_t packet,const void* returnSlot) {
    if (inReplay || !sceneBoundaryEnabled || reinterpret_cast<uintptr_t>(self)!=mainObject) return;
    const bool record=runtimeSession?replaySequence.Available():frame.load()<captureEnd.load();
    const bool rememberEye=eyeOffsetEnabled && (replaySequence.Available() ||
        (runtimeSession && replaySequence.Status()==ReplaySequence::State::Suspended));
    const bool rememberClip=waitForClipTransition && !clipObserved;
    if (!record && !rememberEye && !rememberClip) return;
    std::array<uintptr_t,15> stack{};
    SceneCall call;
    const bool valid=ReadMemory(returnSlot,stack.data(),sizeof(stack)) &&
        DecodeSceneCall(stack,gameBase,slot,context,view,packet,call);
    if (slot==5 && rememberClip) {
        // Arm from the decoded main scene, not arbitrary camera uploads.
        // Stage 6 also uploads auxiliary views with unrelated clip distances.
        clipTrigger={};
        if (valid && GetCurrentThreadId()==renderThread)
            clipTrigger.Observe(frame.load(),context,Read<uint32_t>(context+0xe8),Read<uint32_t>(context+0xec));
    }
    if (slot==5 && rememberEye) {
        previousEyeCamera=currentEyeCamera;
        currentEyeCamera={frame.load(),Read<uintptr_t>(mainObject+0x2c),context};
        currentEyeCamera.valid=valid && Read<uintptr_t>(currentEyeCamera.manager)==gameBase+0xcf8108 &&
            Read<uint32_t>(currentEyeCamera.manager+0xa0)==0 &&
            ReadMemory(reinterpret_cast<void*>(context),currentEyeCamera.camera.data(),currentEyeCamera.camera.size()) &&
            ReadMemory(reinterpret_cast<void*>(view),currentEyeCamera.view.data(),currentEyeCamera.view.size()) &&
            currentEyeCamera.view[0x8c]==1;
        if (currentEyeCamera.valid) eyeHistoryPrimed=true;
    }
    if (!record) return;
    if (slot==5) {
        pendingScene={++serial,frame.load(),call,valid,false};
        emptyListWitness={};
        if (valid && replayEnabled && replaySequence.Available()) {
            pendingScene.manager=Read<uintptr_t>(mainObject+0x2c);
            pendingScene.renderData=Read<uintptr_t>(mainObject+0x710);
            pendingScene.counter=Read<uint32_t>(mainObject+0xc8);
            pendingScene.nearPlane=Read<uint32_t>(mainObject+0x40);
            pendingScene.farPlane=Read<uint32_t>(mainObject+0x44);
            pendingScene.replayInputsSaved=Read<uintptr_t>(pendingScene.manager)==gameBase+0xcf8108 &&
                ReadMemory(reinterpret_cast<void*>(pendingScene.manager),pendingScene.history.data(),pendingScene.history.size()) &&
                ReadMemory(reinterpret_cast<void*>(view),pendingScene.view.data(),pendingScene.view.size()) &&
                ReadMemory(reinterpret_cast<void*>(context),pendingScene.context.data(),pendingScene.context.size());
        }
        DumpStage(pendingScene.id,"scene-entry","stack",reinterpret_cast<uintptr_t>(returnSlot),sizeof(stack));
        Log("scene candidate id=%llu valid=%u packet=%p view=%p context=%p arg1=%p arg4=%p flag=%u",
            pendingScene.id,valid,reinterpret_cast<void*>(call.args[0]),reinterpret_cast<void*>(call.args[2]),
            reinterpret_cast<void*>(call.args[3]),reinterpret_cast<void*>(call.args[1]),reinterpret_cast<void*>(call.args[4]),
            static_cast<unsigned>(call.args[5]));
        if (valid) SceneSnapshot(pendingScene,"scene-entry");
    } else {
        pendingScene.stage6Seen=valid && pendingScene.valid && pendingScene.frame==frame.load() && call.args==pendingScene.call.args;
    }
}
bool WritableHeap(uintptr_t address,size_t bytes) {
    MEMORY_BASIC_INFORMATION region{};
    return VirtualQuery(reinterpret_cast<void*>(address),&region,sizeof(region)) &&
        region.State==MEM_COMMIT && region.Type==MEM_PRIVATE && region.Protect==PAGE_READWRITE &&
        address>=reinterpret_cast<uintptr_t>(region.BaseAddress) &&
        bytes<=region.RegionSize-(address-reinterpret_cast<uintptr_t>(region.BaseAddress));
}
bool VisibilityInstructionsMatch() {
    constexpr uint8_t consumer[]{0x53,0x8b,0xdc,0x83,0xec,0x08,0x83,0xe4,0xf0,0x83,0xc4,0x04,0x55,0x8b,0x6b,0x04,
        0x89,0x6c,0x24,0x04,0x8b,0xec,0x81,0xec,0x78,0x01,0,0};
    constexpr uint8_t caller[]{0x8d,0x86,0xf0,0x05,0,0,0x50,0x8b,0x86,0xe4,0x05,0,0,0x50,0x8b,0x86,0xa8,0,0,0,0x50,0xff,0xd2};
    constexpr uint8_t build[]{0x53,0x8b,0xdc,0x83,0xec,0x08,0x83,0xe4,0xf0,0x83,0xc4,0x04,0x55,0x8b,0x6b,0x04,
        0x89,0x6c,0x24,0x04,0x8b,0xec,0x81,0xec,0x0c,0x01,0,0};
    return MatchesInstructions(reinterpret_cast<void*>(gameBase+0x28d2f0),consumer) &&
        MatchesInstructions(reinterpret_cast<void*>(gameBase+0x11769d),caller) &&
        MatchesInstructions(reinterpret_cast<void*>(gameBase+0x87d860),build);
}
bool RestoreVisibility(const char* reason) {
    auto& pending=pendingVisibility;
    if (!pending.active) return true;
    VisibilityVolume actual{};
    const bool valid=pending.thread==GetCurrentThreadId() && Read<void**>(mainObject)==mainShadow &&
        WritableHeap(mainObject+0x330,224) && ReadMemory(reinterpret_cast<void*>(mainObject+0x330),actual.data(),224) &&
        actual==pending.expanded;
    bool restored=false;
    if (valid) {
        memcpy(reinterpret_cast<void*>(mainObject+0x330),pending.original.data(),224);
        restored=ReadMemory(reinterpret_cast<void*>(mainObject+0x330),actual.data(),224) && actual==pending.original;
        DumpStage(pending.id,"visibility-restored","volume",mainObject+0x330,224);
    }
    Log("visibility restore id=%llu source_frame=%llu restored=%u reason=%s",pending.id,pending.frame,restored,reason);
    pending.active=false;
    if (!restored) {visibilityHealthy=false;opponentlod::host::RevokeWorld();}
    return restored;
}
bool PrepareVisibility(bool valid,uintptr_t view,uintptr_t caller) {
    if(!valid) opponentlod::host::RevokeWorld();
    const bool window=runtimeSession?replaySequence.Available():frame.load()<captureEnd.load();
    {
        // Record attempts too: an invalid callback must not look like loading.
        visibilityConsumerFrame=frame.load();
        if (!window) {
            // Loading/menu cameras can prime camera history without running
            // race visibility. Arm only after a valid native volume was used
            // in the immediately preceding frame; do not modify it here.
            VisibilityVolume volume{};SceneView source{};VisibilityBox readyBox;
            if (valid && ReadMemory(reinterpret_cast<void*>(mainObject+0x330),volume.data(),volume.size()) &&
                ReadMemory(reinterpret_cast<void*>(view),source.data(),source.size()) && MakeVisibilityBox(volume,source,readyBox))
                visibilityReadyFrame=frame.load();
            return false;
        }
        const auto id=++serial;
        if (dynamicViews && replaySequence.Available()) {
            SceneViews views;
            const auto provided=valid && provideViews?provideViews(frame.load(),views):ViewResult::Failed;
            if (runtimeSession && provided==ViewResult::Unavailable) {
                SuspendSession();
                return false;
            }
            const bool frozen=provided==ViewResult::Ready && frozenViews.Freeze(frame.load(),views);
            if (!frozen) {visibilityHealthy=false;opponentlod::host::RevokeWorld();}
            if (frozen) signvisibility::RememberViews(frame.load(),views);
            Log("views frozen valid=%u index=%llu",frozen,replaySequence.Attempts()+1);
            if (frozen) for (unsigned eye=0;eye<2;++eye) {
                const auto& p=views.poses[eye];const auto& f=views.frusta[eye];
                Log("views input eye=%u qx=%.9g qy=%.9g qz=%.9g qw=%.9g x=%.9g y=%.9g z=%.9g left=%.9g right=%.9g down=%.9g up=%.9g",
                    eye,p.orientation[0],p.orientation[1],p.orientation[2],p.orientation[3],p.position[0],p.position[1],p.position[2],f.left,f.right,f.down,f.up);
            }
        }
        SnapshotVisibility(id,"before-jobs");
        signvisibility::Snapshot(frame.load(),id,0,"visibility-before",mainObject,0);
        bool applied=false;
        VisibilityBox box;
        if (expandVisibility && visibilityHealthy && replaySequence.Available()) {
            alignas(16) VisibilityVolume original{},expanded{};
            alignas(16) std::array<float,16> visibilityMatrix{};
            SceneView sourceView{},renderView{};
            const char* failure="write_readback";
            const auto check=[&](bool ok,const char* reason){if(!ok)failure=reason;return ok;};
            const auto* views=trackedVisibility?frozenViews.Get(frame.load()):nullptr;
            bool prepared=check(valid,"native_guard") && check(!pendingVisibility.active,"pending_volume") &&
                check(WritableHeap(mainObject+0x330,224),"volume_not_writable") &&
                check(ReadMemory(reinterpret_cast<void*>(mainObject+0x330),original.data(),224),"volume_read") &&
                check(ReadMemory(reinterpret_cast<void*>(view),sourceView.data(),sourceView.size()),"visibility_view_read") &&
                check(MakeVisibilityBox(original,sourceView,box),"native_volume_geometry");
            if (prepared && trackedVisibility) {
                EyeVisibilityFrustum trackedFrustum;
                prepared=check(views!=nullptr,"frozen_views") &&
                    check(ReadMemory(reinterpret_cast<void*>(mainObject+0x680),renderView.data(),renderView.size()),"render_view_read") &&
                    check(MakeEyeVisibilityFrustum(original,sourceView,renderView,*views,trackedFrustum),"eye_frustum_construction");
                if (prepared) {
                    visibilityMatrix=trackedFrustum.matrix;
                    reinterpret_cast<FrustumBuildFn>(gameBase+0x87d860)(expanded.data(),visibilityMatrix.data());
                    prepared=ValidateEyeVisibilityFrustum(expanded,trackedFrustum);
                    if (!prepared) {
                        EyeVisibilityFrustum local;
                        alignas(16) VisibilityVolume nativeLocal{},translated{};
                        if (MakeLocalEyeVisibilityFrustum(trackedFrustum,local)) {
                            reinterpret_cast<FrustumBuildFn>(gameBase+0x87d860)(nativeLocal.data(),local.matrix.data());
                            prepared=RestoreLocalEyeVisibilityVolume(nativeLocal,trackedFrustum,translated);
                            if (prepared) expanded=translated;
                        }
                        Log("visibility local_reconstruction id=%llu valid=%u",id,prepared);
                    }
                    prepared=check(prepared,"native_frustum_readback");
                    Log("visibility tracked_frustum id=%llu valid=%u near=%.9g far=%.9g left=%.9g right=%.9g down=%.9g up=%.9g",
                        id,prepared,trackedFrustum.nearDistance,trackedFrustum.farDistance,
                        trackedFrustum.slopes[0],trackedFrustum.slopes[1],trackedFrustum.slopes[2],trackedFrustum.slopes[3]);
                    if(!prepared) {
                        // One refused native-builder output, retained before any game write.
                        std::ofstream volumeFile(output/L"tracked-volume-refused.bin",std::ios::binary);
                        volumeFile.write(reinterpret_cast<const char*>(expanded.data()),expanded.size());
                        std::ofstream matrixFile(output/L"tracked-matrix-refused.bin",std::ios::binary);
                        matrixFile.write(reinterpret_cast<const char*>(visibilityMatrix.data()),sizeof(visibilityMatrix));
                        std::ofstream eyesFile(output/L"tracked-eye-points-refused.bin",std::ios::binary);
                        eyesFile.write(reinterpret_cast<const char*>(trackedFrustum.eyePoints.data()),sizeof(trackedFrustum.eyePoints));
                    }
                }
            }
            else if (prepared && asymmetricEyeFov && !nativeVisibilityControl) {
                const auto frusta=CurrentFrusta();
                prepared=ReadMemory(reinterpret_cast<void*>(mainObject+0x680),renderView.data(),renderView.size()) &&
                    MakeEyeVisibilityBox(original,sourceView,renderView,frusta,box);
            }
            if (prepared && nativeVisibilityControl) expanded=original;
            else if (prepared && !trackedVisibility) {
                visibilityMatrix=box.matrix;
                reinterpret_cast<FrustumBuildFn>(gameBase+0x87d860)(expanded.data(),visibilityMatrix.data());
                prepared=ValidateVisibilityBox(expanded,box);
            }
            if (prepared) {
                pendingVisibility={id,frame.load(),GetCurrentThreadId(),true,original,expanded};
                memcpy(reinterpret_cast<void*>(mainObject+0x330),expanded.data(),224);
                VisibilityVolume actual{};
                applied=ReadMemory(reinterpret_cast<void*>(mainObject+0x330),actual.data(),224) && actual==expanded;
                DumpStage(id,"visibility-original","volume",reinterpret_cast<uintptr_t>(original.data()),224);
                DumpStage(id,"visibility-expanded","volume",mainObject+0x330,224);
                if(!nativeVisibilityControl)
                    DumpStage(id,"visibility-expanded","matrix",reinterpret_cast<uintptr_t>(visibilityMatrix.data()),64);
            }
            if (!applied) {
                // A geometric refusal before the write leaves the native volume
                // untouched: cancel this scene and wait for readiness instead.
                if (!prepared && runtimeSession && trackedVisibility && TransientVisibilityRefusal(failure)) {
                    visibilityRefusedFrame=frame.load();visibilityRefusedReason=failure;
                } else visibilityHealthy=false;
                opponentlod::host::RevokeWorld();
                if(trackedVisibility && !visibilityFailureSaved) {
                    // One tiny failure record, not a rolling image capture. The
                    // reason says which inputs were actually read successfully.
                    visibilityFailureSaved=true;
                    const auto save=[&](const wchar_t* name,const void* data,size_t bytes) {
                        std::ofstream file(output/name,std::ios::binary);
                        file.write(static_cast<const char*>(data),bytes);return bool(file);
                    };
                    bool saved=save(L"visibility-failure-volume.bin",original.data(),original.size());
                    saved=save(L"visibility-failure-source.bin",sourceView.data(),sourceView.size()) && saved;
                    saved=save(L"visibility-failure-render.bin",renderView.data(),renderView.size()) && saved;
                    if(views)saved=save(L"visibility-failure-eyes.bin",views,sizeof(*views)) && saved;
                    std::ofstream info(output/L"visibility-failure.txt",std::ios::binary);
                    info << "frame=" << frame.load() << " scene=" << id << " reason=" << failure
                         << " eyes=" << bool(views) << " saved=" << saved << '\n';
                    Log("visibility preparation_failed id=%llu reason=%s inputs_saved=%u",id,failure,saved && bool(info));
                }
            }
        }
        Log("visibility before_jobs id=%llu valid=%u applied=%u radius=%.9g caller_rva=0x%zx eye_bounds=%u native_control=%u tracked=%u",id,valid,applied,box.radius,caller,asymmetricEyeFov,nativeVisibilityControl,trackedVisibility);
        signvisibility::Snapshot(frame.load(),id,0,"visibility-applied",mainObject,0);
    }
    return true;
}
void __fastcall VisibilityConsumer(void* self,void*,uintptr_t a0,uintptr_t context,uintptr_t view,uintptr_t a3,uintptr_t a4) {
    const auto caller=Rva(_ReturnAddress());
    if(frontendSceneAudit) {
        ++frontendVisibilityCalls;
        if(caller==0x1176b4 && reinterpret_cast<uintptr_t>(self)==visibilityConsumer)++frontendWorldVisibilityCalls;
        std::lock_guard lock(frontendAuditMutex);
        const auto end=frontendVisibilityCallers.begin()+frontendVisibilityCallerCount;
        if(frontendVisibilityCallerCount<frontendVisibilityCallers.size() &&
            std::find(frontendVisibilityCallers.begin(),end,caller)==end) {
            std::ofstream out(output/L"frontend-visibility-callers.jsonl",frontendVisibilityCallerCount?std::ios::app:std::ios::trunc);
            frontendVisibilityCallers[frontendVisibilityCallerCount++]=caller;
            out << "{\"frame\":" << frame.load() << ",\"callerRva\":" << caller << ",\"self\":" << reinterpret_cast<uintptr_t>(self)
                << ",\"thread\":" << GetCurrentThreadId() << ",\"renderThread\":" << renderThread
                << ",\"context\":" << context << ",\"view\":" << view << ",\"arg0\":" << a0
                << ",\"arg3\":" << a3 << ",\"arg4\":" << a4 << ",\"worldActive\":" << unsigned(Read<uint8_t>(mainObject+0x138))
                << ",\"visibilityEnabled\":" << unsigned(Read<uint8_t>(mainObject+0x51)) << "}\n";
        }
    }
    bool prepared=false,qualified=false;
    if (caller==0x1176b4 && reinterpret_cast<uintptr_t>(self)==visibilityConsumer) {
        const bool valid=GetCurrentThreadId()==renderThread && Read<uintptr_t>(mainObject+0x30)==visibilityConsumer && Read<void**>(visibilityConsumer)==visibilityShadow &&
            Read<void**>(mainObject)==mainShadow && context==Read<uintptr_t>(mainObject+0x5e4) && view==mainObject+0x5f0 &&
            a0==Read<uintptr_t>(mainObject+0xa8) && Read<uint8_t>(mainObject+0x138)==1 && Read<uint8_t>(mainObject+0x51)==1 &&
            VisibilityInstructionsMatch();
        qualified=valid && !inReplay;
        if(!valid) {opponentVisibilityFrame=0;opponentlod::host::RevokeWorld();}
        prepared=PrepareVisibility(valid,view,caller);
    }
    realVisibilityConsumer(self,a0,context,view,a3,a4);
    // Unlike the startup marker, this witnesses the guarded ORIGINAL consumer
    // on every native-center frame, including continuous stereo. Never stamp eyes.
    if(qualified && GetCurrentThreadId()==renderThread) opponentVisibilityFrame=frame.load();
    if (prepared && GetCurrentThreadId()==renderThread)
        signvisibility::Snapshot(frame.load(),0,0,"visibility-return",mainObject,0);
}
uintptr_t __fastcall VisibilityJobReset(void* self,void*) {
    const auto caller=Rva(_ReturnAddress());
    std::array<uintptr_t,7> stack{};
    const bool stackRead=ReadMemory(_AddressOfReturnAddress(),stack.data(),sizeof(stack));
    // Reset always runs exactly once, even for a caller we do not recognize.
    // Native dispatch has not submitted this job yet (including inline jobs).
    const auto result=realVisibilityReset(self);
    if (!inReplay && caller==0x78d778 && reinterpret_cast<uintptr_t>(self)==mainObject+visibilityEventOffset &&
        Read<uint8_t>(mainObject+0x9d)==1) {
        const bool valid=stackRead && VisibilityDispatchStack(stack,gameBase,mainObject) &&
            GetCurrentThreadId()==renderThread && Read<void**>(reinterpret_cast<uintptr_t>(self))==visibilityEventShadow &&
            Read<void**>(mainObject)==mainShadow && Read<void**>(rendererObject)==rendererShadow &&
            Read<uintptr_t>(mainObject+0x28)==rendererObject &&
            Read<uintptr_t>(mainObject+0x160)==gameBase+0xd0dfdc &&
            Read<uint8_t>(mainObject+0x138)==1 && Read<uint8_t>(mainObject+0x51)==1 &&
            visibilityConsumerFrame.load()!=frame.load() && !pendingVisibility.active && !frozenViews.Pending() &&
            VisibilityDispatchCode(gameBase,ReadLazyMemory) && VisibilityInstructionsMatch();
        if (!valid) {
            visibilityConsumerFrame=frame.load();visibilityHealthy=false;sessionFailureRequested=true;
            opponentVisibilityFrame=0;opponentlod::host::RevokeWorld();
            Log("alternate visibility refused frame=%llu caller_rva=0x%zx",frame.load(),caller);
            return result;
        }
        ++alternateVisibilityCalls;
        // Share the original volume/pose preparation and restoration contract.
        // Do not call the private consumer which this native branch skips.
        PrepareVisibility(valid,mainObject+0x5f0,caller);
        // This guarded native alternate dispatch skips VisibilityConsumer. Its
        // completed pair must still match this frame before LOD can qualify.
        opponentVisibilityFrame=frame.load();
        static unsigned receipts{};
        if (receipts<8) {
            std::ofstream out(output/L"alternate-visibility.jsonl",receipts++?std::ios::app:std::ios::trunc);
            out << "{\"frame\":" << frame.load() << ",\"valid\":" << (valid?"true":"false")
                << ",\"readyFrame\":" << visibilityReadyFrame.load()
                << ",\"expanded\":" << (pendingVisibility.active?"true":"false")
                << ",\"viewsFrozen\":" << (frozenViews.Pending()?"true":"false")
                << ",\"beforeJobSubmission\":true,\"nativeCodeUnchanged\":true}\n";
        }
    }
    return result;
}
bool InstallVisibilityDispatch() {
    if (!VisibilityDispatchCode(gameBase,ReadLazyMemory) ||
        Read<uintptr_t>(mainObject+0x160)!=gameBase+0xd0dfdc ||
        Read<uintptr_t>(mainObject+visibilityEventOffset)!=gameBase+visibilityEventTableRva) return false;
    realVisibilityReset=reinterpret_cast<VisibilityResetFn>(gameBase+visibilityEventMethods[6]);
    const VtablePatch patch{6,reinterpret_cast<void*>(realVisibilityReset),reinterpret_cast<void*>(VisibilityJobReset)};
    visibilityEventShadow=AttachShadowVtable(reinterpret_cast<void*>(mainObject+visibilityEventOffset),
        reinterpret_cast<void**>(gameBase+visibilityEventTableRva),visibilityEventMethods.size(),{&patch,1});
    Log("visibility dispatch installed=%u mode=object_shadow native_code_unchanged=1",visibilityEventShadow!=nullptr);
    return visibilityEventShadow!=nullptr;
}
bool InstallVisibility() {
    if (!traceVisibility || !VisibilityInstructionsMatch()) return false;
    visibilityConsumer=Read<uintptr_t>(mainObject+0x30);
    if (!visibilityConsumer || Read<uintptr_t>(visibilityConsumer)!=gameBase+0xcd9bb8 ||
        !ReadMemory(reinterpret_cast<void*>(gameBase+0xcd9bb8),originalVisibilityTable.data(),sizeof(originalVisibilityTable)) ||
        originalVisibilityTable[9]!=gameBase+0x28d2f0) return false;
    realVisibilityConsumer=reinterpret_cast<VisibilityConsumerFn>(gameBase+0x28d2f0);
    const VtablePatch patch{9,reinterpret_cast<void*>(realVisibilityConsumer),reinterpret_cast<void*>(VisibilityConsumer)};
    visibilityShadow=AttachShadowVtable(reinterpret_cast<void*>(visibilityConsumer),reinterpret_cast<void**>(gameBase+0xcd9bb8),30,{&patch,1});
    Log("visibility hook installed=%u mode=object_shadow consumer=%p table_rva=0xcd9bb8 expansion=%u",
        visibilityShadow!=nullptr,reinterpret_cast<void*>(visibilityConsumer),expandVisibility);
    return visibilityShadow!=nullptr &&
        (!(runtimeSession && std::filesystem::exists(output/L"normal-launch.request")) || InstallVisibilityDispatch());
}
bool ReplayInstructionsMatch() {
    constexpr uint8_t sceneReturn[]{0x5f,0x5e,0x5b,0x5d,0xc2,0x18,0};
    constexpr uint8_t historyCounter[]{0x8b,0x4b,0x0c,0x80,0xb9,0x8c,0,0,0,0,0x74,0x06,0xff,0x87,0x94,0,0,0};
    constexpr uint8_t historySetter[]{0x55,0x8b,0xec,0x8b,0x45,0x08,0x89,0x81,0xa0,0,0,0,0x5d,0xc2,0x04,0};
    constexpr uint8_t historyStore[]{0x8b,0x87,0xa0,0,0,0,0xc5,0xf8,0x28,0x86,0x90,0x01,0,0,0xc1,0xe0,0x06,0x03,0xf8,0xc5,0xf8,0x29,0x47,0x10};
    constexpr uint8_t historyResetBranch[]{0x80,0xbf,0x90,0,0,0,0,0xc5,0xfa,0x11,0x97,0x2c,0x01,0,0,0x0f,0x84,0xcd,0x01,0,0};
    constexpr uint8_t historyResetSeed[]{
        0xc5,0xf8,0x28,0x86,0x90,0x01,0,0,0xc5,0xf8,0x29,0x40,0x10,
        0xc5,0xf8,0x28,0x86,0xa0,0x01,0,0,0xc5,0xf8,0x29,0x40,0x20,
        0xc5,0xf8,0x28,0x86,0xb0,0x01,0,0,0xc5,0xf8,0x29,0x40,0x30,
        0xc5,0xf8,0x28,0x86,0xc0,0x01,0,0,0xc5,0xf8,0x29,0x40,0x40,
        0xc6,0x87,0x90,0,0,0,1};
    constexpr uint8_t renderCounter[]{0xff,0x86,0xc8,0,0,0};
    return MatchesInstructions(reinterpret_cast<void*>(gameBase+0x3be1c5),sceneReturn) &&
        MatchesInstructions(reinterpret_cast<void*>(gameBase+0x2e6a09),historyCounter) &&
        MatchesInstructions(reinterpret_cast<void*>(gameBase+0x5a52e0),historySetter) &&
        MatchesInstructions(reinterpret_cast<void*>(gameBase+0x2e7203),historyStore) &&
        MatchesInstructions(reinterpret_cast<void*>(gameBase+0x2e6abb),historyResetBranch) &&
        MatchesInstructions(reinterpret_cast<void*>(gameBase+0x2e6ca1),historyResetSeed) &&
        MatchesInstructions(reinterpret_cast<void*>(gameBase+0x32c1a4),renderCounter) &&
        LazyInstructionsMatch() &&
        Read<uintptr_t>(gameBase+0xcf8108+15*4)==gameBase+0x2e69c0 &&
        Read<uintptr_t>(gameBase+0xcf8108+23*4)==gameBase+0x5a52e0;
}
bool ClipInstructionsMatch(uintptr_t context) {
    constexpr uint8_t clipStores[]{0xd9,0x47,0x40,0x8b,0x42,0x40,0xd9,0x9e,0xe8,0,0,0,
        0x8b,0xce,0xff,0xd0,0x8b,0x16,0xc5,0xfa,0x10,0x43,0x10,0x8b,0x42,0x40,
        0x8b,0xce,0xc5,0xfa,0x11,0x86,0xec,0,0,0,0xff,0xd0};
    uint8_t subpass[]{0xc5,0xfa,0x10,0x05,0,0,0,0,0x8b,0x53,0x08,0x6a,0x03,0x51,0x8b,0x4b,0x0c,
        0xc5,0xfa,0x11,0x46,0x40,0xc5,0xfa,0x10,0x05,0,0,0,0,0xc5,0xfa,0x11,0x04,0x24,
        0x51,0x52,0x8b,0xce,0xe8,0xaa,0xfe,0x1c,0};
    const auto nearConstant=gameBase+0xbf4e80,farConstant=gameBase+0xc7686c;
    memcpy(subpass+4,&nearConstant,4); memcpy(subpass+26,&farConstant,4);
    return Read<uintptr_t>(context)==gameBase+0xd4b458 &&
        Read<uintptr_t>(gameBase+0xd4b458+16*4)==gameBase+0x93f070 &&
        originalMainTable[14]==gameBase+0x8afc0 &&
        Read<uint32_t>(nearConstant)==0x3e4ccccdu && Read<uint32_t>(farConstant)==0x42c80000u &&
        MatchesInstructions(reinterpret_cast<void*>(gameBase+0x8c02a),subpass) &&
        MatchesInstructions(reinterpret_cast<void*>(gameBase+0x25c6d2),clipStores);
}
void ApplyReplayProjection(void* self,uintptr_t context,uintptr_t caller) {
    if (!inReplay || !asymmetricEyeFov || !pendingScene.valid || context!=pendingScene.call.args[3]) return;
    constexpr uint8_t rebind[]{0x8b,0x4e,0x28,0x8b,0x11,0x8b,0x7b,0x08,0x8b,0x82,0x44,0x01,0,0,0x57,0xff,0xd0};
    // The alternate scene pass uploads the same context after both builders.
    // Guard the entire entry-to-upload path and verify; never rewrite it here.
    constexpr uint8_t alternate[]{0x55,0x8b,0xec,0x8b,0x55,0x08,0x83,0xec,0x08,0x53,0x56,0x8b,0xf1,0x8b,0x4e,0x28,
        0x8b,0x01,0x8b,0x80,0x44,0x01,0,0,0x57,0x52,0xff,0xd0};
    const bool build=caller==0x25cccd;
    const bool rebindCall=(caller==0x32c2ec && MatchesInstructions(reinterpret_cast<void*>(gameBase+0x32c2db),rebind)) ||
        (caller==0x4e469c && MatchesInstructions(reinterpret_cast<void*>(gameBase+0x4e4680),alternate));
    CameraContext candidate{},incoming{};
    bool valid=(build || rebindCall) && reinterpret_cast<uintptr_t>(self)==rendererObject &&
        Read<void**>(rendererObject)==rendererShadow && pendingScene.frame==frame.load() &&
        WritableHeap(context,512) && ClipInstructionsMatch(context) &&
        ReadMemory(reinterpret_cast<void*>(context),incoming.data(),incoming.size());
    candidate=incoming;
    valid=valid && ApplyEyeFrustum(candidate,CurrentFrustum());
    if (rebindCall) valid=valid && eyeProjectionUploads>0 && candidate==incoming;
    eyeProjectionHealthy&=valid;
    if (valid && build) {
        // Only lateral projection terms and their derived VP. Preserve native
        // clip terms and let the existing guarded uploader bind the constants.
        for (size_t offset:{0x110u,0x124u,0x130u,0x134u})
            memcpy(reinterpret_cast<void*>(context+offset),candidate.data()+offset,4);
        memcpy(reinterpret_cast<void*>(context+0x190),candidate.data()+0x190,64);
        ++eyeProjectionUploads;
    }
    Log("eye projection id=%llu applied=%u upload=%u caller_rva=0x%zx operation=%s",pendingScene.id,valid,eyeProjectionUploads,caller,build?"write":rebindCall?"verify":"unknown");
}
bool RenderReplay(const PendingScene& scene,bool nativeStereo,bool firstImageSaved,bool& gpuRestoredOutput) {
    profile::Scope measure(profile::Replay);
    gpuRestoredOutput=false;
    replayedEmptyBuilder=false;
    // Every refusal below precedes ApplyReplayHistorySeed, the first game write.
    auto refuse=[&](const char* reason) {
        if (runtimeSession && TransientReplayRefusal(reason,replayPass)) transientRefusal=reason;
        Log("replay refused id=%llu reason=%s",scene.id,reason); return false;
    };
    auto& renderedEyeHistory=renderedEyeHistories[dualEyes && replayPass==2?1:0];
    const auto& pose=CurrentPose();
    if (dynamicViews && !frozenViews.Get(scene.frame)) return refuse("views_not_frozen");
    if (expandVisibility && (!visibilityHealthy || !pendingVisibility.active || pendingVisibility.frame!=frame.load())) {
        return refuse("visibility_not_ready");
    }
    if (inReplay) return refuse("nested_replay");
    if (nativeStereo) return refuse("native_stereo");
    if (!scene.stage6Seen) return refuse("stage6_missing");
    if (!scene.replayInputsSaved) return refuse("replay_inputs_missing");
    if (!firstImageSaved) return refuse("center_capture_or_gpu_guard");
    if (scene.frame!=frame.load()) return refuse("scene_frame_changed");
    if (Read<void**>(mainObject)!=mainShadow || Read<void**>(rendererObject)!=rendererShadow ||
        Read<uintptr_t>(mainObject+0x28)!=rendererObject || Read<uintptr_t>(mainObject+0x2c)!=scene.manager ||
        !scene.renderData || Read<uintptr_t>(mainObject+0x710)!=scene.renderData ||
        Read<uintptr_t>(scene.manager)!=gameBase+0xcf8108 || Read<uint8_t>(rendererObject+0x512e)!=0 ||
        Read<uint8_t>(rendererObject+0x512c)!=1 || Read<uint32_t>(rendererObject+0x5118)!=0 ||
        Read<uintptr_t>(rendererObject+0x5130)!=0 || !ReplayInstructionsMatch() ||
        !WritableHeap(scene.manager,0x200) || !WritableHeap(mainObject+0xc8,4) ||
        !WritableHeap(scene.call.args[3],0x200) || !WritableHeap(mainObject+0x40,4)) {
        return refuse("identity_instructions_or_memory");
    }
    HistoryState saved{},prepared{};
    alignas(16) SceneView view{},originalView{};
    CameraContext context{};
    ScenePacket packet{};
    const auto counter=Read<uint32_t>(mainObject+0xc8);
    const auto nearPlane=Read<uint32_t>(mainObject+0x40);
    if (!ReadMemory(reinterpret_cast<void*>(scene.manager),saved.data(),saved.size()) ||
        !ReadMemory(reinterpret_cast<void*>(scene.call.args[2]),originalView.data(),originalView.size()) ||
        !ReadMemory(reinterpret_cast<void*>(scene.call.args[3]),context.data(),context.size()) ||
        !ReadMemory(reinterpret_cast<void*>(scene.call.args[0]),packet.data(),packet.size())) {
        return refuse("replay_input_read");
    }
    const auto farPlane=Read<uint32_t>(mainObject+0x44);
    auto refuseInputs=[&](const char* reason) {
        // Retain one exact failure input, about 6 KiB, instead of enabling
        // per-frame captures. All arrays above are successful native reads.
        static bool recordedRejection=false;
        if (!recordedRejection) {
            recordedRejection=true;
            std::ofstream receipt(output/L"replay-input-rejection.json");
            receipt << "{\"frame\":" << scene.frame << ",\"eye\":" << replayPass
                << ",\"reason\":\"" << reason << "\",\"headsetProjection\":" << (headsetProjection?"true":"false")
                << ",\"entryCounter\":" << scene.counter << ",\"completeCounter\":" << counter
                << ",\"entryNear\":" << scene.nearPlane << ",\"completeNear\":" << nearPlane
                << ",\"entryFar\":" << scene.farPlane << ",\"completeFar\":" << farPlane;
            auto bytes=[&](const char* name,const auto& data) {
                constexpr char hex[]{"0123456789abcdef"};
                receipt << ",\"" << name << "\":\"";
                for (auto value:data) receipt << hex[value>>4] << hex[value&15];
                receipt << '"';
            };
            bytes("entryCamera",scene.context); bytes("completeCamera",context);
            bytes("entryHistory",scene.history); bytes("completeHistory",saved);
            bytes("entryView",scene.view); bytes("completeView",originalView);
            bytes("packet",packet);
            LazyListGate gate{};
            const auto gateError=ReadLazyListGate(gameBase,mainObject,ReadLazyMemory,gate);
            receipt << ",\"lazyGateReadable\":" << (gateError?"false":"true")
                << ",\"lazyMode\":" << gate.mode << ",\"lazyBuilder\":" << gate.builder
                << ",\"lazyEnabled\":" << unsigned(gate.enabled)
                << ",\"lazyWitness\":\"completed_scene\""
                << ",\"lazyCenterHealthy\":" << (emptyListWitness.healthy?"true":"false")
                << ",\"lazyCenterRejectionMask\":" << emptyListWitness.rejectionMask
                << ",\"lazyAuxiliaryPending\":" << unsigned(emptyListWitness.state.auxiliaryPending)
                << ",\"lazyCenterBuilder\":" << emptyListWitness.identity.builder;
            bytes("lazyCenterState",emptyListWitness.state.object);
            const auto& roots=emptyListWitness.state.roots;
            receipt << ",\"lazyCenterRoots\":[" << roots[0] << ',' << roots[1] << ',' << roots[2] << ']';
            receipt << "}\n";
        }
        return refuse(reason);
    };
    if (!ReplayCameraSupported(scene.context,context,scene.nearPlane,nearPlane,scene.farPlane,!headsetProjection))
        return refuseInputs("replay_camera_transition");
    if (farPlane!=scene.farPlane) return refuseInputs("replay_far_plane_changed");
    if (context!=scene.context && !ClipInstructionsMatch(scene.call.args[3])) return refuseInputs("replay_clip_instructions");
    if (originalView!=scene.view) return refuseInputs("replay_view_changed");
    if (counter!=scene.counter+1u) return refuseInputs("replay_counter_changed");
    if (memcmp(saved.data()+0x10,scene.context.data()+0x190,0x40)) return refuseInputs("replay_history_camera_mismatch");
    if (!PrepareReplayHistory(scene.history,saved,originalView,prepared,view)) return refuseInputs("replay_history_transition");
    const bool emptyPacket=!PacketHasLazyLists(packet);
    LazyListGate lazyGate{};LazyListState lazyState{};
    if (emptyPacket) {
        if (!lazyListsVerified) return refuseInputs("empty_packet_instructions_unverified");
        const auto reason=ReadLazyListGate(gameBase,mainObject,ReadLazyMemory,lazyGate);
        if (reason) return refuseInputs(reason);
        if(emptyListWitness.rejectionMask==64) return refuseInputs("empty_packet_auxiliary_not_pending");
        if ((lazyGate.Open() && !ReadLazyListState(lazyGate.builder,ReadLazyMemory,lazyState)) ||
            !emptyListWitness.Matches(LazyIdentity(scene.call.args[0],lazyGate.builder),packet,lazyGate,lazyState))
            return refuseInputs("empty_packet_center_witness");
    }
    constexpr uint8_t cameraBuilder[]{0x53,0x8b,0xdc,0x83,0xec,0x08,0x83,0xe4,0xf0,0x83,0xc4,0x04,
        0x55,0x8b,0x6b,0x04,0x89,0x6c,0x24,0x04,0x8b,0xec,0x81,0xec,0x78,0x01,0,0};
    const auto buildCamera=reinterpret_cast<NativeCameraFn>(gameBase+0x25bf00);
    // The near-object pass can leave a different far plane than main+0x44.
    float completedFarPlane{}; memcpy(&completedFarPlane,context.data()+0xec,4);
    if (eyeOffsetEnabled) {
        if (!ClipInstructionsMatch(scene.call.args[3])) return refuse("eye_clip_instructions");
        if (!MatchesInstructions(reinterpret_cast<void*>(gameBase+0x25bf00),cameraBuilder)) return refuse("eye_builder_instructions");
        if (!(fullEyePose ? ApplyScenePose(scene.context,view,pose,view) :
                OffsetSceneView(scene.context,view,diagnosticEyeOffset,view,eyeYaw))) {
            // One small exact CPU input receipt for offline reproduction. No
            // per-frame dumps or GPU readback, including in sustained sessions.
            static bool recordedRejection=false;
            if (!recordedRejection) {
                recordedRejection=true;
                std::ofstream receipt(output/L"camera-rejection.json");
                receipt << "{\"frame\":" << scene.frame << ",\"eye\":" << replayPass;
                auto bytes=[&](const char* name,const void* data,size_t size) {
                    constexpr char hex[]{"0123456789abcdef"};
                    receipt << ",\"" << name << "\":\"";
                    const auto* p=static_cast<const uint8_t*>(data);
                    for (size_t i=0;i<size;++i) receipt << hex[p[i]>>4] << hex[p[i]&15];
                    receipt << '"';
                };
                bytes("camera",scene.context.data(),scene.context.size());
                bytes("view",view.data(),view.size());
                bytes("orientation",pose.orientation.data(),sizeof(pose.orientation));
                bytes("position",pose.position.data(),sizeof(pose.position));
                receipt << "}\n";
            }
            return refuse("eye_camera_pose");
        }
    }
    const bool historyReset=scene.history[0x90]==0;
    // A native reset ignores the old matrix and builds history from this eye's
    // current camera. Do not require or inject a pre-reset eye camera there.
    if (eyeOffsetEnabled && !historyReset && (!previousEyeCamera.valid || previousEyeCamera.frame+1!=scene.frame ||
        previousEyeCamera.manager!=scene.manager || previousEyeCamera.context!=scene.call.args[3] ||
        memcmp(previousEyeCamera.camera.data()+0x190,scene.history.data()+0x10,64) ||
        !(ownedEyeHistory && replaySequence.EpochCompleted()>0
            ? renderedEyeHistory.Seed(scene.frame,scene.call.args[3],scene.manager,prepared)
            : fullEyePose
                ? PreparePoseHistory(previousEyeCamera.camera,previousEyeCamera.view,scene.history,pose,prepared,asymmetricEyeFov?&CurrentFrustum():nullptr)
                : PrepareEyeHistory(previousEyeCamera.camera,previousEyeCamera.view,scene.history,diagnosticEyeOffset,prepared,eyeYaw,asymmetricEyeFov?&diagnosticFrustum:nullptr)))) {
        return refuse("eye_previous_camera_or_history");
    }
    auto replay=scene;
    replay.call.args[2]=reinterpret_cast<uintptr_t>(view.data());
    Log("replay clip id=%llu clip_changed=%u near_entry=%08x near_complete=%08x far=%08x",
        scene.id,context!=scene.context,scene.nearPlane,nearPlane,scene.farPlane);
    Log("replay begin id=%llu manager=%p packet=%p context=%p original_view=%p replay_view=%p counter=%u history_counter=%u",
        scene.id,reinterpret_cast<void*>(scene.manager),reinterpret_cast<void*>(scene.call.args[0]),
        reinterpret_cast<void*>(scene.call.args[3]),reinterpret_cast<void*>(scene.call.args[2]),view.data(),counter,HistoryWord(saved,0x94));
    if (eyeOffsetEnabled) {
        if (fullEyePose) Log("eye fullpose id=%llu qx=%.9g qy=%.9g qz=%.9g qw=%.9g x=%.9g y=%.9g z=%.9g camera_builder=0x25bf00",
            scene.id,pose.orientation[0],pose.orientation[1],pose.orientation[2],pose.orientation[3],
            pose.position[0],pose.position[1],pose.position[2]);
        else Log("eye offset id=%llu world_units=%.9g camera_builder=0x25bf00",scene.id,diagnosticEyeOffset);
        if (eyeYaw!=0) Log("eye pose id=%llu yaw_radians=%.9g",scene.id,eyeYaw);
        const bool previousSaved=DumpStage(scene.id,"eye-previous","context",reinterpret_cast<uintptr_t>(previousEyeCamera.camera.data()),previousEyeCamera.camera.size()) &&
            DumpStage(scene.id,"eye-previous","view",reinterpret_cast<uintptr_t>(previousEyeCamera.view.data()),previousEyeCamera.view.size());
        const bool ownedPrevious=ownedEyeHistory && replaySequence.EpochCompleted()>0;
        const bool renderedSaved=!ownedPrevious || DumpStage(scene.id,"eye-previous-rendered","context",reinterpret_cast<uintptr_t>(renderedEyeHistory.camera.data()),renderedEyeHistory.camera.size());
        Log("eye history id=%llu previous_frame=%llu saved=%u mode=%s",scene.id,previousEyeCamera.frame,previousSaved && renderedSaved,
            historyReset?"native_reset":ownedPrevious?"owned_previous":fullEyePose?"posed_previous":eyeYaw!=0?"rotated_previous":"translated_previous");
    }
    // Reuse the completed packet and the live manager. Never clone its embedded nodes.
    auto* manager=reinterpret_cast<uint8_t*>(scene.manager);
    ApplyReplayHistorySeed(manager,prepared);
    memcpy(reinterpret_cast<void*>(mainObject+0xc8),&scene.counter,4);
    // Native complete-scene rebuilds the camera from main+0x40. Restore the
    // incoming clip state, not the near-object subpass left by the first view.
    memcpy(reinterpret_cast<void*>(mainObject+0x40),&scene.nearPlane,4);
    RestoreClipWords(reinterpret_cast<void*>(scene.call.args[3]),scene.context);
    SceneSnapshot(replay,"replay-entry");
    eyeProjectionHealthy=true; eyeProjectionUploads=0; replayMainCameraValid=false; replayMainSamples=0;
    const bool invoked=InvokeReplayScene(replay.call,emptyPacket,packet,lazyGate,lazyState);
    LazyListGate lazyAfterGate{};LazyListState lazyAfter{};ScenePacket lazyAfterPacket{};
    const bool lazyRead=emptyPacket && !ReadLazyListGate(gameBase,mainObject,ReadLazyMemory,lazyAfterGate) &&
        ReadLazyMemory(scene.call.args[0],lazyAfterPacket.data(),lazyAfterPacket.size()) &&
        (!lazyAfterGate.Open() || ReadLazyListState(lazyAfterGate.builder,ReadLazyMemory,lazyAfter));
    const bool lazyRestored=!emptyPacket || emptyListReplay.Finish(LazyIdentity(scene.call.args[0],lazyAfterGate.builder),
        lazyRead,lazyAfterPacket,lazyAfterGate,lazyAfter);
    if (emptyPacket) Log("replay empty_packet id=%llu builder_enabled=%u witness=completed_scene invoked=%u replay_checked=%u",
        scene.id,lazyGate.Open(),invoked,lazyRestored);
    const bool secondImageSaved=SceneSnapshot(replay,"replay-complete");
    HistoryState after{};
    std::array<uint8_t,0x128> afterPacket{};
    std::array<uint8_t,0x200> afterContext{};
    const bool historyRead=ReadMemory(manager,after.data(),after.size());
    const bool packetSame=ReadMemory(reinterpret_cast<void*>(scene.call.args[0]),afterPacket.data(),afterPacket.size()) && packet==afterPacket;
    const bool contextRead=ReadMemory(reinterpret_cast<void*>(scene.call.args[3]),afterContext.data(),afterContext.size());
    const bool contextSame=contextRead && context==afterContext;
    bool cameraRestored=contextSame;
    const auto afterCounter=Read<uint32_t>(mainObject+0xc8);
    const bool nearSame=Read<uint32_t>(mainObject+0x40)==nearPlane;
    const bool mainHistory=historyRead && replayMainCameraValid &&
        CameraClipEqual(replayMainCamera,scene.context) &&
        !memcmp(after.data()+0x50,replayMainCamera.data()+0x190,64);
    const bool clipSequence=contextRead && replayMainCameraValid &&
        ReplayCameraSupported(replayMainCamera,afterContext,scene.nearPlane,Read<uint32_t>(mainObject+0x40),scene.farPlane,!headsetProjection) &&
        CameraClipEqual(afterContext,context);
    Log("eye clip id=%llu main_history=%u sequence_same=%u main_far=%08x final_far=%08x",
        scene.id,mainHistory,clipSequence,CameraWord(replayMainCamera,0xec),CameraWord(afterContext,0xec));
    RestoreHistoryWrites(manager,saved);
    memcpy(reinterpret_cast<void*>(mainObject+0xc8),&counter,4);
    memcpy(reinterpret_cast<void*>(mainObject+0x40),&nearPlane,4);
    if (eyeOffsetEnabled) {
        // Rebuild every native derived camera field from the original view;
        // never restore an opaque object by copying a guessed byte window.
        BuildNativeCamera(buildCamera,reinterpret_cast<void*>(mainObject),scene.call.args[3],originalView,completedFarPlane);
        CameraContext restoredCamera{};
        cameraRestored=ReadMemory(reinterpret_cast<void*>(scene.call.args[3]),restoredCamera.data(),restoredCamera.size()) && restoredCamera==context;
        Log("eye restored id=%llu camera_same=%u",scene.id,cameraRestored);
    }
    RestoreClipWords(reinterpret_cast<void*>(scene.call.args[3]),context);
    HistoryState restored{};
    const bool restoredSame=ReadMemory(manager,restored.data(),restored.size()) && restored==saved;
    const bool gpuRestored=SceneSnapshot(scene,"replay-restored");
    gpuRestoredOutput=gpuRestored;
    Log("replay end id=%llu same_frame=%u image_saved=%u packet_same=%u context_same=%u history_other_same=%u history_restored=%u counter_before=%u counter_after=%u history_counter_after=%u history_slot1_current=%u near_same=%u",
        scene.id,frame.load()==scene.frame,secondImageSaved,packetSame,contextSame,
        historyRead && HistoryOtherFieldsEqual(saved,after),restoredSame,counter,afterCounter,
        historyRead?HistoryWord(after,0x94):0,historyRead && memcmp(after.data()+0x50,scene.context.data()+0x190,0x40)==0,nearSame);
    bool accepted=invoked && lazyRestored && secondImageSaved && frame.load()==scene.frame && packetSame && cameraRestored && nearSame &&
        historyRead && after[0x90]==1 && HistoryOtherFieldsEqual(saved,after) && restoredSame && afterCounter==counter &&
        HistoryWord(after,0x94)==HistoryWord(saved,0x94) && (replaySequence.Limit()==1 || gpuRestored);
    if (asymmetricEyeFov) accepted&=eyeProjectionHealthy && eyeProjectionUploads>0;
    if (eyeOffsetEnabled) accepted&=mainHistory && clipSequence;
    if (accepted && ownedEyeHistory) accepted=renderedEyeHistory.Store(scene.frame,scene.call.args[3],scene.manager,replayMainCamera);
    replayedEmptyBuilder=accepted && emptyPacket && lazyGate.Open();
    if (dualEyes) Log("stereo eye id=%llu eye=%u accepted=%u gpu_restored=%u",scene.id,replayPass-1,accepted,gpuRestored);
    return accepted;
}
// No C++ objects in this SEH boundary: suppress new LOD policy for the WHOLE
// frozen eye pair, including the gap between eyes, and clear it on native faults.
bool InvokeReplayPair(const PendingScene& scene,bool nativeStereo,bool firstImageSaved,
                      bool& allGpuRestored,bool& allEmptyBuilder) {
    bool accepted=true;
    __try {
        opponentlod::runtime::SetEyeReplay(true);
        for (unsigned pass=1;pass<=(dualEyes?2u:1u) && accepted;++pass) {
            replayPass=pass;
            bool restored{};
            accepted=RenderReplay(scene,nativeStereo,firstImageSaved,restored);
            allGpuRestored&=restored;
            allEmptyBuilder&=replayedEmptyBuilder;
        }
    } __finally {
        replayPass=0;
        opponentlod::runtime::SetEyeReplay(false);
    }
    return accepted;
}
bool ReplayOnce(const PendingScene& scene,bool nativeStereo,bool firstImageSaved) {
    if (!replayEnabled || !replaySequence.Available()) return false;
    if (!replaySequence.Begin(scene.frame)) { opponentlod::host::RevokeWorld();Log("replay refused id=%llu reason=nonconsecutive_frame",scene.id); return false; }
    bool allGpuRestored=true,allEmptyBuilder=true;
    transientRefusal=nullptr;
    const bool accepted=InvokeReplayPair(scene,nativeStereo,firstImageSaved,allGpuRestored,allEmptyBuilder);
    if (accepted || !transientRefusal || !replaySequence.Cancel()) { transientRefusal=nullptr;replaySequence.Finish(accepted); }
    if(!accepted || !allGpuRestored) opponentlod::host::RevokeWorld();
    if (accepted && allGpuRestored && Read<uint8_t>(mainObject+0x9d)==1) ++alternateVisibilityPairs;
    if (accepted && allGpuRestored && scene.history[0x90]==0) {
        static bool resetProofSaved=false;
        if (!resetProofSaved) {
            // One small durable witness; the regular per-eye trace can rotate.
            std::ofstream proof(output/L"history-reset-replay.json");
            proof << "{\"frame\":" << scene.frame << ",\"scene\":" << scene.id
                << ",\"eyes\":" << (dualEyes?2:1)
                << ",\"entryValid\":0,\"completeValid\":1,\"sameFrame\":true,\"historyRestored\":true,\"gpuRestored\":true}\n";
            proof.close();resetProofSaved=proof.good();
        }
    }
    if (accepted && allGpuRestored && allEmptyBuilder) {
        ++emptyBuilderPairs;lastEmptyBuilderFrame=scene.frame;
        if (emptyBuilderPairs==1) {
            // Small durable witness; bounded text logs may rotate this event out.
            std::ofstream proof(output/L"empty-list-replay.json");
            proof << "{\"frame\":" << scene.frame << ",\"scene\":" << scene.id
                << ",\"witness\":\"completed_scene\",\"nativeCodeUnchanged\":true,\"eyes\":" << (dualEyes?2:1)
                << ",\"builder\":" << emptyListWitness.identity.builder
                << ",\"packet\":" << emptyListWitness.identity.packet
                << ",\"thread\":" << emptyListWitness.identity.thread
                << ",\"auxiliaryPending\":" << unsigned(emptyListWitness.state.auxiliaryPending)
                << ",\"sourceRoots\":[0,0,0],\"sameFrame\":true,\"gpuRestored\":true}\n";
        }
    }
    if (replaySequence.Limit()>1) Log("sequence pair id=%llu index=%llu target=%u accepted=%u gpu_restored=%u",
        scene.id,replaySequence.Attempts(),replaySequence.Limit(),accepted,allGpuRestored);
    return accepted && allGpuRestored;
}
// The centre frame is complete and neither eye began. Cancel this pair like a
// menu transition; stereo resumes only through the existing readiness gate.
void CancelTransientScene(const char* reason,bool restored) {
    const auto sceneFrame=pendingScene.frame;
    const bool cancelled=restored && cancelHistory && cancelHistory() &&
        (!dynamicViews || frozenViews.CancelBeforeReplay(sceneFrame));
    if (observePair) observePair(sceneFrame,0,false);
    pendingScene.valid=false;transientRefusal=nullptr;
    const bool suspended=cancelled && SuspendSession();
    if (!suspended) FailSession();
    else { ++transientSuspensions;transientRefusalFrame=sceneFrame;lastTransientRefusal=reason; }
    Log("session transient frame=%llu reason=%s cancelled_before_replay=%u suspended=%u count=%llu",
        sceneFrame,reason,cancelled,suspended,transientSuspensions);
}
void AuditFrontendScene(const CameraContext& complete,bool stable,bool menu,RecordedReplayState playback) {
    if(!frontendSceneAudit)return;
    // Sparse, bounded CPU evidence only: no images or GPU readbacks.
    static unsigned count{};
    static uint64_t previousFrame{};
    static std::array<uint32_t,7> previousSignature{};
    const std::array<uint32_t,7> signature{Read<uint32_t>(mainObject+0x9e4),Read<uint8_t>(mainObject+0x138),
        CameraWord(complete,0xe8),CameraWord(complete,0xec),unsigned(playback),unsigned(menu),
        unsigned(garageMenu.state)};
    // At 120 Hz the ordinary interval now covers 75 seconds; transitions get
    // their own receipt without filling the budget with a moving intro camera.
    // Reserve the second half for transitions if the user takes longer to enter
    // the garage; a title idle period must not consume all useful evidence.
    if(count>=32 || (count && signature==previousSignature && (count>=16 || frame.load()<previousFrame+600)))return;
    previousFrame=frame.load();
    previousSignature=signature;
    VisibilityVolume volume{};SceneView visibilityView{},renderView{};VisibilityBox box{};
    const bool volumeReadable=ReadMemory(reinterpret_cast<void*>(mainObject+0x330),volume.data(),volume.size());
    const bool visibilityViewReadable=ReadMemory(reinterpret_cast<void*>(mainObject+0x5f0),visibilityView.data(),visibilityView.size());
    const bool renderViewReadable=ReadMemory(reinterpret_cast<void*>(mainObject+0x680),renderView.data(),renderView.size());
    const bool volumeValid=volumeReadable && visibilityViewReadable && MakeVisibilityBox(volume,visibilityView,box);
    std::ofstream out(output/L"frontend-scenes.jsonl",count?std::ios::app:std::ios::trunc);
    ++count;
    out << "{\"frame\":" << frame.load() << ",\"main\":" << mainObject << ",\"renderer\":" << rendererObject
        << ",\"cameraManager\":" << currentEyeCamera.manager << ",\"context\":" << currentEyeCamera.context
        << ",\"stable\":" << (stable?"true":"false") << ",\"shortMenu\":" << (menu?"true":"false")
        << ",\"recordedPlayback\":" << unsigned(playback) << ",\"mainNear\":" << Read<uint32_t>(mainObject+0x40)
        << ",\"mainFar\":" << Read<uint32_t>(mainObject+0x44) << ",\"mainType\":" << Read<uint32_t>(mainObject+0x14)
        << ",\"mainMode\":" << Read<uint32_t>(mainObject+0x9e4) << ",\"visibilityFrame\":" << visibilityReadyFrame
        << ",\"worldActive\":" << unsigned(Read<uint8_t>(mainObject+0x138))
        << ",\"visibilityEnabled\":" << unsigned(Read<uint8_t>(mainObject+0x51))
        << ",\"alternateScene\":" << unsigned(Read<uint8_t>(mainObject+0x9d))
        << ",\"prepareCalls\":" << frontendPrepareCalls.load() << ",\"visibilityCalls\":" << frontendVisibilityCalls.load()
        << ",\"worldVisibilityCalls\":" << frontendWorldVisibilityCalls.load()
        << ",\"garageState\":" << unsigned(garageMenu.state)
        << ",\"garageObject\":" << garageMenu.object << ",\"garageContext\":" << garageMenu.context
        << ",\"visibilityReadable\":" << (volumeReadable && visibilityViewReadable && renderViewReadable?"true":"false")
        << ",\"visibilityValid\":" << (volumeValid?"true":"false")
        << ",\"normalLaunch\":" << (std::filesystem::exists(output/L"normal-launch.request")?"true":"false");
    const auto bytes=[&](const char* name,const auto& values) {
        constexpr char hex[]{"0123456789abcdef"};out << ",\"" << name << "\":\"";
        for(auto value:values)out << hex[value>>4] << hex[value&15];out << '"';
    };
    bytes("entryCamera",currentEyeCamera.camera);bytes("completeCamera",complete);bytes("view",currentEyeCamera.view);
    if(volumeReadable)bytes("volume",volume);
    if(visibilityViewReadable)bytes("visibilityView",visibilityView);
    if(renderViewReadable)bytes("renderView",renderView);
    out << "}\n";
}
void AuditFrontendReadiness(uint64_t nextFrame) {
    if(!frontendSceneAudit)return;
    static bool written{},previousFresh{};
    const bool fresh=currentEyeCamera.valid && currentEyeCamera.frame+1==nextFrame;
    if(written && previousFresh==fresh && nextFrame%120)return;
    written=true;previousFresh=fresh;
    std::ofstream out(output/L"frontend-readiness.json",std::ios::trunc);
    out << "{\"frame\":" << nextFrame << ",\"cameraFrame\":" << currentEyeCamera.frame << ",\"cameraFresh\":" << (fresh?"true":"false")
        << ",\"cameraValid\":" << (currentEyeCamera.valid?"true":"false")
        << ",\"cameraManager\":" << currentEyeCamera.manager << ",\"cameraManagerTable\":" << Read<uintptr_t>(currentEyeCamera.manager)
        << ",\"cameraManagerMode\":" << Read<uint32_t>(currentEyeCamera.manager+0xa0)
        << ",\"cameraContext\":" << currentEyeCamera.context << ",\"cameraTable\":" << Read<uintptr_t>(currentEyeCamera.context)
        << ",\"cameraNear\":" << Read<uint32_t>(currentEyeCamera.context+0xe8) << ",\"cameraFar\":" << Read<uint32_t>(currentEyeCamera.context+0xec)
        << ",\"viewCurrent\":" << unsigned(currentEyeCamera.view[0x8c])
        << ",\"main\":" << mainObject << ",\"mainTable\":" << Read<uintptr_t>(mainObject) << ",\"mainType\":" << Read<uint32_t>(mainObject+0x14)
        << ",\"mainActive\":" << unsigned(Read<uint8_t>(mainObject+0x18)) << ",\"mainTarget\":" << Read<uintptr_t>(mainObject+0x94)
        << ",\"mainMode\":" << Read<uint32_t>(mainObject+0x9e4) << ",\"worldActive\":" << unsigned(Read<uint8_t>(mainObject+0x138))
        << ",\"visibilityEnabled\":" << unsigned(Read<uint8_t>(mainObject+0x51)) << ",\"alternateScene\":" << unsigned(Read<uint8_t>(mainObject+0x9d))
        << ",\"prepareCalls\":" << frontendPrepareCalls.load() << ",\"visibilityCalls\":" << frontendVisibilityCalls.load()
        << ",\"worldVisibilityCalls\":" << frontendWorldVisibilityCalls.load() << ",\"visibilityFrame\":" << visibilityReadyFrame
        << ",\"garageState\":" << unsigned(garageMenu.state)
        << ",\"garageObject\":" << garageMenu.object << ",\"garageContext\":" << garageMenu.context << "}\n";
}
void RecordSceneReadiness(const CameraContext& complete,bool stable,bool menu) {
    if (!runtimeSession || !diagnosticLogging || !visibilityEventShadow) return;
    static unsigned count{};
    static std::array<uint32_t,6> previous{};
    const std::array<uint32_t,6> current{unsigned(stable),unsigned(menu),
        Read<uint8_t>(mainObject+0x9d),Read<uint8_t>(mainObject+0x138),
        Read<uint32_t>(mainObject+0x9e4),CameraWord(complete,0xec)};
    if (count>=64 || (count && current==previous)) return;
    previous=current;
    std::ofstream out(output/L"scene-readiness.jsonl",count++?std::ios::app:std::ios::trunc);
    out << "{\"frame\":" << frame.load() << ",\"stable\":" << current[0] << ",\"shortPass\":" << current[1]
        << ",\"alternateScene\":" << current[2] << ",\"worldActive\":" << current[3] << ",\"mainMode\":" << current[4]
        << ",\"entryNear\":" << CameraWord(currentEyeCamera.camera,0xe8)
        << ",\"entryFar\":" << CameraWord(currentEyeCamera.camera,0xec)
        << ",\"completeNear\":" << CameraWord(complete,0xe8) << ",\"completeFar\":" << current[5]
        << ",\"visibilityFrame\":" << visibilityReadyFrame.load()
        << ",\"consumerFrame\":" << visibilityConsumerFrame.load()
        << ",\"lastPairFrame\":" << replaySequence.LastFrame()
        << ",\"alternatePairs\":" << alternateVisibilityPairs << "}\n";
}
bool __fastcall Predicate(void* renderer,void*) {
    const auto caller=Rva(_ReturnAddress());
    const bool result=realPredicate(renderer);
    if (!inReplay && runtimeSession && (menuPresentation || !replay3d || !garage3d || frontendSceneAudit) && caller==0x7a7cf4 &&
        reinterpret_cast<uintptr_t>(renderer)==rendererObject && GetCurrentThreadId()==renderThread &&
        currentEyeCamera.valid && currentEyeCamera.frame==frame.load()) {
        CameraContext complete{};
        if (ReadMemory(reinterpret_cast<void*>(currentEyeCamera.context),complete.data(),complete.size())) {
            const auto nearPlane=Read<uint32_t>(mainObject+0x40),farPlane=Read<uint32_t>(mainObject+0x44);
            const bool stable=complete==currentEyeCamera.camera && nearPlane==CameraWord(complete,0xe8) && farPlane==CameraWord(complete,0xec);
            const bool menu=MenuCamera(currentEyeCamera.camera,complete,
                CameraWord(currentEyeCamera.camera,0xe8),nearPlane,farPlane) && ClipInstructionsMatch(currentEyeCamera.context);
            const auto playback=RecordedReplay(gameBase,[](uintptr_t at,void* value,size_t size) {
                return ReadMemory(reinterpret_cast<void*>(at),value,size);
            });
            replaySceneBlocked=!ReplayPresentationAllowed(replay3d,playback);
            UpdateGaragePresentation();
            if(menu || playback!=RecordedReplayState::Inactive || replaySceneBlocked || garageSceneBlocked)
                opponentlod::host::RevokeWorld();
            AuditFrontendScene(complete,stable,menu,playback);
            RecordSceneReadiness(complete,stable,menu);
            if (menu || stable) { menuScene=menu;menuSceneFrame=frame.load(); }
        }
    }
    if (!inReplay && caller==0x7a7cf4 && pendingScene.valid && pendingScene.frame==frame.load() &&
        reinterpret_cast<uintptr_t>(renderer)==rendererObject) {
        Log("scene complete id=%llu stage6_seen=%u native_stereo=%u caller_rva=0x%zx counter=%u",
            pendingScene.id,pendingScene.stage6Seen,result,caller,Read<uint32_t>(mainObject+0xc8));
        if (runtimeSession && dynamicViews && expandVisibility && !frozenViews.Pending() &&
            CanSuspendWithoutVisibility(replaySequence,frozenViews,pendingScene.frame,visibilityConsumerFrame.load(),pendingVisibility.active,
                visibilityHealthy && GetCurrentThreadId()==renderThread && !result && pendingScene.stage6Seen &&
                Read<void**>(mainObject)==mainShadow && Read<void**>(rendererObject)==rendererShadow &&
                Read<uintptr_t>(mainObject+0x28)==rendererObject &&
                Read<uintptr_t>(mainObject+0x30)==visibilityConsumer && Read<void**>(visibilityConsumer)==visibilityShadow &&
                VisibilityInstructionsMatch() && ReplayInstructionsMatch())) {
            // No visibility/pose was modified and no eye started in this frame.
            // Close the center-only GPU scope before resetting owned histories.
            // The normal Present readiness gate will wait for fresh visibility
            // and camera data before starting a new stereo epoch.
            const auto sceneFrame=pendingScene.frame,sceneId=pendingScene.id;
            const bool cancelled=cancelHistory && cancelHistory();
            const bool suspended=cancelled && SuspendSession();
            if (!suspended) FailSession();
            if (suspended) ++visibilityGapSuspensions;
            Log("session visibility_gap frame=%llu scene=%llu cancelled=%u suspended=%u last_consumer=%llu",
                sceneFrame,sceneId,cancelled,suspended,visibilityConsumerFrame.load());
            static unsigned receipts{};
            if (receipts<32) {
                std::ofstream out(output/L"visibility-handoff.jsonl",receipts++?std::ios::app:std::ios::trunc);
                out << "{\"frame\":" << sceneFrame << ",\"scene\":" << sceneId
                    << ",\"lastConsumerFrame\":" << visibilityConsumerFrame.load()
                    << ",\"lastPairFrame\":" << replaySequence.LastFrame()
                    << ",\"epoch\":" << replaySequence.Epoch()
                    << ",\"centerCancelled\":" << (cancelled?"true":"false")
                    << ",\"suspended\":" << (suspended?"true":"false")
                    << ",\"eyesStarted\":false,\"visibilityModified\":false}\n";
            }
            return result;
        }
        if (runtimeSession && ((menuPresentation && menuScene && menuSceneFrame==frame.load()) || replaySceneBlocked || garageSceneBlocked)) {
            // The center frame is complete, but neither eye has begun. Cancel
            // this proved menu transition or explicitly disabled recorded
            // playback/garage, preserving every replay failure/restoration guard.
            const auto sceneFrame=pendingScene.frame;
            const auto sourceNear=CameraWord(currentEyeCamera.camera,0xe8),sourceFar=CameraWord(currentEyeCamera.camera,0xec),
                menuFar=Read<uint32_t>(currentEyeCamera.context+0xec),mainFar=Read<uint32_t>(mainObject+0x44);
            const bool restored=RestoreVisibility("menu_before_replay");
            const bool cancelled=restored && cancelHistory && cancelHistory() &&
                (!dynamicViews || frozenViews.CancelBeforeReplay(sceneFrame));
            if (observePair) observePair(sceneFrame,0,false);
            pendingScene.valid=false;
            if (!cancelled || !SuspendSession()) FailSession();
            Log("session menu frame=%llu cancelled_before_replay=%u source_far=%08x menu_far=%08x main_far=%08x source_near=%08x replay_disabled=%u garage_disabled=%u",
                sceneFrame,cancelled && !SessionFailed(),sourceFar,menuFar,mainFar,sourceNear,replaySceneBlocked,garageSceneBlocked);
            return result;
        }
        if (runtimeSession && visibilityRefusedFrame==pendingScene.frame && visibilityRefusedFrame==frame.load()) {
            CancelTransientScene(visibilityRefusedReason,RestoreVisibility("visibility_refused"));
            return result;
        }
        ObserveCompletedEmptyLists();
        const bool captured=SceneSnapshot(pendingScene,"scene-complete");
        const bool accepted=ReplayOnce(pendingScene,result,captured);
        const bool restored=RestoreVisibility("scene_complete");
        if (!accepted && transientRefusal) {
            CancelTransientScene(transientRefusal,restored);
            return result;
        }
        if (dynamicViews && frozenViews.Pending()) {
            const bool finished=frozenViews.Finish(pendingScene.frame,accepted && restored);
            if (!finished) {visibilityHealthy=false;opponentlod::host::RevokeWorld();}
            Log("views complete id=%llu accepted=%u visibility_restored=%u",pendingScene.id,finished,restored);
            if (observePair) observePair(pendingScene.frame,pendingScene.id,finished);
        }
        pendingScene.valid=false;
    }
    return result; // Preserve the engine's decision; observation cannot enable stereo.
}
void __fastcall Prepare(void* self,void*,uintptr_t arg) {
    if(frontendSceneAudit && !inReplay && reinterpret_cast<uintptr_t>(self)==mainObject)++frontendPrepareCalls;
    StageScope scope(3,self,arg,0,0,_ReturnAddress());
    // The generic 48-stage trace budget ends before a longer eye sequence.
    // Retain these small CPU snapshots for every bounded expanded frame.
    const bool visibility=scope.record || (!inReplay && pendingVisibility.active &&
        pendingVisibility.frame==frame.load() && reinterpret_cast<uintptr_t>(self)==mainObject &&
        GetCurrentThreadId()==renderThread);
    if (visibility) SnapshotVisibility(currentStage.id,"prepare-before");
    if (GetCurrentThreadId()==renderThread && reinterpret_cast<uintptr_t>(self)==mainObject)
        signvisibility::Snapshot(frame.load(),currentStage.id,replayPass,"prepare-before",mainObject,0);
    realPrepare(self,arg);
    if (visibility) SnapshotVisibility(currentStage.id,"prepare-after");
    if (GetCurrentThreadId()==renderThread && reinterpret_cast<uintptr_t>(self)==mainObject)
        signvisibility::Snapshot(frame.load(),currentStage.id,replayPass,"prepare-after",mainObject,0);
}
void __fastcall Packet(void* self,void*,uintptr_t arg) {
    StageScope scope(9,self,arg,0,0,_ReturnAddress()); realPacket(self,arg);
}
void __fastcall Stage5(void* self,void*,uintptr_t context,uintptr_t view,uintptr_t packet) {
    if (inReplay && eyeOffsetEnabled) {
        // CompleteScene has just built this eye's main camera, before the
        // history update and before stage 6 may select the near-object camera.
        replayMainCameraValid=++replayMainSamples==1 && GetCurrentThreadId()==renderThread &&
            Rva(_ReturnAddress())==0x3be09e && reinterpret_cast<uintptr_t>(self)==mainObject &&
            pendingScene.valid && pendingScene.frame==frame.load() &&
            context==pendingScene.call.args[3] && packet==pendingScene.call.args[0] && Read<uint8_t>(view+0x8c)==0 &&
            ReadMemory(reinterpret_cast<void*>(context),replayMainCamera.data(),replayMainCamera.size()) &&
            DumpStage(pendingScene.id,"replay-main","context",context,512);
    }
    RememberScene(5,self,context,view,packet,_AddressOfReturnAddress());
    StageScope scope(5,self,context,view,packet,_ReturnAddress()); realStage5(self,context,view,packet);
    if (!inReplay && pendingScene.valid && pendingScene.frame==frame.load() && reinterpret_cast<uintptr_t>(self)==mainObject)
        SceneSnapshot(pendingScene,"scene-after5");
}
void __fastcall Stage6(void* self,void*,uintptr_t context,uintptr_t view,uintptr_t packet) {
    RememberScene(6,self,context,view,packet,_AddressOfReturnAddress());
    if (!inReplay && pendingScene.valid && pendingScene.stage6Seen && pendingScene.frame==frame.load())
        SceneSnapshot(pendingScene,"scene-before6");
    StageScope scope(6,self,context,view,packet,_ReturnAddress()); realStage6(self,context,view,packet);
    if (waitForClipTransition && !clipObserved && !inReplay && GetCurrentThreadId()==renderThread &&
        Rva(_ReturnAddress())==0x3be1b8 && reinterpret_cast<uintptr_t>(self)==mainObject &&
        clipTrigger.frame==frame.load() && clipTrigger.context==context) {
        clipTrigger.Observe(frame.load(),context,Read<uint32_t>(context+0xe8),Read<uint32_t>(context+0xec));
        if (clipTrigger.ready && !clipObserved.exchange(true))
            Log("capture trigger observed=clip_transition context=%p source=main_scene_boundaries",reinterpret_cast<void*>(context));
    }
}
bool InstallStages() {
    for (size_t i=0;i<mainMethods.size();++i)
        if (Read<uintptr_t>(gameBase+mainTable+i*4)!=gameBase+mainMethods[i]) return false;
    constexpr uint8_t prepare[]{0x55,0x8b,0xec,0x83,0xec,0x08,0x56,0x8b,0xf1,0x80,0xbe,0xfc,0x04,0x00,0x00,0x00};
    constexpr uint8_t stage5[]{0x55,0x8b,0xec,0x56,0x57,0x8b,0x7d,0x08,0xd9,0x87,0xe8,0,0,0,0x8b,0xf1};
    uint8_t stage6[]{0x55,0x8b,0xec,0x80,0x3d,0,0,0,0,0,0x56,0x8b,0xf1,0x74,0x3a,0x6a,0x01};
    const auto relocated=gameBase+0xe30c24;
    memcpy(stage6+5,&relocated,sizeof(relocated));
    constexpr uint8_t packet[]{0x55,0x8b,0xec,0x53,0x56,0x8b,0x75,0x08,0x56,0x8b,0xd9,0xe8,0x10,0x8c,0xbf,0xff};
    if (!MatchesInstructions(reinterpret_cast<void*>(gameBase+mainMethods[3]),prepare) ||
        !MatchesInstructions(reinterpret_cast<void*>(gameBase+mainMethods[5]),stage5) ||
        !MatchesInstructions(reinterpret_cast<void*>(gameBase+mainMethods[6]),stage6) ||
        !MatchesInstructions(reinterpret_cast<void*>(gameBase+mainMethods[9]),packet)) return false;
    realPrepare=reinterpret_cast<SingleFn>(gameBase+mainMethods[3]);
    realStage5=reinterpret_cast<SceneFn>(gameBase+mainMethods[5]);
    realStage6=reinterpret_cast<SceneFn>(gameBase+mainMethods[6]);
    realPacket=reinterpret_cast<SingleFn>(gameBase+mainMethods[9]);
    const VtablePatch patches[]{
        {3,reinterpret_cast<void*>(realPrepare),reinterpret_cast<void*>(Prepare)},
        {5,reinterpret_cast<void*>(realStage5),reinterpret_cast<void*>(Stage5)},
        {6,reinterpret_cast<void*>(realStage6),reinterpret_cast<void*>(Stage6)},
        {9,reinterpret_cast<void*>(realPacket),reinterpret_cast<void*>(Packet)}};
    // Copy the complete contiguous function-pointer span, including adjacent
    // table entries. The main object's known methods occupy its first 16 slots.
    mainShadow=AttachShadowVtable(reinterpret_cast<void*>(mainObject),reinterpret_cast<void**>(gameBase+mainTable),39,patches);
    return mainShadow!=nullptr;
}

uintptr_t FindMainObject() {
    std::vector<uintptr_t> candidates;
    std::array<uintptr_t,16384> words{};
    uintptr_t address{};
    uint64_t scanned{};
    const auto started=GetTickCount64();
    MEMORY_BASIC_INFORMATION region{};
    while (VirtualQuery(reinterpret_cast<void*>(address),&region,sizeof(region))) {
        const auto start=reinterpret_cast<uintptr_t>(region.BaseAddress);
        const auto end=static_cast<uint64_t>(start)+region.RegionSize;
        if (region.State==MEM_COMMIT && region.Type==MEM_PRIVATE && region.Protect==PAGE_READWRITE) {
            for (size_t offset=0;offset<region.RegionSize;offset+=sizeof(words)) {
                if (GetTickCount64()-started>10000 || scanned>2ull*1024*1024*1024) {
                    Log("object discovery refused: scan bound reached"); return 0;
                }
                const auto bytes=(std::min)(sizeof(words),static_cast<size_t>(region.RegionSize-offset));
                scanned+=bytes;
                if (!ReadMemory(reinterpret_cast<void*>(start+offset),words.data(),bytes)) continue;
                for (size_t i=0;i<bytes/sizeof(uintptr_t);++i) {
                    if (words[i]!=gameBase+mainTable) continue;
                    const auto object=start+offset+i*sizeof(uintptr_t);
                    const auto renderer=Read<uintptr_t>(object+0x28);
                    if (Read<uint32_t>(object+0x14)==0x400 && Read<uint8_t>(object+0x18)==1 &&
                        Read<uintptr_t>(object+0x94) && Read<uintptr_t>(renderer)==gameBase+0xd4a5a0)
                        candidates.push_back(object);
                }
            }
        }
        if (end>UINTPTR_MAX || end<=address) break;
        address=static_cast<uintptr_t>(end);
    }
    Log("main object discovery candidates=%zu scanned_bytes=%llu elapsed_ms=%llu",candidates.size(),scanned,GetTickCount64()-started);
    return candidates.size()==1 ? candidates.front():0;
}
}
void ObserveFaults(uintptr_t base,const std::filesystem::path& folder) {
    if (faultFile!=INVALID_HANDLE_VALUE) return;
    gameBase=base; output=folder; renderThread=GetCurrentThreadId();
    faultFile=CreateFileW((output/L"engine-faults.txt").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    faultContextFile=CreateFileW((output/L"engine-fault-context.txt").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (faultFile!=INVALID_HANDLE_VALUE) AddVectoredExceptionHandler(1,ObserveException);
}
InstallResult Install(uintptr_t base,const std::filesystem::path& folder,BoundaryObserver boundary,ViewProvider views,PairObserver complete,HistoryReset reset,HistoryReset cancel) {
    static auto result=InstallResult::Waiting;
    if (result!=InstallResult::Waiting) return result;
    // Missing live objects during loading are retryable. Every configuration,
    // instruction or hook failure remains terminal, including partial installs.
    result=InstallResult::Refused;
    if (!std::filesystem::exists(folder/L"engine-trace.request")) return result;
    gameBase=base; output=folder;
    runtimeSession=std::filesystem::exists(folder/L"runtime-session.request");resetHistory=reset;cancelHistory=cancel;
    replay3d=!std::filesystem::exists(folder/L"flat-replays.request");
    garage3d=!std::filesystem::exists(folder/L"flat-garage.request");
    frontendSceneAudit=std::filesystem::exists(folder/L"frontend-scene-audit.request");
    diagnosticLogging=!runtimeSession || std::filesystem::exists(folder/L"diagnostic-logging.request");
    nativeVisibilityControl=std::filesystem::exists(folder/L"native-visibility-control.request");
    trackedVisibility=std::filesystem::exists(folder/L"tracked-visibility.request");
    const bool headsetCandidate=HeadsetVisibilityCandidate(folder);
    if(std::filesystem::exists(folder/L"headset-visibility-candidate.request") && !headsetCandidate) {
        Log("engine trace refused: incomplete headset visibility candidate");return result;
    }
    if(std::filesystem::exists(folder/L"instance-fallback-candidate.request")) {
        Log("engine trace refused: rejected instance fallback candidate; ordinary renderer prerequisites unresolved");return result;
    }
    if (nativeVisibilityControl && (!runtimeSession || !std::filesystem::exists(folder/L"recorded-views.request") ||
        !std::filesystem::exists(folder/L"flicker-diagnostic.request") ||
        std::filesystem::exists(folder/L"xr-projection.request") || std::filesystem::exists(folder/L"xr-session.request"))) {
        Log("engine trace refused: native visibility control requires bounded recorded-view diagnostics"); return result;
    }
    if (trackedVisibility && !headsetCandidate && (nativeVisibilityControl || !runtimeSession ||
        !std::filesystem::exists(folder/L"recorded-views.request") ||
        !std::filesystem::exists(folder/L"flicker-diagnostic.request") ||
        !std::filesystem::exists(folder/L"expand-visibility.request") ||
        std::filesystem::exists(folder/L"xr-projection.request") || std::filesystem::exists(folder/L"xr-session.request"))) {
        Log("engine trace refused: tracked visibility requires exclusive bounded recorded-view diagnostics"); return result;
    }
    packedSnapshots=std::filesystem::exists(folder/L"packed-snapshots.request");
    constexpr uint8_t prologue[]{0x53,0x8b,0xdc,0x83,0xec,0x08,0x83,0xe4,0xf0,0x83,0xc4,0x04,0x55,0x8b,0x6b,0x04};
    auto** slot=reinterpret_cast<void**>(base+0xd4a5a0+81*4);
    auto* target=reinterpret_cast<void*>(base+0x958060);
    if (!MatchesInstructions(target,prologue) || Read<uintptr_t>(base+0xd4a5a0+54*4)!=base+0x93bdf0 ||
        Read<uintptr_t>(reinterpret_cast<uintptr_t>(slot))!=reinterpret_cast<uintptr_t>(target)) {
        Log("engine trace refused: camera instructions or vtable mismatch"); return result;
    }
    if (!ReadMemory(reinterpret_cast<void*>(base+0xd4a5a0),originalRendererTable.data(),sizeof(originalRendererTable)) ||
        !ReadMemory(reinterpret_cast<void*>(base+mainTable),originalMainTable.data(),sizeof(originalMainTable))) return result;
    mainObject=FindMainObject();
    rendererObject=mainObject?Read<uintptr_t>(mainObject+0x28):0;
    if (!mainObject) {
        Log("engine trace waiting: no unique live main object");
        return result=InstallResult::Waiting;
    }
    realCamera=reinterpret_cast<CameraFn>(target);
    sceneBoundaryEnabled=std::filesystem::exists(folder/L"scene-boundary-trace.request");
    traceVisibility=std::filesystem::exists(folder/L"visibility-trace.request");
    constexpr uint8_t predicate[]{0x8a,0x81,0x2e,0x51,0,0,0xc3};
    constexpr uint8_t scenePrologue[]{0x55,0x8b,0xec,0x53,0x56,0x8b,0xf1,0x8b,0x46,0x14,0x57,0xbb,1,0,0,0};
    constexpr uint8_t completeSite[]{0x8b,0x4e,0x28,0x8b,0x11,0x8b,0x82,0xd8,0,0,0,0xff,0xd0,0x84,0xc0};
    if (sceneBoundaryEnabled && (!MatchesInstructions(reinterpret_cast<void*>(base+0x93bdf0),predicate) ||
        !MatchesInstructions(reinterpret_cast<void*>(base+0x3be050),scenePrologue) ||
        !MatchesInstructions(reinterpret_cast<void*>(base+0x7a7ce7),completeSite))) {
        Log("engine trace refused: scene boundary instruction guards"); return result;
    }
    const auto owner=instancecapacity::ReadyToBind(mainObject);
    if(owner!=instancecapacity::BindReadiness::Ready) {
        if(owner==instancecapacity::BindReadiness::Waiting) {
            Log("engine trace waiting: instance owner is not initialized");
            return result=InstallResult::Waiting;
        }
        Log("engine trace refused: malformed initialized instance owner");return result;
    }
    realPredicate=reinterpret_cast<PredicateFn>(base+0x93bdf0);
    const VtablePatch patches[]{
        {81,target,reinterpret_cast<void*>(Camera)},
        {54,reinterpret_cast<void*>(realPredicate),reinterpret_cast<void*>(Predicate)}};
    rendererShadow=AttachShadowVtable(reinterpret_cast<void*>(rendererObject),reinterpret_cast<void**>(base+0xd4a5a0),88,{patches,sceneBoundaryEnabled?2u:1u});
    installed=rendererShadow!=nullptr;
    Log("engine vtable trace installed=%u mode=object_shadow main=%p renderer=%p instructions_unchanged=%u original_slot_unchanged=%u",
        installed.load(),reinterpret_cast<void*>(mainObject),reinterpret_cast<void*>(rendererObject),MatchesInstructions(target,prologue),*slot==target);
    if (installed) {
        observeBoundary=boundary;
        provideViews=views;observePair=complete;
        const bool stagesInstalled=InstallStages();
        expandVisibility=traceVisibility && std::filesystem::exists(folder/L"expand-visibility.request");
        const bool visibilityInstalled=!traceVisibility || (stagesInstalled && InstallVisibility());
        if (!visibilityInstalled) {visibilityHealthy=false;opponentlod::host::RevokeWorld();}
        replayEnabled=stagesInstalled && sceneBoundaryEnabled &&
            std::filesystem::exists(folder/L"replay-once.request") &&
            (runtimeSession || std::filesystem::exists(folder/L"short-probe.request")) &&
            std::filesystem::exists(folder/L"stage-target-capture.request") && ReplayInstructionsMatch();
        traceReplayWork=replayEnabled && std::filesystem::exists(folder/L"replay-work.request");
        eyeOffsetEnabled=traceReplayWork && std::filesystem::exists(folder/L"eye-offset.request");
        if (runtimeSession) {
            if (!replaySequence.ConfigureSession()) replayEnabled=false;
        } else if (eyeOffsetEnabled && std::filesystem::exists(folder/L"eye-sequence.request")) {
            unsigned count{}; std::ifstream request(folder/L"eye-sequence.request"); request>>count;
            const bool compactStereo=packedSnapshots && std::filesystem::exists(folder/L"compact-stereo.request") &&
                std::filesystem::exists(folder/L"stereo-eyes.request");
            if (!request || !replaySequence.Configure(count,compactStereo) || count<2) {
                replayEnabled=false; Log("sequence refused reason=invalid_count");
            } else Log("sequence requested count=%u",count);
        }
        ownedEyeHistory=eyeOffsetEnabled && (runtimeSession || replaySequence.Limit()>1) && std::filesystem::exists(folder/L"eye-camera-history.request");
        if (std::filesystem::exists(folder/L"eye-yaw.request")) {
            int degrees{}; std::ifstream request(folder/L"eye-yaw.request"); request>>degrees;
            const auto absolute=std::abs(degrees);
            if (!request || !ownedEyeHistory || (absolute!=15 && absolute!=60 && absolute!=90) ||
                (absolute>15 && !expandVisibility)) {
                replayEnabled=false; Log("sequence refused reason=invalid_yaw");
            } else eyeYaw=degrees*0.017453292519943295f;
        }
        asymmetricEyeFov=std::filesystem::exists(folder/L"eye-asymmetric-fov.request");
        fullEyePose=std::filesystem::exists(folder/L"eye-pose.request");
        if (fullEyePose) {
            std::ifstream request(folder/L"eye-pose.request");
            for (auto& value:eyePose.orientation) request>>value;
            for (auto& value:eyePose.position) request>>value;
            PoseRotation axes{};
            const bool parsed=bool(request); request>>std::ws;
            if (!parsed || !request.eof() || !ownedEyeHistory || !expandVisibility ||
                std::filesystem::exists(folder/L"eye-yaw.request") || !PoseAxes(eyePose,axes)) {
                replayEnabled=false; Log("sequence refused reason=invalid_full_pose");
            }
        }
        if (asymmetricEyeFov && !ownedEyeHistory) { replayEnabled=false; Log("sequence refused reason=fov_requires_history"); }
        dualEyes=std::filesystem::exists(folder/L"stereo-eyes.request");
        if (dualEyes && (!fullEyePose || !ownedEyeHistory || !asymmetricEyeFov || !expandVisibility ||
            !SeparateSceneEyes(eyePose,diagnosticEyeOffset,stereoPoses))) {
            replayEnabled=false; Log("sequence refused reason=invalid_stereo_pose");
        }
        waitForClipTransition=replayEnabled && std::filesystem::exists(folder/L"wait-clip-transition.request");
        headsetProjection=std::filesystem::exists(folder/L"xr-projection.request");
        // A simulated UI-stereo run must exercise the same loading/pause
        // transitions as a headset run, including the shared-screen fallback.
        menuPresentation=headsetProjection || std::filesystem::exists(folder/L"screen-ui-stereo.request");
        dynamicViews=std::filesystem::exists(folder/L"recorded-views.request") || headsetProjection;
        if (dynamicViews && (!dualEyes || !expandVisibility || !provideViews || !observePair)) {
            replayEnabled=false;Log("sequence refused reason=invalid_dynamic_views");
        }
        if (runtimeSession && (!replayEnabled || !dynamicViews || !dualEyes || !resetHistory || !visibilityInstalled ||
            !std::filesystem::exists(folder/L"compact-stereo.request") ||
            !std::filesystem::exists(folder/L"restore-compute-state.request") ||
            !std::filesystem::exists(folder/L"sort-vpl.request") ||
            !std::filesystem::exists(folder/L"isolate-shadow-sampler.request") ||
            !std::filesystem::exists(folder/L"normalize-shadow-bindings.request") ||
            !std::filesystem::exists(folder/L"disable-motion-blur.request"))) FailSession();
        // Normal/race owners finish initialization later than attract mode.
        // Defer read-only lazy-code and resource qualification to a world frame;
        // allocation-call telemetry is excluded from these interactive modes.
        deferredNativeHooks=runtimeSession && (std::filesystem::exists(folder/L"normal-launch.request") ||
            std::filesystem::exists(folder/L"race-launch.request"));
        if (replayEnabled && !deferredNativeHooks && !VerifyLazyLists()) { replayEnabled=false;Log("replay refused reason=lazy_list_instructions");if(runtimeSession)FailSession(); }
        instanceObservationPolicy={std::filesystem::exists(folder/L"instance-allocation-telemetry.request"),runtimeSession,
            std::filesystem::exists(folder/L"recorded-views.request"),std::filesystem::exists(folder/L"flicker-unattended.request"),
            std::filesystem::exists(folder/L"flicker-diagnostic.request"),headsetCandidate || headsetProjection || std::filesystem::exists(folder/L"xr-session.request"),
            std::filesystem::exists(folder/L"normal-launch.request"),std::filesystem::exists(folder/L"race-launch.request"),
            std::filesystem::exists(folder/L"player-control.request")};
        instanceObservationRequested=replayEnabled && !instanceObservationPolicy.Suppression();
        Log("instance allocation telemetry configured requested=%u allowed=%u reason=%s capacity_guards=%u",
            instanceObservationPolicy.requested,instanceObservationRequested,
            instanceObservationPolicy.Suppression()?instanceObservationPolicy.Suppression():"isolated_ai",instancecapacity::Enabled());
        if(replayEnabled && !deferredNativeHooks &&
            !instancecapacity::QualifyReplayResources(instancecapacity::Enabled(),instanceObservationRequested,
                []{return instancecapacity::Bind(mainObject);},[]{return InstallInstanceObservation();})) {
            replayEnabled=false;FailSession();Log("instance capacity or allocation telemetry refused");
        }
        if (waitForClipTransition) Log("capture trigger waiting=clip_transition");
        Log("main stage trace installed=%u mode=object_shadow table_rva=0xcd9da8 slots=3,5,6,9",stagesInstalled);
        Log("scene boundary observation enabled=%u replay_enabled=%u",sceneBoundaryEnabled,replayEnabled);
    }
    return result=installed?InstallResult::Installed:InstallResult::Refused;
}
void WriteSessionStatus() {
    std::ofstream status(output/L"runtime-status.json",std::ios::trunc);
    status << "{\"frame\":" << frame.load() << ",\"state\":" << static_cast<unsigned>(replaySequence.Status())
        << ",\"epoch\":" << replaySequence.Epoch() << ",\"attemptedPairs\":" << replaySequence.Attempts()
        << ",\"acceptedPairs\":" << replaySequence.Completed() << ",\"failed\":" << (SessionFailed()?"true":"false")
        << ",\"emptyBuilderPairs\":" << emptyBuilderPairs << ",\"lastEmptyBuilderFrame\":" << lastEmptyBuilderFrame
        << ",\"visibilityGapSuspensions\":" << visibilityGapSuspensions
        << ",\"transientSuspensions\":" << transientSuspensions << ",\"lastTransientRefusal\":\"" << lastTransientRefusal << '"'
        << ",\"alternateVisibilityCalls\":" << alternateVisibilityCalls
        << ",\"alternateVisibilityPairs\":" << alternateVisibilityPairs
        << ",\"visibilityDispatchCodeUnchanged\":" << (VisibilityDispatchCode(gameBase,ReadLazyMemory)?"true":"false")
        << ",\"instanceAllocationTelemetry\":" << (realInstanceAllocate?"true":"false")
        << ",\"instanceAllocationTelemetryRequested\":" << (instanceObservationPolicy.requested?"true":"false")
        << ",\"instanceAllocationTelemetryPolicy\":\"" << (instanceObservationPolicy.Suppression()?instanceObservationPolicy.Suppression():"isolated_ai") << '"'
        << ",\"instanceAllocationEntryUnchanged\":" << (MatchesInstructions(reinterpret_cast<void*>(gameBase+0x185940),instanceAllocateEntry)?"true":"false")
        << ",\"instanceCapacityGuards\":" << (instancecapacity::Enabled()?"true":"false")
        << ",\"instanceRequests\":" << instanceRequests.load() << ",\"instanceFailures\":" << instanceFailures.load();
    instancecapacity::Status(status);status << "}\n";
}
void Frame(uint64_t nextFrame) {
    if (sessionFailureRequested.exchange(false)) FailSession();
    if (pendingVisibility.active) RestoreVisibility("present_fallback");
    if (frozenViews.Pending()) {
        const auto dropped=frozenViews.Frame();frozenViews.Finish(dropped,false);visibilityHealthy=false;
        opponentlod::host::RevokeWorld();
        Log("views incomplete source_frame=%llu",dropped);
        if (observePair) observePair(dropped,0,false);
    }
    if (runtimeSession && !visibilityHealthy) FailSession();
    // No scene since the preceding Present means the native game stopped
    // rendering a race (e.g. loading). Only a completely restored pair may suspend.
    if (runtimeSession && SessionRendering() && replaySequence.LastFrame()!=nextFrame-1) SuspendSession();
    frame=nextFrame;
    if (!installed) return;
    if (deferredNativeHooks && replayEnabled && NativeWorldReady()) {
        const bool lazy=VerifyLazyLists();
        const bool observation=instancecapacity::QualifyReplayResources(instancecapacity::Enabled(),instanceObservationRequested,
            []{return instancecapacity::Bind(mainObject);},[]{return InstallInstanceObservation();});
        deferredNativeHooks=false;
        Log("world native hooks frame=%llu lazy=%u instance=%u",nextFrame,lazy,observation);
        if(!lazy || !observation) {replayEnabled=false;FailSession();}
    }
    AuditFrontendReadiness(nextFrame);
    if (nextFrame%1200==0) {
        std::array<uintptr_t,88> renderer{};
        std::array<uintptr_t,39> main{};
        std::array<uintptr_t,30> visibility{};
        const bool unchanged=ReadMemory(reinterpret_cast<void*>(gameBase+0xd4a5a0),renderer.data(),sizeof(renderer)) &&
            ReadMemory(reinterpret_cast<void*>(gameBase+mainTable),main.data(),sizeof(main)) &&
            renderer==originalRendererTable && main==originalMainTable &&
            (!visibilityEventShadow || (VisibilityDispatchCode(gameBase,ReadLazyMemory) &&
                Read<void**>(mainObject+visibilityEventOffset)==visibilityEventShadow)) &&
            (!visibilityShadow || (ReadMemory(reinterpret_cast<void*>(gameBase+0xcd9bb8),visibility.data(),sizeof(visibility)) && visibility==originalVisibilityTable));
        Log("engine image tables unchanged=%u",unchanged);
        if (runtimeSession && !unchanged) FailSession();
    }
    if (runtimeSession) {
        static bool failureReported=false;
        if (nextFrame%120==0 || (SessionFailed() && !failureReported)) {
            WriteSessionStatus();
            failureReported=SessionFailed();
        }
        cameras=0;for (auto& calls:stageCalls) calls=0;
        return;
    }
    static std::ofstream csv(output/L"engine-vtable-frames.csv");
    static bool header=false;
    if (!header) { csv << "frame,camera_uploads,slot3,slot5,slot6,slot9\n"; header=true; }
    csv << nextFrame-1 << ',' << cameras.exchange(0);
    for (unsigned slot:{3,5,6,9}) csv << ',' << stageCalls[slot].exchange(0);
    csv << '\n';
    if (nextFrame%120==0 || (captureEnd.load() && nextFrame==captureEnd.load()+1)) csv.flush();
}
void CaptureUntil(uint64_t endFrame) { samples=0; stageSamples=0; captureEnd=endFrame; }
bool ReadyForCapture() {
    return CaptureWaitReason()==nullptr;
}
uint64_t NativeCameraFrame() { return currentEyeCamera.valid?currentEyeCamera.frame:0; }
bool NativeWorldReady() {
    return installed && mainObject && currentEyeCamera.valid &&
        currentEyeCamera.frame+1==frame.load() && visibilityReadyFrame &&
        visibilityReadyFrame+1==frame.load() && Read<uint8_t>(mainObject+0x138)==1;
}
bool OpponentLodWorldReady() {
    // This reads native/XR-adjacent state only on the render/controller thread.
    // Workers invalidate atomically through RevokeWorld, never through this API.
    if(GetCurrentThreadId()!=renderThread || !installed || !mainObject || !rendererObject) return false;
    opponentlod::host::CenterWorld state{};
    state.presentFrame=frame.load();state.cameraFrame=currentEyeCamera.frame;
    state.consumerFrame=opponentVisibilityFrame.load();state.pairFrame=replaySequence.LastFrame();
    state.sceneFrame=menuSceneFrame;
    state.controller=GetCurrentThreadId()==renderThread;state.installed=installed.load();
    state.rendering=SessionRendering() && replaySequence.Available();
    state.failed=SessionFailed() || sessionFailureRequested.load();
    state.replaying=inReplay || replayPass!=0;state.cameraValid=currentEyeCamera.valid;
    state.objectsOwned=Read<void**>(mainObject)==mainShadow && Read<void**>(rendererObject)==rendererShadow &&
        Read<uintptr_t>(mainObject+0x28)==rendererObject && Read<uintptr_t>(mainObject+0x30)==visibilityConsumer &&
        Read<void**>(visibilityConsumer)==visibilityShadow && Read<uintptr_t>(mainObject+0x2c)==currentEyeCamera.manager &&
        Read<uintptr_t>(currentEyeCamera.manager)==gameBase+0xcf8108 && Read<uint32_t>(currentEyeCamera.manager+0xa0)==0;
    state.worldActive=Read<uint8_t>(mainObject+0x138)==1;
    state.visibilityEnabled=Read<uint8_t>(mainObject+0x51)==1;
    state.visibilityHealthy=visibilityHealthy.load() && state.consumerFrame==visibilityConsumerFrame.load();
    const auto read=[](uintptr_t at,void* value,size_t size) {return ReadMemory(reinterpret_cast<void*>(at),value,size);};
    state.normalPhase=!menuScene && !replaySceneBlocked && !garageSceneBlocked &&
        RecordedReplay(gameBase,read)==RecordedReplayState::Inactive &&
        ReadGarageMenu(gameBase,read).state==GarageMenuState::Inactive;
    return opponentlod::host::Eligible(state);
}
const char* CaptureWaitReason() {
    if (runtimeSession) {
        if (!installed) return "engine_not_installed";
        if (SessionFailed()) return "session_failed";
        if (!replayEnabled) return "replay_disabled";
        if (deferredNativeHooks) return "native_world_startup";
        if (!currentEyeCamera.valid || currentEyeCamera.frame+1!=frame.load()) return "native_camera";
        if (replaySceneBlocked) return "replay_3d_disabled";
        if (garageSceneBlocked) return "garage_3d_disabled";
        if (menuPresentation && (menuScene || menuSceneFrame+1!=frame.load())) return "menu_or_incomplete_scene";
        if (TransientRefusalHold(transientRefusalFrame,frame.load())) return "transient_refusal";
    }
    if (waitForClipTransition && !clipObserved) return "clip_transition";
    if (eyeOffsetEnabled && !eyeHistoryPrimed) return "camera_history";
    if (expandVisibility && (!visibilityReadyFrame || visibilityReadyFrame+1!=frame.load())) return "native_visibility";
    return nullptr;
}
bool Continuous() { return runtimeSession && replaySequence.Continuous(); }
bool SessionRendering() { return Continuous() && replaySequence.Status()==ReplaySequence::State::Rendering; }
bool SessionFailed() { return runtimeSession && replaySequence.Status()==ReplaySequence::State::Failed; }
void FailSession() {
    if (GetCurrentThreadId()!=renderThread) {
        sessionFailureRequested=true;opponentlod::host::RevokeWorld();return;
    }
    opponentlod::host::RevokeWorld();
    if (runtimeSession && !SessionFailed()) { replaySequence.Fail();replayEnabled=false;Log("session failed=1");WriteSessionStatus(); }
}
bool SuspendSession() {
    opponentlod::host::RevokeWorld();
    if (!Continuous() || GetCurrentThreadId()!=renderThread || SessionFailed()) return false;
    if (replaySequence.Status()==ReplaySequence::State::Suspended || replaySequence.Status()==ReplaySequence::State::Stopped) return true;
    if (inReplay || pendingVisibility.active || frozenViews.Pending() || !visibilityHealthy ||
        !replaySequence.Suspend() || !resetHistory || !resetHistory() || !frozenViews.ResetEpoch()) {
        FailSession();return false;
    }
    currentEyeCamera={};previousEyeCamera={};renderedEyeHistories={};pendingScene={};
    eyeHistoryPrimed=false;visibilityReadyFrame=0;opponentVisibilityFrame=0;
    Log("session suspended=1 epoch=%llu",replaySequence.Epoch());return true;
}
bool ResumeSession() {
    if (!runtimeSession || GetCurrentThreadId()!=renderThread || !ReadyForCapture() ||
        replaySequence.Status()!=ReplaySequence::State::Suspended) return false;
    if (!resetHistory || !resetHistory()) { FailSession();return false; }
    if (!replaySequence.Resume()) return false;
    Log("session rendering=1 epoch=%llu",replaySequence.Epoch());return true;
}
bool SuspendForSwapchainChange() {
    opponentlod::host::RevokeWorld();
    if (!Continuous() || GetCurrentThreadId()!=renderThread || SessionFailed()) return false;
    if (pendingVisibility.active || frozenViews.Pending()) {
        const auto current=frame.load();
        // A resize is not a completed scene. Only the visibility-only prefix
        // can be abandoned; an active scene/eye or foreign volume must refuse.
        if (!replaySequence.Available() || inReplay || pendingScene.valid || !visibilityHealthy ||
            !pendingVisibility.active || pendingVisibility.frame!=current ||
            !frozenViews.Get(current) || replaySequence.LastFrame()>=current || !observePair) return false;
        if (!RestoreVisibility("swapchain_before_scene") || !frozenViews.CancelBeforeReplay(current)) return false;
        observePair(current,0,false); // End the acquired XR frame with no layers.
        if (SessionFailed()) return false;
        Log("session swapchain frame=%llu cancelled_before_scene=1",current);
    }
    return SuspendSession();
}
bool StopSession() {
    opponentlod::host::RevokeWorld();
    if (!SuspendSession() || !replaySequence.Stop()) return false;
    Log("session stopped=1 accepted=%llu",replaySequence.Completed());WriteSessionStatus();return true;
}
unsigned CaptureFrameCount() { return replaySequence.Limit()+2; }
bool RepeatingEyes() { return runtimeSession || replaySequence.Limit()>1; }
unsigned EyePairCount() { return replaySequence.Limit(); }
bool ObservingEyes() { return replaySequence.Observing(); }
bool DualEyes() { return dualEyes; }
std::string SnapshotPhase(const char* phase) {
    return replayPass==2 && (!strncmp(phase,"replay-",7) || !strncmp(phase,"eye-previous",12)) ? std::string("right-")+phase : phase;
}
CameraStamp CurrentCamera() { return currentCamera; }
StageStamp CurrentStage() { return currentStage; }
SceneStamp CurrentScene() {
    return pendingScene.valid && pendingScene.frame==frame.load() ? SceneStamp{pendingScene.id,inReplay,replayPass}:SceneStamp{};
}
bool ReplayingEye() { return inReplay; }
const SceneViews* CurrentReplayViews() {
    return inReplay && dualEyes && fullEyePose && asymmetricEyeFov ? frozenViews.Get(frame.load()):nullptr;
}
bool CurrentLightCameras(std::array<uint8_t,512>& center,std::array<uint8_t,512>& eye) {
    if (GetCurrentThreadId()!=renderThread || !CurrentReplayViews() ||
        !pendingScene.valid || pendingScene.frame!=frame.load() || !pendingScene.replayInputsSaved ||
        !replayMainCameraValid || replayPass<1 || replayPass>2) return false;
    center=pendingScene.context;eye=replayMainCamera;return true;
}
bool CurrentCockpitCamera() {
    // Original scene identity is shared by center/left/right. Do not classify
    // an eye-adjusted projection or an auxiliary/shadow camera as a cockpit.
    return GetCurrentThreadId()==renderThread && pendingScene.valid &&
        pendingScene.frame==frame.load() && pendingScene.replayInputsSaved &&
        CockpitExposure::CameraSupported(pendingScene.nearPlane,pendingScene.farPlane);
}
}
