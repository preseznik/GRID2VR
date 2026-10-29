#include "opponent_lod_world_policy.h"
#include "replay_sequence.h"
#pragma warning(push)
#pragma warning(disable:4456) // Existing scene_pose.h nested-local names, not this fixture.
#include "scene_views.h"
#pragma warning(pop)
#include "replay_presentation.h"
#include "garage_presentation.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>

#define CHECK(value) do {if(!(value)){std::fprintf(stderr,"host gate failure line %d: %s\n",__LINE__,#value);std::exit(1);}}while(false)

// These are ONLY runtime/XR execution-boundary substitutes. The extracted host
// and engine predicates/lifecycle functions below are production source. No game
// instructions, D3D device, window or OpenXR runtime is loaded by this fixture.
namespace grid2vr::opponentlod::runtime {
bool fixtureDiagnosticComplete{};
bool fixtureControlledDormant{};
bool DiagnosticComplete() noexcept {return fixtureDiagnosticComplete;}
bool WorldChecksNeeded() noexcept {return !fixtureDiagnosticComplete && !fixtureControlledDormant;}
std::atomic<bool> fixtureWorld{},fixtureReady{};
unsigned ticks{},revokes{};
void (*duringTick)(){};
void RevokeWorld() noexcept {fixtureWorld=false;fixtureReady=false;++revokes;}
void Tick(bool ready) noexcept {
    ++ticks;
    if(duringTick) {const auto callback=duringTick;duringTick=nullptr;callback();}
    fixtureWorld=ready;fixtureReady=ready;
}
bool SelectionWitness() {return fixtureWorld.load() && fixtureReady.load();}
}

