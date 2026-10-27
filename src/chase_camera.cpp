#define DIRECTINPUT_VERSION 0x0800
#include "chase_camera.h"
#include "chase_camera_request.h"
#include "chase_camera_adapter.h"
#include "chase_camera_guard.h"
#include "chase_camera_input.h"
#include "chase_camera_input_guard.h"
#include "chase_camera_state.h"
#include "chase_camera_region_guard.h"
#include "chase_camera_playback_state.h"
#include "chase_camera_transaction.h"
#include "chase_camera_collision.h"
#include "chase_camera_replay.h"
#include "driving_controls_patch.h"
#include "chase_observer_guard.h"
#include "solo_race.h"
#include "engine_trace.h"
#include <dinput.h>
#include <xinput.h>
#include <intrin.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>

namespace grid2vr::chasecamera {
namespace {
using Native=uintptr_t (__thiscall*)(void*,void*);
using Alternate=uintptr_t (__thiscall*)(void*,void*,uint32_t);
using Getter=uintptr_t (__thiscall*)(void*);
using TargetHelper=uintptr_t (__thiscall*)(void*,void*,uint32_t);
TargetHelper originalTarget{};
bool targetCallInstalled{},hooksNeedCleanup{};
uint64_t collisionTargets{};
constexpr std::array<size_t,8> routes{0,1,3,4,6,2,0,0};
constexpr std::array<size_t,7> hookIndexes{0,1,2,4,5,6,7};
uintptr_t ControlSlot(size_t i) {return i==7?transactionCopyBackSlotRva:i==6?lookSlotRva:chaseobserver::SlotRva(routes[i]);}
uintptr_t ControlRoutine(size_t i) {return i==7?transactionCopyBackRva:i==6?lookRoutineRva:chaseobserver::routines[routes[i]];}
std::array<void*,8> original{};
uintptr_t base{};
std::filesystem::path folder;
std::timed_mutex mutex;
std::mutex inputMutex;
std::atomic<bool> enabled{},stopping{};
std::atomic<bool> stopPending{};
std::atomic<unsigned> evaluations{},windowState{};
std::atomic<uint64_t> frame{},contention{},suspensionEpoch{};
std::atomic<uint64_t> nativeEvaluation{};
thread_local CameraTransaction transaction;
std::atomic<unsigned> outstandingTransactions{};
thread_local bool transactionCounted{};
thread_local uintptr_t transactionReturnSlot{},transactionCamera{};
void ResetTransaction() {
    if(transactionCounted) {--outstandingTransactions;transactionCounted=false;}
    transactionReturnSlot=transactionCamera=0;
    transaction.Reset();
}
bool prepared{},requested{},installed{},failed{},restored{};
ChaseCameraConfig config;
ChaseCameraState behavior;
ReplayClock replayClock;
bool presentedReplay{};
uint64_t replayUpdates{},replayResets{};
const char* lastReplayRefusal="";
std::array<PoseOverlay,8> overlays{};
ChaseCameraInput pendingInput;
float inputDt{},lastStepDt{1.f/60};
std::chrono::steady_clock::time_point inputTime{};
uint64_t consumedFrame{},seenSuspension{},updated{},refused{},restores{},duplicate{};
uint64_t neutralLookCalls{};
std::atomic<uint64_t> lastEligible{};
uintptr_t lastCamera{},lastEntity{};
uintptr_t presentedSelection{},presentedEntity{};
uint64_t selectionEpoch{};
ChaseVector lastPosition{};
double lastReplayTime{};
bool havePosition{};
const char* reason="not_requested";
const char* lastRefusal="";
IDirectInput8W* directInput{};
IDirectInputDevice8W* mouse{};
HWND mouseWindow{};
HWND inputWindow{};
bool mouseWasAvailable{};
DWORD inputThread{};
using GetController=DWORD(WINAPI*)(DWORD,XINPUT_STATE*);
GetController getController{};
HMODULE xinput{};
DWORD controller=UINT32_MAX;
#ifdef GRID2VR_CHASE_CAMERA_TEST
bool testFocused{};
#endif

template<class T> bool Read(uintptr_t at,T& value) {return ReadMemory(reinterpret_cast<void*>(at),&value,sizeof(value));}
bool Reader(uintptr_t at,void* out,size_t bytes) {return ReadMemory(reinterpret_cast<void*>(at),out,bytes);}
bool WriteData(uintptr_t at,const void* value,size_t bytes) {
    // Caller bounds this to a pose or to the native helper's stack target xyz.
    // Never change page access.
    MEMORY_BASIC_INFORMATION region{};
    if(!VirtualQuery(reinterpret_cast<void*>(at),&region,sizeof(region)) || region.State!=MEM_COMMIT ||
       (region.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
       !(region.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) ||
       at+bytes<at || at+bytes>reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize)return false;
    __try { std::memcpy(reinterpret_cast<void*>(at),value,bytes);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool WritePose(uintptr_t at,const void* value,size_t bytes) {
    return bytes==sizeof(Pose) && WriteData(at,value,bytes);
}
bool Focused() {
#ifdef GRID2VR_CHASE_CAMERA_TEST
    return testFocused;
#else
    DWORD pid{};const auto foreground=GetForegroundWindow();
    return foreground && GetWindowThreadProcessId(foreground,&pid) && pid==GetCurrentProcessId() && windowState==5;
#endif
}
bool InputAvailable() {
    std::unique_lock lock(inputMutex,std::try_to_lock);
    return lock.owns_lock() && (pendingInput.mouseAvailable || pendingInput.stickAvailable);
}
void Suspend(const char* why) {behavior.Suspend();reason=lastRefusal=why;++refused;havePosition=false;}
const char* LocalTargetRefusal(const chaseobserver::PreQueryObservation& snapshot,ReplayPlayback* playback=nullptr);
void TrackSelection() {
    std::lock_guard lock(mutex);
    uint32_t root{},state{},owner{},manager{},selected{},provider{},entity{},alternate{};
    uint8_t useAlternate{};
    const auto pointer=[](uintptr_t at,uint32_t& value) {return Read(at,value) && chaseobserver::OwnerNativePointer(value);};
    if(!pointer(base+0xdef810,root) || !pointer(root+0x10,state) || !pointer(state+0x14fc,owner) ||
       !pointer(owner+0x33c,manager) || !pointer(manager+0x748,selected) || !Read(manager+0x759,useAlternate) || useAlternate>1)
        selected=0;
    if(selected && useAlternate) {
        if(!Read(selected+0x70,alternate) || (alternate && !chaseobserver::OwnerNativePointer(alternate)))selected=0;
        else if(alternate)selected=alternate;
    }
    if(selected && (!pointer(selected+0x60,provider) || !pointer(provider+8,entity)))entity=0;
    ReplayPlayback playback{};uint32_t car{};
    chaseobserver::PreQueryObservation target{};
    target.camera=selected;target.provider=provider;target.entity=entity;
    if(Read(provider+0xc,car))target.provider0c=car;
    if(const auto refusal=LocalTargetRefusal(target,&playback)) {
        ++suspensionEpoch;Suspend(refusal);replayClock.Leave();
    }
    const bool replay=playback.Ui()!=0;
    if(selected!=presentedSelection || entity!=presentedEntity || replay!=presentedReplay) {
        presentedSelection=selected;presentedEntity=entity;++selectionEpoch;behavior.Reset();havePosition=false;
        replayClock.Leave();presentedReplay=replay;
    }
}
void ReleaseMouse() {
    if(mouse){mouse->Unacquire();mouse->Release();mouse=nullptr;}
    mouseWindow=nullptr;
}
void PollInput() {
    if(!inputThread)inputThread=GetCurrentThreadId();
    if(inputThread!=GetCurrentThreadId()) {
        ++suspensionEpoch;std::lock_guard lock(inputMutex);pendingInput={};return;
    }
    ChaseCameraInput value{};
    if(!Focused()) {
        ReleaseMouse();inputWindow=nullptr;mouseWasAvailable=false;++suspensionEpoch;
        std::lock_guard lock(inputMutex);pendingInput={};return;
    }
    const auto foreground=GetForegroundWindow();
    if(inputWindow!=foreground) {ReleaseMouse();inputWindow=foreground;mouseWasAvailable=false;++suspensionEpoch;}
    if(!directInput)DirectInput8Create(GetModuleHandleW(nullptr),DIRECTINPUT_VERSION,IID_IDirectInput8W,
        reinterpret_cast<void**>(&directInput),nullptr);
    if(directInput && !mouse) {
        if(SUCCEEDED(directInput->CreateDevice(GUID_SysMouse,&mouse,nullptr))) {
            if(FAILED(mouse->SetDataFormat(&c_dfDIMouse2)) ||
               FAILED(mouse->SetCooperativeLevel(foreground,DISCL_FOREGROUND|DISCL_NONEXCLUSIVE)))ReleaseMouse();
            else mouseWindow=foreground;
        }
    }
    DIMOUSESTATE2 state{};
    if(mouse) {
        auto result=mouse->GetDeviceState(sizeof(state),&state);
        if(FAILED(result)) {
            if(mouseWasAvailable)++suspensionEpoch;
            if(SUCCEEDED(mouse->Acquire()))mouse->GetDeviceState(sizeof(state),&state);
            state={}; // Discard the acquisition click/deltas.
        } else {
            value.mouseAvailable=true;value.mouseX=static_cast<float>(state.lX);
            value.mouseY=static_cast<float>(state.lY);value.wheel=static_cast<float>(state.lZ);
        }
    }
    mouseWasAvailable=value.mouseAvailable;
    value.leftMouse=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;
    value.rightMouse=(GetAsyncKeyState(VK_RBUTTON)&0x8000)!=0;
    if(!xinput) {
        xinput=LoadLibraryExW(L"xinput1_4.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(xinput)getController=reinterpret_cast<GetController>(GetProcAddress(xinput,"XInputGetState"));
    }
    if(getController) {
        XINPUT_STATE pad{};const auto previous=controller;
        if(controller==UINT32_MAX || getController(controller,&pad)!=ERROR_SUCCESS) {
            controller=UINT32_MAX;
            for(DWORD slot=0;slot<4;++slot)if(getController(slot,&pad)==ERROR_SUCCESS) {controller=slot;break;}
        }
        if(previous!=controller)++suspensionEpoch;
        if(controller!=UINT32_MAX) {
            value.stickAvailable=true;
            value.stickX=pad.Gamepad.sThumbRX/32768.f;value.stickY=pad.Gamepad.sThumbRY/32768.f;
            value.rearHeld=(pad.Gamepad.wButtons&XINPUT_GAMEPAD_RIGHT_THUMB)!=0;
        }
    }
    std::lock_guard lock(inputMutex);
    const auto now=std::chrono::steady_clock::now();
    const auto elapsed=inputTime.time_since_epoch().count()?std::chrono::duration<float>(now-inputTime).count():1.f/60;
    inputTime=now;
    const bool fresh=frame.load()<=lastEligible+2;
    inputDt=(fresh?inputDt:0)+elapsed;
    if(fresh && value.mouseAvailable && pendingInput.mouseAvailable) {
        value.mouseX=std::clamp(value.mouseX+pendingInput.mouseX,-2000.f,2000.f);
        value.mouseY=std::clamp(value.mouseY+pendingInput.mouseY,-2000.f,2000.f);
        value.wheel=std::clamp(value.wheel+pendingInput.wheel,-1920.f,1920.f);
    }
    pendingInput=value;
}
PoseOverlay* FindOverlay(uintptr_t camera) {
    for(auto& entry:overlays)if(entry.camera==camera)return &entry;
    for(auto& entry:overlays)if(!entry.valid) {entry={};entry.camera=camera;return &entry;}
    return nullptr;
}
bool RestorePose(uintptr_t camera) {
    auto* entry=FindOverlay(camera);if(!entry)return false;
    if(!entry->valid)return true;
    uint32_t provider{},entity{};
    if(!Read(camera+0x60,provider) || !Read(provider+8,entity)) {entry->valid=false;return false;}
    const bool ok=entry->Restore(camera,provider,entity,Reader,WritePose);
    if(ok)++restores;return ok;
}
struct Evaluation {
    bool sole{};
    uint64_t token{};
    Evaluation() {
        std::lock_guard lock(mutex);
        const auto active=++evaluations;
        sole=active==1 && !engine::ReplayingEye();
        if(!sole) {++suspensionEpoch;++contention;}
        token=suspensionEpoch;
    }
    ~Evaluation(){std::lock_guard lock(mutex);--evaluations;}
    bool Fresh()const {return sole && evaluations==1 && token==suspensionEpoch;}
};
struct StageContext {
    uintptr_t camera{},timing{};bool eligible{},applied{};Pose native{};
    uintptr_t provider{},entity{};uint64_t token{},evaluation{};
    CollisionTargetTransform collision;
    uintptr_t caller{},returnSlot{};
    ReplayPlayback replay;
};
thread_local StageContext* stage{};

const char* LocalTargetRefusal(const chaseobserver::PreQueryObservation& snapshot,ReplayPlayback* playback) {
    if(playback)*playback={};
    uint32_t parent{},begin{},car{},entity{};
    if(const auto refusal=NativeDrivingReplayRefusal(base,Reader)) {
        ReplayPlayback captured{};
        if(const auto why=CaptureRecordedReplay(base,snapshot.camera,snapshot.provider,snapshot.entity,Reader,captured)) {
            lastReplayRefusal=why;return refusal;
        }
        // Replay ownership is established above, independently of this existing
        // local/AI-only restriction. Never infer an offline session from UI mode.
        if(!NormalLocalRaceReady(base)) {lastReplayRefusal="replay_local_race_not_ready";return lastReplayRefusal;}
        if(playback)*playback=captured;
        return nullptr;
    }
    if(!NormalLocalRaceReady(base))return "local_race_not_ready";
    if(!Read(base+0xd96808,parent) || !Read(parent+0xa8,begin) || !Read(begin,car))return "local_car_unreadable";
    if(car+0x12c!=snapshot.provider)return "local_provider_mismatch";
    if(car!=snapshot.provider0c)return "local_provider_car_mismatch";
    if(!Read(car+0x134,entity))return "local_entity_unreadable";
    return entity==snapshot.entity?nullptr:"local_entity_mismatch";
}
bool TransactionOwner(uintptr_t camera,TransactionIdentity& value) {
    uintptr_t manager{};uint32_t provider{},entity{},car{},table{};
    const bool draining=stopping && transactionCounted;
    if((!draining && (!enabled || stopping)) || engine::ReplayingEye() || !Focused() ||
       !SelectedOwner(base,camera,Reader,manager) || !Read(camera+0x60,provider) ||
       !Read(provider,table) || table!=base+chaseobserver::providerTableRva || !Read(provider+8,entity) || !Read(provider+0xc,car))return false;
    chaseobserver::PreQueryObservation target{};target.camera=camera;target.provider=provider;target.provider0c=car;target.entity=entity;
    ReplayPlayback playback{};
    if(LocalTargetRefusal(target,&playback) || !WorldRunning(base,Reader,playback.Ui()!=0))return false;
    value={camera,manager,provider,entity,GetCurrentThreadId(),nativeEvaluation.load(),suspensionEpoch.load()};
    value.replay=playback.clock;value.replayMode=playback.mode;value.replayDirector=playback.direction;
    return true;
}
bool OwnDisplayed(const TransactionIdentity& owner,PoseOverlay*& entry) {
    entry=FindOverlay(owner.camera);Pose actual{};
    return entry && entry->valid && entry->provider==owner.provider && entry->entity==owner.entity &&
        Reader(owner.camera+0xb0,actual.data(),sizeof(actual)) && !std::memcmp(actual.data(),entry->displayed.data(),sizeof(actual));
}
void Apply(void* provider,void* output,uintptr_t returned,uintptr_t caller) {
    std::lock_guard lock(mutex);
    if(!stage || !stage->eligible || stage->applied || caller!=base+0x1f78ed ||
       evaluations!=1 || stage->token!=suspensionEpoch || !enabled || stopping || engine::ReplayingEye())return;
    chaseobserver::PreQueryObservation snapshot{};
    const auto failure=chaseobserver::CapturePreQuery(base,stage->camera,stage->timing,reinterpret_cast<uintptr_t>(provider),
        reinterpret_cast<uintptr_t>(output),returned,Reader,snapshot);
    if(failure) {Suspend(failure);return;}
    uintptr_t manager{};
    if(!SelectedOwner(base,stage->camera,Reader,manager))return; // Other native cameras are updated too.
    ReplayPlayback playback{};
    if(const auto refusal=LocalTargetRefusal(snapshot,&playback)) {Suspend(refusal);return;}
    const bool replay=playback.Ui()!=0;
    if(!WorldRunning(base,Reader,replay)) {Suspend("native_pause_or_unavailable");return;}
    // Replay only uses the proven selected-camera stack transaction. Qualify
    // its real caller before any write, not merely when copying the result out.
    if(replay && (stage->caller!=base+transactionBeginCallerRva || stage->returnSlot%16!=8 ||
        stage->timing!=stage->returnSlot+transactionTimingOffset)) {Suspend("replay_camera_sequence");return;}
    uint8_t rear{},pendingRear{};
    const float nativeLookX=std::bit_cast<float>(snapshot.recordWords[0x174/4]);
    const float nativeLookY=std::bit_cast<float>(snapshot.recordWords[0x178/4]);
    if(!Read(stage->camera+0x2562,rear) || !Read(stage->camera+0x2563,pendingRear) || rear || pendingRear ||
       (snapshot.recordWords[0x1a4/4]&12) || !std::isfinite(nativeLookX) || !std::isfinite(nativeLookY) ||
       std::abs(nativeLookX)>.001f || std::abs(nativeLookY)>.001f) {Suspend("native_look_settling");return;}
    const float nativeDt=std::bit_cast<float>(snapshot.timingWords[0]);
    if(!Focused() || !std::isfinite(nativeDt) || std::abs(nativeDt)>.25f || (!replay && nativeDt<=0)) {Suspend("focus_or_inactive_timestep");return;}
    ChaseVector pivot{};
    if(!VehicleOrbitPivot(snapshot,pivot)) {Suspend("invalid_native_pivot");return;}
    Pose source{};std::memcpy(source.data(),snapshot.recordWords.data(),sizeof(source));
    if(seenSuspension!=suspensionEpoch) {behavior.Suspend();seenSuspension=suspensionEpoch;}
    const auto inputFrame=frame.load();
    ChaseCameraInput input{};
    float dt{};
    {
        std::unique_lock inputLock(inputMutex,std::try_to_lock);
        if(!inputLock.owns_lock()){Suspend("input_busy");return;}
        if(!pendingInput.mouseAvailable && !pendingInput.stickAvailable){Suspend("input_device_unavailable");return;}
        lastEligible=inputFrame;
        dt=consumedFrame==inputFrame?lastStepDt:inputDt;
        if(consumedFrame!=inputFrame) {
            input=pendingInput;pendingInput.mouseX=pendingInput.mouseY=pendingInput.wheel=0;
            consumedFrame=inputFrame;
            lastStepDt=dt;inputDt=0;
        }
    }
    if(!std::isfinite(dt) || dt<=0 || dt>.25f) {Suspend("input_clock_discontinuity");return;}
    ReplayClockResult playbackStep{};
    if(replay) {
        playbackStep=replayClock.Observe(playback.clock,inputFrame,dt);
        if(!playbackStep.valid) {Suspend("replay_clock_discontinuity");return;}
        if(playbackStep.reset) {++selectionEpoch;++replayResets;behavior.Reset();havePosition=false;}
    } else replayClock.Leave();
    ChaseVector position{std::bit_cast<float>(snapshot.entityPositionWords[0]),
        std::bit_cast<float>(snapshot.entityPositionWords[1]),std::bit_cast<float>(snapshot.entityPositionWords[2])};
    float speed{};
    if(havePosition && lastCamera==stage->camera && lastEntity==snapshot.entity) {
        const float dx=position.x-lastPosition.x,dy=position.y-lastPosition.y,dz=position.z-lastPosition.z;
        // Fast playback multiplies wall-time displacement. Use recording time
        // for return-to-centre speed; ReplayClock handles replay discontinuities.
        const double travelTime=replay?std::abs(playback.clock.time-lastReplayTime):dt;
        speed=travelTime>1e-6?static_cast<float>(std::sqrt(dx*dx+dy*dy+dz*dz)/travelTime*3.6):0;
        if(!std::isfinite(speed) || (!replay && speed>500)) {behavior.Reset();speed=0;}
        else if(replay)speed=std::min(speed,500.f);
    }
    ChaseCameraContext context{stage->camera,snapshot.entity,(selectionEpoch<<32)|snapshot.nameHash,inputFrame,dt,speed,true,true,true,replay,!replay || playbackStep.forward};
    const auto result=behavior.Step(config,context,input);
    if(result==ChaseCameraStep::Duplicate)++duplicate;
    else {lastPosition=position;lastCamera=stage->camera;lastEntity=snapshot.entity;lastReplayTime=playback.clock.time;havePosition=true;}
    if(!behavior.active) {reason="awaiting_neutral_input";return;}
    Pose desired=source;
    ChaseQuaternion rotation;
    if(!OrbitChasePose(desired,pivot,{0,1,0},{1,0,0},behavior.orbit.yaw,behavior.orbit.pitch,behavior.zoom.scale,&rotation)) {
        Suspend("invalid_native_pose");return;
    }
    if(!std::memcmp(source.data(),desired.data(),sizeof(source))) {reason="controls_ready";return;}
    Pose check{};uintptr_t managerAgain{};uint32_t providerAgain{},entityAgain{},nameAgain{};ReplayPlayback playbackAgain{};
    if(evaluations!=1 || stage->token!=suspensionEpoch || !enabled || stopping || !SelectedOwner(base,stage->camera,Reader,managerAgain) ||
       managerAgain!=manager || LocalTargetRefusal(snapshot,&playbackAgain) || playbackAgain!=playback ||
       !WorldRunning(base,Reader,replay) || !Reader(snapshot.record,check.data(),sizeof(check)) || std::memcmp(check.data(),source.data(),sizeof(source)) ||
       !Read(stage->camera+0x60,providerAgain) || providerAgain!=snapshot.provider ||
       !Read(providerAgain+8,entityAgain) || entityAgain!=snapshot.entity ||
       !Read(stage->camera+0x98,nameAgain) || nameAgain!=snapshot.nameHash ||
       !FindOverlay(stage->camera)) {Suspend("source_changed");return;}
    if(!WritePose(snapshot.record,desired.data(),sizeof(desired))) {Suspend("pose_write_failed");return;}
    stage->native=source;stage->provider=snapshot.provider;stage->entity=snapshot.entity;
    stage->collision={pivot,rotation,behavior.zoom.scale};
    stage->replay=playback;
    stage->applied=true;++updated;
    if(replay) {++replayUpdates;reason="replay_camera_controls_applied";} else reason="driving_camera_controls_applied";
}
uintptr_t ForwardCollisionTarget(void* self,void* output,uint32_t adjusted,uintptr_t caller) {
    // The original calls our provider getter. Do not hold mutex across it.
    const auto result=originalTarget(self,output,adjusted);
    std::lock_guard lock(mutex);
    if(!stage || !stage->applied || stage->camera!=reinterpret_cast<uintptr_t>(self) ||
       caller!=base+collisionTargetReturnRva || adjusted!=1 || result!=reinterpret_cast<uintptr_t>(output) ||
       evaluations!=1 || stage->token!=suspensionEpoch || !enabled || stopping || engine::ReplayingEye())return result;
    const auto address=reinterpret_cast<uintptr_t>(output);
    ULONG_PTR stackLow{},stackHigh{};GetCurrentThreadStackLimits(&stackLow,&stackHigh);
    uintptr_t manager{};uint32_t provider{},entity{};
    std::array<float,4> target{};
    chaseobserver::PreQueryObservation owner{};owner.camera=stage->camera;owner.provider=stage->provider;owner.entity=stage->entity;
    Read(stage->provider+0xc,owner.provider0c);
    ReplayPlayback playback{};
    if(address%16 || address<stackLow || address>stackHigh || stackHigh-address<64 ||
       !SelectedOwner(base,stage->camera,Reader,manager) || !Read(stage->camera+0x60,provider) || provider!=stage->provider ||
       !Read(provider+8,entity) || entity!=stage->entity || !Focused() || LocalTargetRefusal(owner,&playback) ||
       playback!=stage->replay || !WorldRunning(base,Reader,playback.Ui()!=0) ||
       !Reader(address+0x30,target.data(),sizeof(target)) || !stage->collision.Apply(target) ||
       !WriteData(address+0x30,target.data(),3*sizeof(float))) {
        Suspend("collision_target_refused");return result;
    }
    ++collisionTargets;return result;
}
__declspec(noinline) uintptr_t __fastcall CollisionTarget(void* self,void*,void* output,uint32_t adjusted) {
    return ForwardCollisionTarget(self,output,adjusted,reinterpret_cast<uintptr_t>(_ReturnAddress()));
}
uintptr_t ForwardProvider(void* self,void* output,uintptr_t caller) {
    const auto result=reinterpret_cast<Native>(original[4])(self,output);
    Apply(self,output,result,caller);return result;
}
__declspec(noinline) uintptr_t __fastcall Provider(void* self,void*,void* output) {
    return ForwardProvider(self,output,reinterpret_cast<uintptr_t>(_ReturnAddress()));
}
uintptr_t __fastcall Update(void* self,void*,void* timing) {
    Evaluation scope;
    {std::lock_guard lock(mutex);++nativeEvaluation;ResetTransaction();if(scope.Fresh() && !RestorePose(reinterpret_cast<uintptr_t>(self)))Suspend("restore_failed");}
    return reinterpret_cast<Native>(original[0])(self,timing);
}
uintptr_t __fastcall UpdateAlternate(void* self,void*,void* timing,uint32_t flag) {
    Evaluation scope;
    {std::lock_guard lock(mutex);++nativeEvaluation;ResetTransaction();if(scope.Fresh() && !RestorePose(reinterpret_cast<uintptr_t>(self)))Suspend("restore_failed");}
    return reinterpret_cast<Alternate>(original[1])(self,timing,flag);
}
uintptr_t ForwardGetter(void* self,uintptr_t) {
    // No getter hook:59a100 must cache the prior displayed position so its
    // native camera-region pass measures actual displayed movement. Slot28
    // restores the solver baseline after that cache, before the native solver.
    return reinterpret_cast<Getter>(original[3])(self);
}
uintptr_t ForwardObstruction(void* self,void* timing,uintptr_t caller,uintptr_t returnSlot) {
    Evaluation scope;
    StageContext context{reinterpret_cast<uintptr_t>(self),reinterpret_cast<uintptr_t>(timing),scope.sole && !stage};
    context.token=scope.token;
    context.caller=caller;context.returnSlot=returnSlot;
    const auto previous=stage;
    {std::lock_guard lock(mutex);
        context.evaluation=++nativeEvaluation;ResetTransaction();
        context.eligible=context.eligible && scope.Fresh();
        if(context.eligible && !RestorePose(context.camera)){context.eligible=false;Suspend("restore_failed");}}
    stage=&context;
    const auto result=reinterpret_cast<Native>(original[2])(self,timing);
    stage=previous;
    // Save the final native obstruction result solely to recognize our overlay
    // at the next update. Never overwrite the native obstruction/visibility pass.
    if(context.applied) {
        std::lock_guard lock(mutex);
        auto* entry=FindOverlay(context.camera);uint32_t provider{},entity{};
        if(entry && context.token==suspensionEpoch && evaluations==1 &&
           Read(context.camera+0x60,provider) && provider==context.provider && Read(provider+8,entity) && entity==context.entity &&
           Reader(context.camera+0xb0,entry->displayed.data(),sizeof(Pose))) {
            entry->camera=context.camera;entry->provider=provider;entry->entity=entity;
            entry->native=context.native;entry->valid=true;
            TransactionIdentity identity{};
            if(caller==base+transactionBeginCallerRva && context.evaluation==nativeEvaluation && TransactionOwner(context.camera,identity) &&
               transaction.Begin(identity,caller-base,returnSlot,context.timing)) {
                transactionCounted=true;transactionReturnSlot=returnSlot;transactionCamera=context.camera;++outstandingTransactions;
            }
        } else {enabled=false;failed=true;Suspend("post_stage_unreadable");}
    }
    return result;
}
__declspec(noinline) uintptr_t __fastcall Obstruction(void* self,void*,void* timing) {
    return ForwardObstruction(self,timing,reinterpret_cast<uintptr_t>(_ReturnAddress()),reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()));
}
uintptr_t ForwardCopy(void* self,void* output,uintptr_t caller,uintptr_t returnSlot) {
    Evaluation scope;
    bool preserve{};TransactionIdentity identity{};PoseOverlay* entry{};
    {
        std::lock_guard lock(mutex);
        const bool pending=transactionCounted;
        preserve=scope.Fresh() && TransactionOwner(reinterpret_cast<uintptr_t>(self),identity) && OwnDisplayed(identity,entry) &&
            transaction.Copy(identity,caller-base,returnSlot,reinterpret_cast<uintptr_t>(output));
        if(!preserve) {
            transaction.Reset(); // Retain the native frame lifetime until slot25.
            if(pending) {enabled=false;failed=true;Suspend("camera_copy_sequence_changed");}
            if(scope.Fresh() && !RestorePose(reinterpret_cast<uintptr_t>(self)))Suspend("restore_failed");
        }
    }
    const auto result=reinterpret_cast<Native>(original[5])(self,output);
    if(preserve) {
        std::lock_guard lock(mutex);TransactionIdentity again{};Pose copied{};
        if(!scope.Fresh() || !TransactionOwner(identity.camera,again) || again!=identity || !OwnDisplayed(identity,entry) ||
           !Reader(reinterpret_cast<uintptr_t>(output),copied.data(),sizeof(copied)) || std::memcmp(copied.data(),entry->displayed.data(),sizeof(copied))) {
            transaction.Reset();enabled=false;failed=true;Suspend("camera_copy_changed");
        }
    }
    return result;
}
__declspec(noinline) uintptr_t __fastcall CopyRecord(void* self,void*,void* output) {
    return ForwardCopy(self,output,reinterpret_cast<uintptr_t>(_ReturnAddress()),reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()));
}
uintptr_t ForwardCopyBack(void* self,void* input,uintptr_t caller,uintptr_t returnSlot) {
    Evaluation scope;TransactionIdentity identity{};PoseOverlay* entry{};bool pending{},qualified{};
    {
        std::lock_guard lock(mutex);pending=transactionCounted;
        qualified=pending && transaction.Active() && scope.Fresh() && TransactionOwner(reinterpret_cast<uintptr_t>(self),identity) &&
            identity==transaction.Identity() && OwnDisplayed(identity,entry);
    }
    const auto result=reinterpret_cast<Native>(original[7])(self,input);
    if(pending) {
        std::lock_guard lock(mutex);TransactionIdentity again{};Pose displayed{},copied{};
        if(qualified && scope.Fresh() && TransactionOwner(identity.camera,again) && again==identity &&
           transaction.Commit(identity,caller-base,returnSlot,reinterpret_cast<uintptr_t>(input)) &&
           Reader(identity.camera+0xb0,displayed.data(),sizeof(displayed)) && Reader(reinterpret_cast<uintptr_t>(input),copied.data(),sizeof(copied)) &&
           !std::memcmp(displayed.data(),copied.data(),sizeof(copied))) {
            entry->displayed=displayed; // Native slot9/postprocessing remains intact.
            ResetTransaction();
        } else {
            // A failed ownership/sequence check still cannot end the enclosing
            // native frame early. Only its actual slot25 boundary can drain it.
            if(caller==base+transactionCommitCallerRva && returnSlot==transactionReturnSlot && reinterpret_cast<uintptr_t>(self)==transactionCamera)ResetTransaction();
            else transaction.Reset();
            enabled=false;failed=true;Suspend("camera_copyback_changed");
        }
    }
    return result;
}
__declspec(noinline) uintptr_t __fastcall CopyBack(void* self,void*,void* input) {
    return ForwardCopyBack(self,input,reinterpret_cast<uintptr_t>(_ReturnAddress()),reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()));
}
uintptr_t __fastcall Look(void* self,void*,void* packet) {
    Evaluation scope;LookPacket copy{};bool neutral{};
    {
        std::lock_guard lock(mutex);
        const auto camera=reinterpret_cast<uintptr_t>(self);
        uintptr_t manager{};uint32_t provider{},entity{},car{},flags{},providerTable{},cameraOwner{};
        uint8_t inactive{};
        std::array<char,32> name{};LookPacket input{},again{};
        if(scope.Fresh() && enabled && !stopping && Focused() && InputAvailable() && SelectedOwner(base,camera,Reader,manager) &&
           Read(camera+0x60,provider) && Read(provider,providerTable) && providerTable==base+chaseobserver::providerTableRva &&
           Read(provider+8,entity) && Read(provider+0xc,car) &&
           Read(camera+0xc,cameraOwner) && cameraOwner && Read(camera+0xa4,inactive) && !inactive &&
           Read(camera+0xb0+0x1a4,flags) && !(flags&6) &&
           Read(camera+0x19,name) && (!std::memcmp(name.data(),"chase_close",12) || !std::memcmp(name.data(),"chase_far",10)) &&
           Read(reinterpret_cast<uintptr_t>(packet),input) && Read(reinterpret_cast<uintptr_t>(packet),again) && input==again) {
            chaseobserver::PreQueryObservation target{};target.camera=camera;target.provider=provider;target.provider0c=car;target.entity=entity;
            ReplayPlayback playback{};
            if(const auto refusal=LocalTargetRefusal(target,&playback)) {++suspensionEpoch;Suspend(refusal);}
            else if(WorldRunning(base,Reader,playback.Ui()!=0))neutral=NeutralLookPacket(base,input,copy);
            if(neutral)++neutralLookCalls;
        }
    }
    // A private packet is consumed synchronously. Preserve type/vtable/flag and
    // forward once; other cameras receive the unchanged shared native packet.
    return reinterpret_cast<Native>(original[6])(self,neutral?copy.data():packet);
}
const std::array<void*,8> replacements{reinterpret_cast<void*>(Update),reinterpret_cast<void*>(UpdateAlternate),
    reinterpret_cast<void*>(Obstruction),nullptr,reinterpret_cast<void*>(Provider),reinterpret_cast<void*>(CopyRecord),reinterpret_cast<void*>(Look),reinterpret_cast<void*>(CopyBack)};
bool RestoreHooks(size_t count) {
    bool ok=true;
    if(targetCallInstalled) {
        const drivingcontrols::CallPatch patch{reinterpret_cast<uint8_t*>(base+collisionTargetCallRva),
            reinterpret_cast<void*>(CollisionTarget),reinterpret_cast<void*>(base+collisionTargetRoutineRva)};
        if(drivingcontrols::PatchCalls({&patch,1}))ok=false;
        else targetCallInstalled=false;
    } else {
        const drivingcontrols::CallPatch patch{reinterpret_cast<uint8_t*>(base+collisionTargetCallRva),
            reinterpret_cast<void*>(base+collisionTargetRoutineRva),reinterpret_cast<void*>(CollisionTarget)};
        if(!drivingcontrols::CallBytesMatch(patch))ok=false;
    }
    for(size_t patch=0;patch<count;++patch) {
        const auto i=hookIndexes[patch];
        auto** slot=reinterpret_cast<void**>(base+ControlSlot(i));void* value{};
        const auto expected=reinterpret_cast<void*>(base+ControlRoutine(i));
        if(!ReadMemory(slot,&value,sizeof(value)) || (value!=expected && !SwapGuardedPointer(slot,replacements[i],expected)))ok=false;
    }
    hooksNeedCleanup=!ok;
    return ok;
}
struct Receipt {std::string text;uint64_t revision{};};
uint64_t receiptRevision{};
Receipt StatusLocked() {
    if(!requested)return {};
    if(failed && !*lastRefusal)lastRefusal=reason;
    std::ostringstream out;
    const auto settings=[&] {
        out<<"{\"enabled\":"<<config.enabled<<",\"keepAngle\":"<<config.keepAngle
           <<",\"mouseMode\":"<<static_cast<int>(config.mouseMode)<<",\"maxZoomOutPercent\":"<<config.maxZoomOutPercent
           <<",\"mouseSensitivityPercent\":"<<config.mouseSensitivityPercent<<",\"stickSensitivityPercent\":"<<config.stickSensitivityPercent
           <<",\"invertVertical\":"<<config.invertVertical<<"}";
    };
    out<<std::boolalpha<<"{\"schema\":1,\"requested\":";settings();
    out<<",\"applied\":";if(updated)settings();else out<<"null";
    out<<",\"installed\":"<<installed<<",\"failed\":"<<failed
       <<",\"stopped\":"<<stopping.load()<<",\"hooksRestored\":"<<restored<<",\"reason\":\""<<reason
       <<"\",\"lastRefusal\":\""<<lastRefusal<<"\",\"appliedUpdates\":"<<updated<<",\"refusals\":"<<refused<<",\"restores\":"<<restores<<",\"duplicates\":"<<duplicate
       <<",\"outstandingTransactions\":"<<outstandingTransactions.load()<<",\"poseCleanupDeferred\":"<<stopPending.load()
       <<",\"contention\":"<<contention.load()<<",\"neutralLookCalls\":"<<neutralLookCalls<<",\"collisionTargets\":"<<collisionTargets
       <<",\"collisionCallInstalled\":"<<targetCallInstalled<<",\"controllerImplemented\":true,\"recordedReplayEnabled\":true"
       <<",\"replayUpdates\":"<<replayUpdates<<",\"replayResets\":"<<replayResets<<",\"lastReplayRefusal\":\""<<lastReplayRefusal<<"\""
       <<",\"desktopAccepted\":false,\"headsetAccepted\":false}\n";
    return {out.str(),++receiptRevision};
}
void WriteStatus(const Receipt& receipt) {
    if(receipt.text.empty())return;
    static std::mutex writer;std::lock_guard lock(writer);
    static uint64_t written{};if(receipt.revision<=written)return;
    std::ofstream out(folder/L"chase-camera.json",std::ios::trunc);out<<receipt.text;out.flush();
    if(out)written=receipt.revision;
    if(!out) {std::lock_guard stateLock(mutex);enabled=false;failed=true;reason=lastRefusal="receipt_write_failed";}
}
}
void Prepare(uintptr_t image,const std::filesystem::path& output) {
    std::unique_lock lock(mutex);if(prepared)return;prepared=true;base=image;folder=output;
    std::error_code error;const auto path=output/L"chase-camera.request";
    if(!std::filesystem::exists(path,error))return;
    requested=true;std::array<char,81> bytes{};std::ifstream file(path,std::ios::binary);
    file.read(bytes.data(),bytes.size());
    if(!ParseChaseCameraRequest({bytes.data(),static_cast<size_t>(file.gcount())},config)) {failed=true;reason="invalid_request";}
    else if(config.enabled && std::filesystem::exists(output/L"chase-camera-observer.request")) {failed=true;reason="observer_conflict";}
    else reason=config.enabled?"waiting_for_local_driving":"disabled";
    const auto receipt=StatusLocked();lock.unlock();WriteStatus(receipt);
}
void Install() {
    std::unique_lock lock(mutex);if(!requested || !config.enabled || failed || installed || stopping)return;
    if(!chaseobserver::GuardsMatch(base) || !AdapterGuardsMatch(base) || !LookGuardsMatch(base) || !PauseGuardsMatch(base) ||
       !RegionGuardsMatch(base) || !ReplayStateGuardsMatch(base,Reader) || !TransactionGuardsMatch(base,Reader) ||
       !CollisionGuardsMatch(base) || !RecordedReplayGuardsMatch(base)) {failed=true;reason="instruction_or_vtable_guard";const auto receipt=StatusLocked();lock.unlock();WriteStatus(receipt);return;}
    for(size_t i=0;i<routes.size();++i)original[i]=reinterpret_cast<void*>(base+ControlRoutine(i));
    for(size_t patch=0;patch<hookIndexes.size();++patch) {
        const auto i=hookIndexes[patch];
        if(!SwapGuardedPointer(reinterpret_cast<void**>(base+ControlSlot(i)),original[i],replacements[i])) {
            restored=RestoreHooks(patch);failed=true;reason=restored?"install_failed":"rollback_failed";const auto receipt=StatusLocked();lock.unlock();WriteStatus(receipt);return;
        }
        hooksNeedCleanup=true;
    }
    originalTarget=reinterpret_cast<TargetHelper>(base+collisionTargetRoutineRva);
    const drivingcontrols::CallPatch targetPatch{reinterpret_cast<uint8_t*>(base+collisionTargetCallRva),
        reinterpret_cast<void*>(originalTarget),reinterpret_cast<void*>(CollisionTarget)};
    if(const auto failure=drivingcontrols::PatchCalls({&targetPatch,1})) {
        targetCallInstalled=drivingcontrols::CallBytesMatch(targetPatch,true);
        restored=RestoreHooks(hookIndexes.size());
        restored=drivingcontrols::CallBytesMatch(targetPatch) && restored;
        failed=true;reason=restored?failure:"rollback_failed";
        const auto receipt=StatusLocked();lock.unlock();WriteStatus(receipt);return;
    }
    targetCallInstalled=true;
    installed=true;enabled=true;reason="waiting_for_selected_chase";const auto receipt=StatusLocked();lock.unlock();WriteStatus(receipt);
}
void Present(uint64_t value,unsigned state) {
    frame=value;windowState=state;
    if(stopPending)Stop();
    if(enabled && !stopping) {TrackSelection();PollInput();}
    if(value%120)return;
    std::unique_lock lock(mutex,std::try_to_lock);
    if(lock.owns_lock()) {const auto receipt=StatusLocked();lock.unlock();WriteStatus(receipt);}
}
const char* StopForExit() {
    stopping=true;enabled=false;stopPending=false;
    std::unique_lock lock(mutex,std::defer_lock);
    if(!lock.try_lock_for(std::chrono::milliseconds(250)))return "chase_controls_busy";
    const bool hadHooks=installed || hooksNeedCleanup;
    if(hadHooks)restored=RestoreHooks(hookIndexes.size());
    reason=hadHooks?(restored?"controls_stopped":"pointer_restore_failed"):"controls_not_installed";
    if(hadHooks && !restored)failed=true;
    const auto result=reason;const auto receipt=StatusLocked();lock.unlock();WriteStatus(receipt);return result;
}
void Stop() {
    stopping=true;enabled=false;
    {
        std::unique_lock lock(mutex);
        if(evaluations!=0 || outstandingTransactions!=0) {
            stopPending=true;reason="waiting_for_native_camera_stop";
        } else {
            for(auto& overlay:overlays)if(overlay.valid)RestorePose(overlay.camera);
            stopPending=false;
            const bool hadHooks=installed || hooksNeedCleanup;
            if(hadHooks)restored=RestoreHooks(hookIndexes.size());
            reason=hadHooks?(restored?"controls_stopped":"pointer_restore_failed"):"controls_not_installed";
            if(hadHooks && !restored)failed=true;
        }
        const auto receipt=StatusLocked();lock.unlock();WriteStatus(receipt);
    }
    if(inputThread==GetCurrentThreadId()) {
        ReleaseMouse();if(directInput){directInput->Release();directInput=nullptr;}
        if(xinput){FreeLibrary(xinput);xinput=nullptr;getController=nullptr;}
    }
}
#ifdef GRID2VR_CHASE_CAMERA_TEST
void TestOriginals(void* update,void* alternate,void* obstruction,void* getter,void* provider,void* copy,void* look,void* copyBack) {
    original={update,alternate,obstruction,getter,provider,copy?copy:original[5],look?look:original[6],copyBack?copyBack:original[7]};enabled=true;
}
void TestInput(uint64_t value,const ChaseCameraInput& input,bool focused,float dt) {
    frame=value;testFocused=focused;windowState=focused?5:4;
    TrackSelection();
    std::lock_guard lock(inputMutex);pendingInput=input;inputDt=dt;
}
uintptr_t TestCall(unsigned route,void* camera,void* argument,uint32_t flag) {
    if(route==1)return UpdateAlternate(camera,nullptr,argument,flag);
    if(route==2)return Obstruction(camera,nullptr,argument);
    if(route==3)return ForwardGetter(camera,base+0x59a16d);
    if(route==5)return CopyRecord(camera,nullptr,argument);
    if(route==6)return Look(camera,nullptr,argument);
    return Update(camera,nullptr,argument);
}
uintptr_t TestProvider(void* provider,void* output,uintptr_t callerRva) {return ForwardProvider(provider,output,base+callerRva);}
uintptr_t TestGetter(void* camera,uintptr_t callerRva) {return ForwardGetter(camera,base+callerRva);}
void TestTargetOriginal(void* target) {originalTarget=reinterpret_cast<TargetHelper>(target);}
uintptr_t TestCollisionTarget(void* camera,void* output,uint32_t adjusted,uintptr_t callerRva) {
    return ForwardCollisionTarget(camera,output,adjusted,base+callerRva);
}
uintptr_t TestTransaction(unsigned route,void* camera,void* argument,uintptr_t callerRva,uintptr_t returnSlot) {
    if(route==2)return ForwardObstruction(camera,argument,base+callerRva,returnSlot);
    if(route==5)return ForwardCopy(camera,argument,base+callerRva,returnSlot);
    return ForwardCopyBack(camera,argument,base+callerRva,returnSlot);
}
#endif

}
