#include "replay_sequence.h"
#include "scene_views.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <thread>

using Microsoft::WRL::ComPtr;
using grid2vr::ReplaySequence;
using grid2vr::SceneViewFrame;
namespace {
ReplaySequence replaySequence;
SceneViewFrame frozenViews;
std::atomic<uint64_t> frame{51};
DWORD renderThread{};
bool runtimeSession{true},inReplay{},visibilityHealthy{true},eyeHistoryPrimed{};
uint64_t visibilityReadyFrame{};
struct {bool active{};uint64_t frame{};} pendingVisibility;
struct {bool valid{};} pendingScene;
struct Camera {unsigned sentinel{};} currentEyeCamera,previousEyeCamera,renderedEyeHistories;
bool nativeGuard{true},endFrameOk{true},historyOk{true};
unsigned restores{},ends{},resets{};
ComPtr<ID3D11Texture2D> ownedBackbuffer;
bool Continuous() {return runtimeSession && replaySequence.Continuous();}
bool SessionFailed() {return replaySequence.Status()==ReplaySequence::State::Failed;}
void FailSession() {replaySequence.Fail();}
bool ReadyForCapture() {return true;}
void Log(const char*,...) {}
bool RestoreVisibility(const char*) {
    if (!nativeGuard) return false;
    ++restores;pendingVisibility.active=false;return true;
}
void EndFrame(uint64_t value,uint64_t scene,bool accepted) {
    ++ends;
    if (value!=frame || scene || accepted || pendingVisibility.active || frozenViews.Pending() || !endFrameOk) FailSession();
}
bool ResetHistory() {
    ++resets;
    if (!historyOk || pendingVisibility.active || frozenViews.Pending() || pendingScene.valid) return false;
    ownedBackbuffer.Reset();return true;
}
bool (*resetHistory)()=ResetHistory;
void (*observePair)(uint64_t,uint64_t,bool)=EndFrame;
#include "swapchain_session.inc"

void Setup() {
    replaySequence={};frozenViews={};frame=51;renderThread=GetCurrentThreadId();
    runtimeSession=visibilityHealthy=nativeGuard=endFrameOk=historyOk=true;inReplay=false;
    pendingScene={};currentEyeCamera={1};previousEyeCamera={2};renderedEyeHistories={3};
    eyeHistoryPrimed=true;visibilityReadyFrame=50;restores=ends=resets=0;
    observePair=EndFrame;resetHistory=ResetHistory;
    replaySequence.ConfigureSession();replaySequence.Resume();
    const grid2vr::SceneViews views{{grid2vr::ScenePose{},grid2vr::ScenePose{}},
        {grid2vr::diagnosticFrustum,grid2vr::diagnosticFrustum}};
    frozenViews.Freeze(50,views);replaySequence.Begin(50);replaySequence.Finish(true);frozenViews.Finish(50,true);
    frozenViews.Freeze(51,views);pendingVisibility={true,51};
}
}
#define CHECK(test) do {if (!(test)) {std::fprintf(stderr,"line %d: %s\n",__LINE__,#test);return 1;}} while(0)
int main() {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context)));
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory> factory;
    CHECK(SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))));
    const auto window=CreateWindowExW(0,L"STATIC",L"GRID2VR resize lifecycle fixture",WS_OVERLAPPEDWINDOW,0,0,320,180,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window && !IsWindowVisible(window));
    DXGI_SWAP_CHAIN_DESC desc{};desc.OutputWindow=window;desc.Windowed=TRUE;desc.BufferCount=2;
    desc.BufferDesc.Width=320;desc.BufferDesc.Height=180;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.SampleDesc.Count=1;
    ComPtr<IDXGISwapChain> chain;CHECK(SUCCEEDED(factory->CreateSwapChain(device.Get(),&desc,&chain)));
    Setup();CHECK(SUCCEEDED(chain->GetBuffer(0,IID_PPV_ARGS(&ownedBackbuffer))));
    // Old path rejects the captured visibility-only prefix and retains GPU
    // references. A real DXGI resize cannot proceed with that backbuffer held.
    CHECK(!SuspendSession() && SessionFailed() && resets==0 && ownedBackbuffer);
    CHECK(FAILED(chain->ResizeBuffers(0,400,200,DXGI_FORMAT_UNKNOWN,0)));
    Setup();CHECK(SuspendForSwapchainChange() && !SessionFailed());
    CHECK(restores==1 && ends==1 && resets==1 && !ownedBackbuffer && !frozenViews.Pending());
    CHECK(!eyeHistoryPrimed && !visibilityReadyFrame && !currentEyeCamera.sentinel && !previousEyeCamera.sentinel && !renderedEyeHistories.sentinel);
    CHECK(SUCCEEDED(chain->ResizeBuffers(0,400,200,DXGI_FORMAT_UNKNOWN,0)));
    CHECK(ResumeSession() && replaySequence.Epoch()==2);
    const grid2vr::SceneViews views{{grid2vr::ScenePose{},grid2vr::ScenePose{}},
        {grid2vr::diagnosticFrustum,grid2vr::diagnosticFrustum}};
    CHECK(frozenViews.Freeze(60,views) && replaySequence.Begin(60));
    CHECK(frozenViews.Get(60)); // Both eyes share the new immutable view pair.
    replaySequence.Finish(true);CHECK(frozenViews.Finish(60,true) && replaySequence.Completed()==2);
    for (unsigned fault=0;fault<10;++fault) {
        Setup();CHECK(SUCCEEDED(chain->GetBuffer(0,IID_PPV_ARGS(&ownedBackbuffer))));
        if (fault==0) pendingScene.valid=true;
        if (fault==1) inReplay=true;
        if (fault==2) CHECK(replaySequence.Begin(51));
        if (fault==3) pendingVisibility.frame=50;
        if (fault==4) visibilityHealthy=false;
        if (fault==5) pendingVisibility.active=false;
        if (fault==6) observePair=nullptr;
        if (fault==7) CHECK(frozenViews.CancelBeforeReplay(51));
        if (fault==8) nativeGuard=false;
        bool result{};
        if (fault==9) {std::thread other([&]{result=SuspendForSwapchainChange();});other.join();}
        else result=SuspendForSwapchainChange();
        CHECK(!result && !restores && !ends && !resets && ownedBackbuffer);
        ownedBackbuffer.Reset();
    }
    Setup();endFrameOk=false;CHECK(!SuspendForSwapchainChange() && SessionFailed() && restores==1 && ends==1 && resets==0);
    Setup();historyOk=false;CHECK(!SuspendForSwapchainChange() && SessionFailed() && resets==1);
    Setup();CHECK(RestoreVisibility("fixture") && frozenViews.CancelBeforeReplay(51));restores=0;
    CHECK(SuspendForSwapchainChange() && !restores && !ends && resets==1);
    CHECK(SuspendForSwapchainChange() && resets==1); // Already suspended is harmless.
    chain.Reset();DestroyWindow(window);
    std::puts("Swapchain: pre-scene cancellation, zero-layer end, GPU release, real resize, fresh epoch and unsafe-state refusals passed.");
    return 0;
}