namespace grid2vr::engine {
uintptr_t gameBase=0x400000,mainObject=0x2000000,rendererObject=0x2100000,visibilityConsumer=0x2200000;
void** mainShadow=reinterpret_cast<void**>(0x3000000);
void** rendererShadow=reinterpret_cast<void**>(0x3100000);
void** visibilityShadow=reinterpret_cast<void**>(0x3200000);
std::atomic<bool> installed{true},sessionFailureRequested{},visibilityHealthy{true};
std::atomic<uint64_t> frame{},visibilityConsumerFrame{},opponentVisibilityFrame{},visibilityReadyFrame{};
DWORD renderThread{};
bool runtimeSession=true,replayEnabled=true,inReplay{},eyeHistoryPrimed{},menuScene{},replaySceneBlocked{},garageSceneBlocked{};
unsigned replayPass{};
uint64_t menuSceneFrame{};
ReplaySequence replaySequence;
SceneViewFrame frozenViews;
struct Camera {uint64_t frame{};uintptr_t manager{};bool valid{};};
Camera currentEyeCamera,previousEyeCamera,renderedEyeHistories;
struct {bool active{};uint64_t frame{};} pendingVisibility;
struct {bool valid{};} pendingScene;
std::map<uintptr_t,uint8_t> memory;
unsigned nativeReads{};
template<class T> void Put(uintptr_t at,T value) {
    const auto* bytes=reinterpret_cast<const uint8_t*>(&value);
    for(size_t i=0;i<sizeof(T);++i)memory[at+i]=bytes[i];
}
bool ReadMemory(const void* address,void* value,size_t size) {
    ++nativeReads;
    const auto at=reinterpret_cast<uintptr_t>(address);
    for(size_t i=0;i<size;++i)if(!memory.count(at+i))return false;
    for(size_t i=0;i<size;++i)static_cast<uint8_t*>(value)[i]=memory.at(at+i);
    return true;
}
template<class T> T Read(uintptr_t at) {T value{};ReadMemory(reinterpret_cast<void*>(at),&value,sizeof(value));return value;}
bool ResetHistory() {return true;}
bool (*resetHistory)()=ResetHistory;
void (*observePair)(uint64_t,uint64_t,bool){};
bool ReadyForCapture() {return true;}
bool RestoreVisibility(const char*) {pendingVisibility.active=false;return true;}
void Log(const char*,...) {}
void WriteSessionStatus() {}
bool Continuous();bool SessionRendering();bool SessionFailed();void FailSession();
#define GRID2VR_HOST_GATE_ENGINE
#include "opponent_lod_host_gate.inc"
#undef GRID2VR_HOST_GATE_ENGINE

void Setup() {
    memory.clear();renderThread=GetCurrentThreadId();installed=true;sessionFailureRequested=false;visibilityHealthy=true;
    runtimeSession=replayEnabled=eyeHistoryPrimed=true;inReplay=menuScene=replaySceneBlocked=garageSceneBlocked=false;
    replayPass=0;pendingVisibility={};pendingScene={};frozenViews={};replaySequence={};
    CHECK(replaySequence.ConfigureSession());CHECK(replaySequence.Resume());
    currentEyeCamera={};previousEyeCamera={};renderedEyeHistories={};visibilityReadyFrame=50;
    frame=50;visibilityConsumerFrame=0;opponentVisibilityFrame=0;menuSceneFrame=0;
    Put(mainObject,mainShadow);Put(rendererObject,rendererShadow);Put(visibilityConsumer,visibilityShadow);
    Put(mainObject+0x28,rendererObject);Put(mainObject+0x30,visibilityConsumer);
    constexpr uintptr_t cameraManager=0x2300000;
    Put(mainObject+0x2c,cameraManager);Put(cameraManager,gameBase+0xcf8108);Put(cameraManager+0xa0,uint32_t(0));
    Put(mainObject+0x138,uint8_t(1));Put(mainObject+0x51,uint8_t(1));
    Put(gameBase+0xe61010,uintptr_t(0)); // Proven inactive recorded-playback owner.
    // Actual ReadGarageMenu pointer/table validation, with an empty active list.
    constexpr uintptr_t app=0x2400000,frontend=0x2500000,wrapper=0x2600000,manager=0x2700000,
        registry=0x2800000,list=0x2900000;
    Put(gameBase+0xdef804,app);Put(app,gameBase+0xce96b8);Put(app+0x1428,frontend);
    Put(frontend,gameBase+0xccaf98);Put(frontend+0x40,wrapper);Put(wrapper,gameBase+0xd53e64);
    Put(wrapper+0x10,manager);Put(manager,gameBase+0xd50e0c);Put(manager+8,registry);
    Put(manager+0x1c,list);Put(registry,gameBase+0xd51700);Put(list+0x14,uint32_t(4));Put(list+0x18,uint32_t(0));
}
void AcceptedCenter(uint64_t sourceFrame) {
    frame=sourceFrame;CHECK(replaySequence.Begin(sourceFrame));replaySequence.Finish(true);
    currentEyeCamera={sourceFrame,0x2300000,true};menuSceneFrame=sourceFrame;
    visibilityConsumerFrame=sourceFrame;opponentVisibilityFrame=sourceFrame;frame=sourceFrame+1;
}
}

namespace grid2vr::hostfixture {
bool runtimeSession=true,runtimeStopped{},recordedRequested{},xrRequested=true,xrProjection=true,xrStopped{};
struct XrBoundary {
    bool running=true,failed{},focused=true,views=true,stopOnPoll{},failOnPoll{},unfocusOnPoll{};
    unsigned calls{};
    bool Running() {++calls;return running;}
    bool Failed() {++calls;return failed;}
    bool ReadyForViews(bool requireFocus) {++calls;return running && !failed && views && (!requireFocus || focused);}
    void Poll() {++calls;if(stopOnPoll)running=false;if(failOnPoll)failed=true;if(unfocusOnPoll)focused=false;}
    void Shutdown() {++calls;running=false;}
} xrSession;
#define GRID2VR_HOST_GATE_DIAGNOSTICS
#include "opponent_lod_host_gate.inc"
#undef GRID2VR_HOST_GATE_DIAGNOSTICS
void Setup() {
    engine::Setup();xrSession={};runtimeSession=xrRequested=xrProjection=true;
    runtimeStopped=recordedRequested=xrStopped=false;opponentlod::runtime::duringTick=nullptr;
    opponentlod::runtime::fixtureDiagnosticComplete=false;
    opponentlod::runtime::fixtureControlledDormant=false;
    opponentlod::host::RevokeWorld();engine::AcceptedCenter(50);
}
}

void PolicyCases() {
    using namespace grid2vr::opponentlod::host;
    CenterWorld state{};state.presentFrame=52;state.cameraFrame=state.consumerFrame=state.pairFrame=state.sceneFrame=51;
    state.controller=state.installed=state.rendering=state.cameraValid=state.objectsOwned=state.worldActive=
        state.visibilityEnabled=state.visibilityHealthy=state.normalPhase=true;
    CHECK(Eligible(state));
    for(auto field:{&CenterWorld::controller,&CenterWorld::installed,&CenterWorld::rendering,&CenterWorld::cameraValid,
        &CenterWorld::objectsOwned,&CenterWorld::worldActive,&CenterWorld::visibilityEnabled,
        &CenterWorld::visibilityHealthy,&CenterWorld::normalPhase}) {
        auto bad=state;bad.*field=false;CHECK(!Eligible(bad));
    }
    for(auto field:{&CenterWorld::failed,&CenterWorld::replaying}) {auto bad=state;bad.*field=true;CHECK(!Eligible(bad));}
    for(auto field:{&CenterWorld::cameraFrame,&CenterWorld::consumerFrame,&CenterWorld::pairFrame,&CenterWorld::sceneFrame}) {
        auto bad=state;--(bad.*field);CHECK(!Eligible(bad));
    }
    state.presentFrame=0;CHECK(!Eligible(state));
}

int main() {
    namespace e=grid2vr::engine;namespace f=grid2vr::hostfixture;
    namespace rt=grid2vr::opponentlod::runtime;namespace host=grid2vr::opponentlod::host;
    PolicyCases();f::Setup();
    // Four consecutive accepted native centers/pairs. The startup marker NEVER
    // advances, reproducing the old integration failure after the first frame.
    const auto epoch=e::replaySequence.Epoch();
    for(uint64_t sourceFrame=50;sourceFrame<54;++sourceFrame) {
        if(sourceFrame!=50)e::AcceptedCenter(sourceFrame);
        CHECK(e::NativeWorldReady()==(sourceFrame==50));
        CHECK(e::OpponentLodWorldReady());f::PollOpponentXr();f::PublishOpponentWorld();
        CHECK(rt::SelectionWitness());CHECK(e::frame==sourceFrame+1 && e::replaySequence.LastFrame()==sourceFrame);
        CHECK(e::visibilityReadyFrame==50 && e::replaySequence.Epoch()==epoch);
    }
    rt::fixtureDiagnosticComplete=true;
    const auto completedReads=e::nativeReads,completedXrCalls=f::xrSession.calls;
    f::PublishOpponentWorld();CHECK(!rt::SelectionWitness());
    CHECK(e::nativeReads==completedReads && f::xrSession.calls==completedXrCalls);
    rt::fixtureDiagnosticComplete=false;
    rt::fixtureControlledDormant=true;f::PublishOpponentWorld();CHECK(!rt::SelectionWitness());
    CHECK(e::nativeReads==completedReads && f::xrSession.calls==completedXrCalls);
    rt::fixtureControlledDormant=false;
    // A real XR poll decision must revoke BEFORE any further Present/Tick.
    for(unsigned loss=0;loss<3;++loss) {
        f::Setup();f::PublishOpponentWorld();CHECK(rt::SelectionWitness());const auto ticks=rt::ticks;
        if(loss==0)f::xrSession.stopOnPoll=true;
        if(loss==1)f::xrSession.failOnPoll=true;
        if(loss==2)f::xrSession.unfocusOnPoll=true;
        f::PollOpponentXr();CHECK(!rt::SelectionWitness() && rt::ticks==ticks);
        f::PublishOpponentWorld();CHECK(!rt::SelectionWitness());
    }
    f::Setup();f::PublishOpponentWorld();CHECK(rt::SelectionWitness());
    f::ShutdownOpponentXr();CHECK(!rt::SelectionWitness() && f::xrStopped);
    // Actual extracted lifecycle functions revoke even on refusal/wrong thread.
    f::Setup();f::PublishOpponentWorld();const auto beforeSuspendTicks=rt::ticks;
    CHECK(e::SuspendSession());CHECK(!rt::SelectionWitness() && rt::ticks==beforeSuspendTicks && !e::opponentVisibilityFrame);
    CHECK(e::ResumeSession());CHECK(!e::OpponentLodWorldReady());
    e::AcceptedCenter(60);f::PublishOpponentWorld();CHECK(rt::SelectionWitness());
    CHECK(e::StopSession());CHECK(!rt::SelectionWitness());
    f::Setup();f::PublishOpponentWorld();CHECK(e::SuspendForSwapchainChange());CHECK(!rt::SelectionWitness());
    f::Setup();f::PublishOpponentWorld();e::FailSession();CHECK(!rt::SelectionWitness() && e::SessionFailed());
    f::Setup();f::PublishOpponentWorld();const auto xrCalls=f::xrSession.calls;
    std::thread worker([]{grid2vr::engine::FailSession();});worker.join();
    CHECK(e::sessionFailureRequested && !rt::SelectionWitness() && f::xrSession.calls==xrCalls);
    f::PublishOpponentWorld();CHECK(!rt::SelectionWitness());
    bool workerReady=true;
    std::thread reader([&]{workerReady=e::OpponentLodWorldReady();});reader.join();
    CHECK(!workerReady && f::xrSession.calls==xrCalls);
    // Publication recheck: worker invalidation inside Tick cannot leave a stale
    // true decision enabled after Publish returns. No second enabling Tick.
    f::Setup();rt::duringTick=[] {std::thread other([]{grid2vr::engine::FailSession();});other.join();};
    f::PublishOpponentWorld();CHECK(!rt::SelectionWitness());
    f::Setup();rt::duringTick=[] {std::thread other([]{CHECK(!grid2vr::engine::SuspendSession());});other.join();};
    f::PublishOpponentWorld();CHECK(!rt::SelectionWitness());
    // Actual source predicate, not just pure policy booleans: invalid ownership,
    // stale/invalid consumer, menu, recorded playback and unreadable phase refuse.
    for(unsigned invalid=0;invalid<13;++invalid) {
        f::Setup();
        switch(invalid) {
        case 0:e::currentEyeCamera.valid=false;break;
        case 1:e::opponentVisibilityFrame=49;break;
        case 2:e::visibilityConsumerFrame=49;break;
        case 3:e::menuScene=true;break;
        case 4:e::menuSceneFrame=49;break;
        case 5:e::Put(e::mainObject+0x138,uint8_t(0));break;
        case 6:e::Put(e::mainObject+0x51,uint8_t(0));break;
        case 7:e::Put(e::mainObject,static_cast<void**>(nullptr));break;
        case 8:e::Put(e::gameBase+0xe61010,uintptr_t(1));break;
        case 9:e::Put(e::gameBase+0xdef804,uintptr_t(0));break;
        case 10:e::inReplay=true;break;
        case 11:e::replayPass=1;break;
        case 12:
            e::Put(e::gameBase+0xe61010,uintptr_t(0x4000000));
            e::Put(0x40000a8,uintptr_t(0x4100000));e::Put(0x4100000,e::gameBase+0xd1266c);break;
        }
        f::PublishOpponentWorld();CHECK(!rt::SelectionWitness());
    }
    for(unsigned invalid=0;invalid<6;++invalid) {
        f::Setup();
        if(invalid==0)f::runtimeStopped=true;
        if(invalid==1)f::recordedRequested=true;
        if(invalid==2)f::xrRequested=false;
        if(invalid==3)f::xrProjection=false;
        if(invalid==4)f::xrStopped=true;
        if(invalid==5)f::xrSession.views=false;
        f::PublishOpponentWorld();CHECK(!rt::SelectionWitness());
    }
    std::puts("PASS: production host predicates, four advancing stereo pairs with constant startup marker, immediate XR/worker/suspend/stop revocation, publication recheck, fresh ownership/phase refusals; ZERO game/XR/device/window execution");
}
