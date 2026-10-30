#include "diagnostics.h"
#include "build_identity.h"
#include "game_executable.h"
#include "foreground_hold.h"
#include "garage_presentation.h"
#include "pixels.h"
#include "camera_shader.h"
#include "engine_trace.h"
#include "replay_trace.h"
#include "spotlight_witness.h"
#include "cockpit_animation.h"
#include "opponent_lod_runtime.h"
#include "opponent_lod_bridge.h"
#include "opponent_lod_world_policy.h"
#include "texture_digest.h"
#include "eye_textures.h"
#include "eye_mirror.h"
#include "preview_frame.h"
#include "screen_ui.h"
#include "msaa_readback.h"
#include "msaa_audit.h"
#include "focus_cockpit.h"
#include "dlc_cockpit.h"
#include "replay_profile.h"
#include "bounded_log.h"
#include "flicker_capture.h"
#include "driving_controls.h"
#include "player_control.h"
#include "background_play.h"
#include "ffb_observer.h"
#include "flashback_observer.h"
#include "chase_observer.h"
#include "chase_camera.h"
#include "solo_race.h"
#include "guarded_hooks.h"
#include "instance_capacity.h"
#include "headset_visibility_candidate.h"
#include "process_exit.h"
#include "stop_receipt.h"
#include "scene_resolution.h"
#include "frontend_video_resize.h"
#include "exposure_audit.h"
#ifdef GRID2VR_ENABLE_XR_SESSION
#include "xr_session.h"
#include "tracked_views.h"
#include "recenter_hotkey.h"
#endif
#include <MinHook.h>
#include <intrin.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <bcrypt.h>
#include <psapi.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace grid2vr {
namespace {
using Microsoft::WRL::ComPtr;
std::filesystem::path output;
std::mutex logMutex, attachMutex, shaderMutex;
std::set<void*> hooked;
std::set<uint64_t> shaders;
std::atomic<uint64_t> frame{}, draws{}, dispatches{};
std::atomic<unsigned> cameraSamples{};
std::atomic<unsigned> drawSamples{};
std::atomic<uint64_t> captureUntil{};
uintptr_t gameBase{};
ComPtr<ID3D11DeviceContext> engineContext;
bool captureStageTargets{};
bool traceReplayWork{};
bool traceReplaySamples{};
bool verifyFreshReplayColor{};
bool repeatAoDraw{};
bool repeatShadowDraw{};
bool repeatShadowBindingControl{};
bool retainEyeTextures{};
bool runtimeSession{},runtimeStopped{};
bool diagnosticLogging{true};
StopReceipt stopReceipt;
FocusCockpit focusCockpit;
EyeTextures eyeTextures;
EyeMirror eyeMirror;
PreviewFrame previewFrame;
LateScreenUi lateUi;
bool lateUiEnabled{};
ScreenUiNativeWitness nativeUiWitness;
bool nativeUiWitnessEnabled{};
FlatScreenUi flatMenuUi;
SceneViews acquiredViews;
uint64_t acquiredViewsFrame{},lateUiScene{};
void FinishLateUi(bool present);
bool mirrorPreview{};
uint64_t previewDigestBudget=64ull*1024*1024;
std::vector<SceneViews> recordedViews;
size_t recordedIndex{};
bool recordedRequested{};
bool repeatRecordedViews{}; // Bounded unattended observation only; never live XR.
bool unattendedStereo{};
#ifdef GRID2VR_ENABLE_XR_SESSION
XrSessionProbe xrSession;
bool xrRequested{},xrStopped{};
// Read by a foreign ExitProcess caller: XR objects may remain until a closed shutdown.
std::atomic<bool> xrLive{};
bool xrProjection{},xrRecentered{};
bool recenterKeepHorizonLevel{true};
XrPosef xrReference{};
float xrUnitsPerMetre{1};
RecenterShortcut recenterBinding;
bool recenterBindingValid{};
bool OpponentXrReady() noexcept {
    return runtimeSession && !runtimeStopped && !recordedRequested && xrRequested && xrProjection && !xrStopped &&
        xrSession.Running() && !xrSession.Failed() && xrSession.ReadyForViews(true);
}
void PollOpponentXr() {
    xrSession.Poll();
    if(!OpponentXrReady()) opponentlod::host::RevokeWorld();
}
void ShutdownOpponentXr() {
    opponentlod::host::RevokeWorld();
    xrSession.Shutdown();xrStopped=true;xrLive=!xrSession.Closed();
}
#endif
void PublishOpponentWorld() noexcept {
    opponentlod::host::PublishWorld([]() noexcept {
#ifdef GRID2VR_ENABLE_XR_SESSION
        return opponentlod::runtime::WorldChecksNeeded() && engine::OpponentLodWorldReady() && OpponentXrReady();
#else
        return false;
#endif
    });
}
uint64_t eyeTextureScene{},eyeDigestBudget=256ull*1024*1024;
// Diagnostic metadata belongs to the shader object and follows its COM lifetime.
constexpr GUID cameraBindingId{0xbe029720,0xb43d,0x437d,{0xa5,0xed,0x47,0x90,0x48,0xf5,0x3c,0x96}};

void Log(const char* format, ...) {
    profile::Scope measure(profile::GraphicsLog);
    char line[2048]{};
    va_list args; va_start(args,format); vsnprintf_s(line,sizeof(line),_TRUNCATE,format,args); va_end(args);
    if (runtimeSession && !diagnosticLogging && !strstr(line,"session ") && !strstr(line,"refused") &&
        !strstr(line,"failed") && !strstr(line,"initialized") && !strstr(line,"requested") &&
        !strstr(line,"configured") && !strstr(line,"shortcut") && !strstr(line,"CreateSwapChain") && !strstr(line,"ResizeBuffers")) return;
    std::lock_guard lock(logMutex);
    static BoundedLog file(output/L"trace.log",runtimeSession?sessionLogSegmentBytes:0);
    file << GetTickCount64() << " tid=" << GetCurrentThreadId() << ' ' << line << '\n';
    if (runtimeSession && (strstr(line,"session ") || strstr(line,"refused") || strstr(line,"failed"))) file.FlushNow();
    else file.flush();
}

void RecordStartupPhase(unsigned phase,uint64_t sceneFrame) {
    if (!runtimeSession || !sceneFrame || phase>=5) return;
    struct Mark { uint64_t frame{},tick{}; };
    static std::array<Mark,5> marks{};
    if (marks[phase].frame || (phase && !marks[0].frame)) return;
    marks[phase]={sceneFrame,GetTickCount64()};
    constexpr const char* names[]{"firstPresent","captureRequested","firstValidatedMainObject","firstNativeCamera","firstStereoPair"};
    std::ofstream receipt(output/L"stereo-startup.json",std::ios::trunc);
    receipt << "{\"schema\":1,\"elapsedFrom\":\"firstPresent\"";
    for (unsigned i=0;i<marks.size();++i) {
        receipt << ",\"" << names[i] << "\":";
        if (marks[i].frame) receipt << "{\"frame\":" << marks[i].frame << ",\"elapsedMs\":" << marks[i].tick-marks[0].tick << '}';
        else receipt << "null";
    }
    receipt << "}\n";
}

void Stack(const char* label) {
    void* entries[20]{};
    const auto count=CaptureStackBackTrace(1,20,entries,nullptr);
    std::string text;
    for (unsigned i=0;i<count;++i) {
        char item[64]{};
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(entries[i],&region,sizeof(region)) && reinterpret_cast<uintptr_t>(region.AllocationBase)==gameBase)
            sprintf_s(item," game+0x%zx",reinterpret_cast<uintptr_t>(entries[i])-gameBase);
        else sprintf_s(item," %p",entries[i]);
        text+=item;
    }
    Log("stack %s:%s",label,text.c_str());
}

const char* BeforeGameExit() {
    opponentlod::host::RevokeWorld();
    focusCockpit.Summary();
    // ExitProcess can bypass the next Present entirely. Stop at this verified
    // owning-thread boundary, but never attempt to unwind an incomplete eye pair.
    if(engine::Continuous() && !runtimeStopped && !engine::StopSession()) {
        Log("session exit cleanup refused reason=renderer_not_restored");
        return "renderer_not_restored";
    }
    runtimeStopped=true;
    frontendvideo::Stop();
    playercontrol::Stop();backgroundplay::Stop();ffbobserver::Stop();flashbackobserver::Stop();drivingcontrols::Stop();chaseobserver::Stop();chasecamera::Stop();dlccockpit::Stop();cockpitanimation::Stop();opponentlod::runtime::Stop();
    if (!flatMenuUi.Reset()) return "flat_menu_not_restored";
#ifdef GRID2VR_ENABLE_XR_SESSION
    recenter::Enable(false);
    if(xrRequested && !xrStopped) {
        ShutdownOpponentXr();
        if(!xrSession.Closed() || xrSession.Failed()) return "xr_shutdown_incomplete";
        xrStopped=true;
    }
#endif
    if(!stopReceipt.GameExit(output/L"runtime-stopped")) return "stop_receipt_failed";
    Log("session exit cleanup complete=1 frame=%llu",frame.load());
    return "complete";
}

template<class T> bool Hook(void* object,unsigned slot,void* replacement,T& original) {
    const auto target=(*static_cast<void***>(object))[slot];
    if (hooked.contains(target)) return true;
    // One implementation per interface method for this diagnostic process.
    if (original) { Log("hook refused: second implementation for slot %u",slot); return false; }
    auto status=MH_CreateHook(target,replacement,reinterpret_cast<void**>(&original));
    if (status==MH_OK) status=MH_EnableHook(target);
    Log("session hook slot=%u status=%s",slot,MH_StatusToString(status));
    if (status==MH_OK) hooked.insert(target);
    return status==MH_OK;
}

HWND diagnosticWindow{};
bool manualStereoStart{},manualStereoReady{},manualStereoReleased{};
ForegroundHold foregroundHold;
ForegroundHold backgroundHold;
ForegroundHold normalXrHold;
unsigned WindowState(uint64_t frameId,bool force=false) {
    if (!diagnosticWindow) return ~0u;
    DWORD foregroundProcess{};GetWindowThreadProcessId(GetForegroundWindow(),&foregroundProcess);
    const unsigned state=(foregroundProcess==GetCurrentProcessId()?1u:0u) |
        (IsIconic(diagnosticWindow)?2u:0u) | (IsWindowVisible(diagnosticWindow)?4u:0u);
    static unsigned previous=~0u,changes{};
    if (force || state!=previous) {
        if (changes<512) Log("window_state frame=%llu foreground_game=%u minimized=%u visible=%u",frameId,state&1u,(state>>1)&1u,(state>>2)&1u);
        else if (changes==512) Log("window_state truncated=1 limit=512");
        if (changes<=512) ++changes;
        previous=state;
    }
    return state;
}
void Describe(IDXGISwapChain* chain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(chain->GetDesc(&desc))) return;
    Log("swapchain=%p size=%ux%u format=%u samples=%u buffers=%u windowed=%d effect=%u",chain,
        desc.BufferDesc.Width,desc.BufferDesc.Height,desc.BufferDesc.Format,desc.SampleDesc.Count,
        desc.BufferCount,desc.Windowed,desc.SwapEffect);
    diagnosticWindow=desc.OutputWindow;
#ifdef GRID2VR_ENABLE_XR_SESSION
    if (recenterBindingValid)
        Log("session recenter shortcut attached=%u key=%u modifiers=%u",recenter::Attach(diagnosticWindow,recenterBinding),recenterBinding.key,recenterBinding.modifiers);
#endif
    WindowState(frame.load(),true);
}

bool CaptureTexture(ID3D11Texture2D* back,const std::string& name,unsigned thumbnailWidth=0) {
    if (runtimeSession && !thumbnailWidth) return false;
    ComPtr<ID3D11Texture2D> source,staging;
    D3D11_TEXTURE2D_DESC desc{}; back->GetDesc(&desc);
    const bool depth24=desc.Format==DXGI_FORMAT_R24G8_TYPELESS || desc.Format==DXGI_FORMAT_D24_UNORM_S8_UINT;
    // This diagnostic saves the whole single-subresource scene target only.
    if (desc.ArraySize!=1 || desc.MipLevels!=1) { Log("capture refused non-simple target=%s",name.c_str()); return false; }
    if (desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
        desc.Format!=DXGI_FORMAT_R10G10B10A2_UNORM &&
        !(traceReplayWork && (desc.Format==DXGI_FORMAT_R16G16_UNORM || (depth24 && desc.SampleDesc.Count==1)))) { Log("capture unsupported format=%u",desc.Format); return false; }
    ComPtr<ID3D11Device> device; back->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context);
    source=back;
    if (desc.SampleDesc.Count>1) {
        desc.SampleDesc={1,0}; desc.BindFlags=0; desc.MiscFlags=0;
        if (FAILED(device->CreateTexture2D(&desc,nullptr,&source))) return false;
        context->ResolveSubresource(source.Get(),0,back,0,desc.Format);
    }
    desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    desc.BindFlags=0; desc.MiscFlags=0;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&staging))) return false;
    context->CopyResource(staging.Get(),source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return false;
    // Work comparisons use source-precision .raw. Keep previews for frame/stage
    // evidence; avoid hundreds of redundant per-draw RGB8 conversions and files.
    const bool preview=!traceReplayWork || !name.starts_with("work-");
    std::ofstream file;
    if (preview) file.open(output/(name+".ppm"),std::ios::binary);
    std::ofstream raw;
    if (traceReplayWork && !thumbnailWidth) {
        raw.open(output/(name+".raw"),std::ios::binary);
        D3D11_TEXTURE2D_DESC original{}; back->GetDesc(&original);
        // Tight rows after resolve, with source format/sample count retained.
        const uint32_t header[]{desc.Width,desc.Height,static_cast<uint32_t>(desc.Format),original.SampleDesc.Count};
        raw.write(reinterpret_cast<const char*>(header),sizeof(header));
    }
    const UINT step=thumbnailWidth?std::max(1u,(desc.Width+thumbnailWidth-1)/thumbnailWidth):1;
    const UINT width=(desc.Width+step-1)/step,height=(desc.Height+step-1)/step;
    if (preview) file << "P6\n" << width << ' ' << height << "\n255\n";
    const bool bgra=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool rgb10=desc.Format==DXGI_FORMAT_R10G10B10A2_UNORM;
    const bool rg16=desc.Format==DXGI_FORMAT_R16G16_UNORM;
    std::vector<char> row(width*3);
    for (UINT y=0;y<desc.Height;y+=step) {
        const auto* pixels=static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch;
        if (raw.is_open()) raw.write(reinterpret_cast<const char*>(pixels),desc.Width*4);
        if (!preview) continue;
        for (UINT column=0;column<width;++column) {
            const UINT x=column*step;
            if (depth24) {
                uint32_t packed{}; memcpy(&packed,pixels+x*4,4);
                for (unsigned channel=0;channel<3;++channel) row[column*3+channel]=static_cast<char>((packed>>16)&255);
            } else if (rg16) {
                uint16_t channels[2]{}; memcpy(channels,pixels+x*4,4);
                row[column*3]=static_cast<char>(channels[0]>>8); row[column*3+1]=static_cast<char>(channels[1]>>8); row[column*3+2]=0;
            } else if (rgb10) {
                uint32_t packed{}; memcpy(&packed,pixels+x*4,4);
                const auto rgb=Rgb10ToRgb8(packed);
                for(unsigned channel=0;channel<3;++channel) row[column*3+channel]=rgb[channel];
            } else {
                row[column*3]=pixels[x*4+(bgra?2:0)]; row[column*3+1]=pixels[x*4+1]; row[column*3+2]=pixels[x*4+(bgra?0:2)];
            }
        }
        file.write(row.data(),row.size());
    }
    context->Unmap(staging.Get(),0);
    if (preview) file.close();
    if (raw.is_open()) raw.close();
    D3D11_TEXTURE2D_DESC original{}; back->GetDesc(&original);
    Log("capture name=%s frame=%llu source=%ux%u format=%u samples=%u saved=%d",name.c_str(),frame.load(),
        original.Width,original.Height,original.Format,original.SampleDesc.Count,file.good());
    return file.good() && (!traceReplayWork || thumbnailWidth || raw.good());
}

void Screenshot(IDXGISwapChain* chain,uint64_t number) {
    ComPtr<ID3D11Texture2D> back;
    if (SUCCEEDED(chain->GetBuffer(0,IID_PPV_ARGS(&back)))) CaptureTexture(back.Get(),"frame-"+std::to_string(number));
}

void FrontendPreview(IDXGISwapChain* chain,uint64_t number) {
    // Two small passive images in an explicit desktop-only audit. No input,
    // window manipulation, headset work or full-resolution capture retention.
    static const bool enabled=std::filesystem::is_regular_file(output/L"normal-launch.request") &&
        std::filesystem::is_regular_file(output/L"frontend-scene-audit.request") &&
        !std::filesystem::exists(output/L"xr-session.request");
    if(!enabled)return;
    static const ULONGLONG started=GetTickCount64();
    static unsigned next=0;
    constexpr ULONGLONG times[]{10000,50000};
    if(next>=2 || GetTickCount64()-started<times[next])return;
    ++next;
    ComPtr<ID3D11Texture2D> back;
    if(SUCCEEDED(chain->GetBuffer(0,IID_PPV_ARGS(&back))))
        CaptureTexture(back.Get(),"frame-"+std::to_string(number),960);
}

using PresentFn=HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
using ResizeFn=HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT);
using SwapFn=HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory*,IUnknown*,DXGI_SWAP_CHAIN_DESC*,IDXGISwapChain**);
using VSFn=HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11VertexShader**);
using LayoutFn=HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*,const D3D11_INPUT_ELEMENT_DESC*,UINT,const void*,SIZE_T,ID3D11InputLayout**);
using PSFn=HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11PixelShader**);
using CSFn=HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11ComputeShader**);
using MapFn=HRESULT (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT);
using DrawFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
using DrawIndexedFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT);
using DrawInstancedFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,UINT);
using DrawIndexedInstancedFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,INT,UINT);
using DispatchFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT);
using ClearColorFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11RenderTargetView*,const FLOAT*);
using ClearDepthFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11DepthStencilView*,UINT,FLOAT,UINT8);
using QueryFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Asynchronous*);
using QueryDataFn=HRESULT (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Asynchronous*,void*,UINT,UINT);
QueryFn realQueryBegin{},realQueryEnd{};QueryDataFn realQueryData{};
PresentFn realPresent{}; ResizeFn realResize{}; SwapFn realSwap{}; VSFn realVS{}; PSFn realPS{};
CSFn realCS{};
LayoutFn realLayout{};
MapFn realMap{}; UnmapFn realUnmap{}; DrawFn realDraw{}; DrawIndexedFn realDrawIndexed{};
DrawInstancedFn realDrawInstanced{}; DrawIndexedInstancedFn realDrawIndexedInstanced{}; DispatchFn realDispatch{};
ClearColorFn realClearColor{}; ClearDepthFn realClearDepth{};
void CaptureComputeResource(ID3D11DeviceContext* c,ID3D11Resource* resource,const std::string& name);

const char* StopCameraHooksForExit() {
    const auto observer=chaseobserver::StopForExit();
    const auto controls=chasecamera::StopForExit();
    return std::strcmp(controls,"controls_not_installed")?controls:observer;
}
bool ExitLeavesLiveXr() {
#ifdef GRID2VR_ENABLE_XR_SESSION
    return xrLive.load();
#else
    return false;
#endif
}
bool EnsureExitHandler() {
    if (!runtimeSession) return true;
    static bool attempted=false,ready=false;
    if (!attempted) { attempted=true;ready=processexit::Install(gameBase,output,BeforeGameExit,StopCameraHooksForExit,ExitLeavesLiveXr);Log("session exit handler installed=%u",ready); }
    return ready;
}
#ifdef GRID2VR_ENABLE_XR_SESSION
void InitializeXr(ID3D11Device* device) {
    static bool attempted=false;
    if (attempted) return;
    attempted=true;
    xrRequested=std::filesystem::exists(output/L"xr-session.request");
    xrProjection=std::filesystem::exists(output/L"xr-projection.request");
    if (!xrRequested) return;
    xrLive=true;
    std::wifstream request(output/L"xr-session.request");std::wstring runtime;std::getline(request,runtime);
    // Early menu-state hooks regressed native loading before stereo starts.
    // Keep the established window/ExitProcess cleanup path; the menu gate is
    // restricted to its isolated fixture until live startup safety is proven.
    const bool closeReady=EnsureExitHandler() && processexit::AttachWindow(diagnosticWindow);
    Log("session close handler attached=%u",closeReady);
    bool initialized=closeReady && !std::filesystem::exists(output/L"recorded-views.request") && xrSession.Initialize(device,runtime,output);
    if (initialized && xrProjection) {
        unsigned width{},height{};std::ifstream settings(output/L"xr-projection.request");
        settings>>width>>height>>xrUnitsPerMetre;const bool parsed=bool(settings);settings>>std::ws;
        initialized=parsed && settings.eof() && std::isfinite(xrUnitsPerMetre) && xrUnitsPerMetre>=.1f && xrUnitsPerMetre<=10 &&
            xrSession.InitializeImages(realDraw,width,height);
    }
    Log("openxr session requested=1 initialized=%u first_present=%u",initialized,runtimeSession);
    if (!initialized) ShutdownOpponentXr();
}
#endif

engine::ViewResult ProvideViews(uint64_t frameId,SceneViews& views) {
    using Result=engine::ViewResult;
    const auto refuse=[](Result result) {opponentlod::host::RevokeWorld();return result;};
    acquiredViewsFrame=0;
    if (recordedRequested) {
        opponentlod::host::RevokeWorld();
        if(repeatRecordedViews && recordedIndex==recordedViews.size())recordedIndex=0;
        if (recordedIndex>=recordedViews.size()) return runtimeSession?Result::Unavailable:Result::Failed;
        views=recordedViews[recordedIndex++];acquiredViews=views;acquiredViewsFrame=frameId;return Result::Ready;
    }
#ifdef GRID2VR_ENABLE_XR_SESSION
    if (!xrProjection || xrStopped || xrSession.Failed()) return refuse(Result::Failed);
    // Catch dashboard transitions before acquiring views or changing native
    // camera/visibility state. VISIBLE can still report shouldRender=true.
    if (runtimeSession) PollOpponentXr();
    if (xrSession.Failed()) return refuse(Result::Failed);
    if (!xrSession.Running() || (runtimeSession && !xrSession.Focused())) return refuse(Result::Unavailable);
    if (!xrSession.BeginFrame(frameId)) return refuse(Result::Failed);
    const auto* sample=xrSession.Views();
    bool valid=sample && sample->frame==frameId && sample->tracked && sample->shouldRender;
    if (runtimeSession && sample && sample->frame==frameId && !valid) {
        opponentlod::host::RevokeWorld();
        return xrSession.FinishFrame(frameId,false)?Result::Unavailable:Result::Failed;
    }
    if (valid && !xrRecentered) {
        valid=MakeRecenterReference(sample->head,recenterKeepHorizonLevel,xrReference);
        xrRecentered=valid;
        if (valid) Log("openxr recentered frame=%llu units_per_metre=%.9g keep_horizon_level=%u",frameId,xrUnitsPerMetre,recenterKeepHorizonLevel);
    }
    bool outOfRange=false;
    valid=valid && MakeTrackedSceneViews(sample->eyes,sample->head,xrReference,xrUnitsPerMetre,views,&outOfRange);
    if (!valid) {
        opponentlod::host::RevokeWorld();
        const bool ended=xrSession.FinishFrame(frameId,false);
        if (runtimeSession && outOfRange) {
            Log("tracking translation outside_range frame=%llu limit_native=%.9g",frameId,maxSceneTranslation);
            // No eye/visibility state has been changed. Use the existing clean
            // suspension path; returning inside the bound starts a fresh epoch.
            return ended?Result::Unavailable:Result::Failed;
        }
    }
    if (valid) {acquiredViews=views;acquiredViewsFrame=frameId;}
    return valid?Result::Ready:refuse(Result::Failed);
#else
    return refuse(Result::Failed);
#endif
}
bool ResetEyeHistory() {
    opponentlod::host::RevokeWorld();
    nativeUiWitness.End(true);
    FinishLateUi(false);lateUi.Reset();
    previewFrame.Reset();
    if (!replaytrace::ResetHistory()) return false;
    eyeTextures.Reset();eyeTextureScene=0;Log("session eye_images_invalidated=1");return true;
}
bool CancelEyeHistory() { opponentlod::host::RevokeWorld();return replaytrace::CancelBeforeReplay(); }
void CompleteViews(uint64_t frameId,uint64_t scene,bool accepted) {
    const bool ready=accepted && scene && eyeTextures.Ready(scene) && replaytrace::PairHealthy();
    if(!ready) opponentlod::host::RevokeWorld();
    bool completed=ready;
    if(mirrorPreview)previewFrame.Publish(frameId,scene,ready,flicker::Capturing(scene));
    flicker::Complete(engineContext.Get(),frameId,scene,ready,
        {eyeTextures.Get(scene,0),eyeTextures.Get(scene,1),eyeTextures.Get(scene,2)});
    WindowState(frameId,!ready);
    Log("views output frame=%llu scene=%llu accepted=%u ready=%u",frameId,scene,accepted,ready);
    if (nativeUiWitnessEnabled && ready) {
        ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11Resource> resource;
        engineContext->OMGetRenderTargets(1,&target,nullptr);if (target) target->GetResource(&resource);
        nativeUiWitness.Begin(engineContext.Get(),frameId,reinterpret_cast<uintptr_t>(resource.Get()));
    }
    // Explicit diagnostic candidate only. The actual garage menu draw path is
    // still being identified; ordinary Normal launches keep existing timing.
    if (lateUiEnabled && ready && acquiredViewsFrame==frameId) {
        ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> native;
        engineContext->OMGetRenderTargets(1,&target,nullptr);
        if (target) target->GetResource(&resource);
        if (resource && SUCCEEDED(resource.As(&native)) &&
            lateUi.Begin(engineContext.Get(),frameId,acquiredViews,native.Get(),eyeTextures.Get(scene,1),eyeTextures.Get(scene,2))) {
            lateUiScene=scene;return;
        }
    }
#ifdef GRID2VR_ENABLE_XR_SESSION
    if (xrProjection && !xrStopped && xrSession.Views()) {
        const bool ended=xrSession.FinishFrame(frameId,ready,eyeTextures.Get(scene,1),eyeTextures.Get(scene,2));
        completed=ready && ended;
        if (runtimeSession && !ended) engine::FailSession();
        Log("openxr pair frame=%llu scene=%llu accepted=%u ended=%u",frameId,scene,ready,ended);
    }
#endif
    if (completed) RecordStartupPhase(4,frameId);
    else opponentlod::host::RevokeWorld();
}

void FinishLateUi(bool present) {
    const auto frameId=lateUi.Frame();if (!frameId) return;
    if (!lateUi.OnThread()) {Log("screen_ui late_finish_failed=1 reason=thread");engine::FailSession();return;}
    bool ready=present && frameId==frame.load() && !runtimeStopped && !engine::SessionFailed() &&
        eyeTextures.Ready(lateUiScene) && replaytrace::PairHealthy();
#ifdef GRID2VR_ENABLE_XR_SESSION
    if (xrProjection && !xrStopped && xrSession.Views()) {
        const bool ended=xrSession.FinishFrame(frameId,ready,eyeTextures.Get(lateUiScene,1),eyeTextures.Get(lateUiScene,2));
        if (!ended && runtimeSession) engine::FailSession();
        ready&=ended;
    }
#endif
    if (ready) RecordStartupPhase(4,frameId);
    else opponentlod::host::RevokeWorld();
    lateUi.End(!present);lateUiScene=0;
}

void MirrorPreview(IDXGISwapChain* chain,uint64_t frameId) {
    if(!mirrorPreview)return;
    const auto pending=previewFrame.Consume(frameId,!runtimeStopped && !engine::SessionFailed() && engine::SessionRendering());
    if(!pending.scene)return;
    // GetBuffer is local: ResizeBuffers must never inherit a retained reference.
    ComPtr<ID3D11Texture2D> back;
    auto* source=eyeTextures.Get(pending.scene,1);
    const bool copied=SUCCEEDED(chain->GetBuffer(0,IID_PPV_ARGS(&back))) &&
        eyeMirror.Copy(engineContext.Get(),source,back.Get());
    static unsigned rows{};
    if(rows++<900) {
        static std::ofstream report(output/L"preview-mirror.jsonl");
        report << "{\"frame\":" << frameId << ",\"scene\":" << pending.scene
               << ",\"eye\":1,\"copied\":" << copied << ",\"audit\":" << pending.audit;
        if(pending.audit && copied) {
            // Readbacks must not inherit conditional execution from the game.
            ComPtr<ID3D11Predicate> predicate;BOOL predicateValue{};
            engineContext->GetPredication(&predicate,&predicateValue);
            engineContext->SetPredication(nullptr,FALSE);
            const auto eye=DigestTexture(engineContext.Get(),source,previewDigestBudget);
            const auto preview=DigestTexture(engineContext.Get(),back.Get(),previewDigestBudget);
            engineContext->SetPredication(predicate.Get(),predicateValue);
            const bool valid=!strcmp(eye.status,"ok") && !strcmp(preview.status,"ok") &&
                eye.subresources.size()==1 && preview.subresources.size()==1;
            report << ",\"readbackPredicationDisabled\":true,\"readbackOk\":" << valid;
            if(valid)report << ",\"eyeSha256\":\"" << eye.subresources[0].sha256
                << "\",\"previewSha256\":\"" << preview.subresources[0].sha256 << '"';
        }
        report << "}\n";report.flush();
    }
    if(!copied) {
        // A preview mismatch must not change the working stereo renderer.
        mirrorPreview=false;previewFrame.Reset();
        D3D11_TEXTURE2D_DESC input{},target{};
        if(source)source->GetDesc(&input);if(back)back->GetDesc(&target);
        Log("preview mirror disabled reason=copy_refused source=%ux%u/%u target=%ux%u/%u",
            input.Width,input.Height,input.Format,target.Width,target.Height,target.Format);
    }
}

bool StageBoundary(engine::StageStamp stage,const char* phase) {
    profile::Scope measure(profile::GraphicsBoundary);
    const auto namePhase=engine::SnapshotPhase(phase);
    auto* context=engineContext.Get();
    if (!context) return false;
    if (runtimeSession && stage.slot==100 && !strcmp(phase,"scene-entry")) flicker::Begin(stage.id);
    replaytrace::SceneBoundary(context,phase);
    const bool eyePair=retainEyeTextures && stage.slot==100 && replaytrace::Active();
    const bool detailed=replaytrace::Detailed();
    const bool auditImages=detailed || replaytrace::FirstPair() || replaytrace::LastPair();
    auto imageHash=[&](ID3D11Texture2D* texture) {
        const auto digest=DigestTexture(context,texture,eyeDigestBudget);
        return !strcmp(digest.status,"ok") && digest.subresources.size()==1 ? digest.subresources[0].sha256:std::string("unavailable");
    };
    if (eyePair && !strcmp(phase,"scene-entry")) {
        eyeTextureScene=eyeTextures.Begin(stage.id,engine::DualEyes()?3:2)?stage.id:0;
    }
    if (eyePair && !strcmp(phase,"replay-restored") && (!engine::DualEyes() || engine::CurrentScene().pass==2)) {
        const bool ready=eyeTextures.Ready(stage.id);
        const auto left=auditImages?imageHash(eyeTextures.Get(stage.id,engine::DualEyes()?1:0)):"not_read_back";
        const auto right=auditImages?imageHash(eyeTextures.Get(stage.id,engine::DualEyes()?2:1)):"not_read_back";
        Log("eye pair id=%llu ready=%u left=%s right=%s",stage.id,ready,left.c_str(),right.c_str());
    }
    if (detailed && stage.slot==100 && (!strcmp(phase,"scene-complete") || !strcmp(phase,"replay-restored"))) {
        if (auto* scratch=replaytrace::ComputeScratchResource())
            CaptureComputeResource(context,scratch,"stage-"+std::to_string(stage.id)+"-"+namePhase+"-ao");
    }
    bool captured=false;
    UINT viewportCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    context->RSGetViewports(&viewportCount,viewports);
    Log("stage boundary id=%llu slot=%u phase=%s context=%p draws=%llu dispatches=%llu viewports=%u viewport=%.0fx%.0f",
        stage.id,stage.slot,namePhase.c_str(),context,draws.load(),dispatches.load(),viewportCount,
        viewportCount?viewports[0].Width:0,viewportCount?viewports[0].Height:0);
    ID3D11RenderTargetView* colors[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,colors,&depth);
    auto describe=[&](ID3D11View* view,const char* kind,unsigned index) {
        if (!view) return;
        ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture; D3D11_TEXTURE2D_DESC desc{};
        if (SUCCEEDED(resource.As(&texture))) texture->GetDesc(&desc);
        Log("stage target id=%llu phase=%s kind=%s index=%u view=%p resource=%p size=%ux%u format=%u samples=%u",
            stage.id,namePhase.c_str(),kind,index,view,resource.Get(),desc.Width,desc.Height,desc.Format,desc.SampleDesc.Count);
    };
    for (unsigned i=0;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;++i) {
        ComPtr<ID3D11RenderTargetView> view; view.Attach(colors[i]);
        describe(view.Get(),"color",i);
        // Passive native-size copy before later passes reuse this target.
        // Readback itself does not change camera or output binding.
        const bool slot6=stage.slot==6 && strcmp(phase,"after")==0;
        const bool slot5=traceReplayWork && stage.slot==5 && strcmp(phase,"after")==0;
        const bool complete=stage.slot==100 && (strcmp(phase,"scene-complete")==0 || strcmp(phase,"replay-complete")==0);
        if (captureStageTargets && i==0 && view && (slot5 || slot6 || complete)) {
            ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(resource.As(&texture))) {
                if (complete && i==0) {D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);sceneresolution::ObserveScene(desc.Width,desc.Height);}
                if(complete)flicker::FinishPass(context,stage.id,!strcmp(phase,"replay-complete")?engine::CurrentScene().pass:0,texture.Get());
                if (eyePair && complete) {
                    const unsigned eye=!strcmp(phase,"replay-complete")?engine::CurrentScene().pass:0;
                    const auto hash=auditImages?imageHash(texture.Get()):"not_read_back";
                    const bool copied=eyeTextureScene==stage.id && eyeTextures.Capture(context,stage.id,eye,texture.Get());
                    if (engine::RepeatingEyes()) captured=copied && replaytrace::PairHealthy();
                    Log("eye texture id=%llu eye=%u captured=%u source_sha256=%s",stage.id,eye,copied,hash.c_str());
                }
                const auto name="stage-"+std::to_string(stage.id)+(slot5?"-slot5":slot6?"-slot6":std::string("-")+namePhase);
                if (detailed || (complete && auditImages)) captured=CaptureTexture(texture.Get(),name) && (!engine::RepeatingEyes() || captured);
                static bool readbackControl=false;
                if (detailed && traceReplayWork && slot6 && !readbackControl) {
                    readbackControl=true;
                    CaptureTexture(texture.Get(),name+"-control");
                }
            }
        }
    }
    describe(depth.Get(),"depth",0);
    if (detailed && traceReplayWork && stage.slot==5 && strcmp(phase,"after")==0 && depth) {
        ComPtr<ID3D11Resource> resource; depth->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(resource.As(&texture))) CaptureTexture(texture.Get(),"stage-"+std::to_string(stage.id)+"-slot5-depth");
    }
    if (eyePair && !strcmp(phase,"replay-restored"))
        return eyeTextures.Captured(stage.id,engine::CurrentScene().pass+1) && replaytrace::PairHealthy();
    return captured;
}

HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* chain,UINT sync,UINT flags) {
    // Retain no permission while this Present is still deciding XR/engine state.
    // Only the final controller decision below can publish another world epoch.
    opponentlod::host::RevokeWorld();
    if (flags & DXGI_PRESENT_TEST) return realPresent(chain,sync,flags);
    if(runtimeStopped && stopReceipt.Reported()) {
        opponentlod::runtime::Tick(false);
        return realPresent(chain,sync,flags);
    }
    nativeUiWitness.End();
    FinishLateUi(true);
    const auto f=frame.fetch_add(1);
    if (!f) RecordStartupPhase(0,f+1);
    profile::NextFrame(f+1);
    engine::Frame(f+1);
    RecordStartupPhase(3,engine::NativeCameraFrame());
    if(runtimeSession) {
        // Live input state overrides also serve Press Start and frontend menus.
        // The host fingerprint is verified before Present is attached; this
        // path keeps native code intact and waits for guarded input/job instances.
        if(!f) {
            drivingcontrols::Install(gameBase,output);
            // Observe-only rumble/force-feedback counters for every launch mode.
            ffbobserver::Install(gameBase,output);
            // Flashback monitor sampling for every launch mode; in -benchmarkAttract
            // (direct) launches the benchmark mode word is cleared only while racing.
            // No game code is patched; Normal launches are never written.
            flashbackobserver::Install(gameBase,output);
            // Active() has already validated the admitted AVX executable. The
            // LOD module additionally pins every live body/table it consumes.
            opponentlod::bridge::Configure(output);
            const bool qualified=opponentlod::runtime::Prepare(gameBase,output,opponentlod::bridge::Transaction);
            Log("opponent LOD candidate prepared=%u; native execution acceptance pending",qualified);
        }
        drivingcontrols::Tick();
        ffbobserver::Tick();
        flashbackobserver::Tick();
    }
    processexit::Service();
    // An early window-close request has stopped XR at this restored boundary.
    // Forward native Presents without rearming input, eye rendering or overlays.
    if(runtimeStopped) {opponentlod::host::RevokeWorld();opponentlod::runtime::Tick(false);return realPresent(chain,sync,flags);}
#ifdef GRID2VR_ENABLE_XR_SESSION
    if(runtimeSession && xrProjection && !xrStopped && !engine::SessionFailed())
        cockpitanimation::Tick(engine::NativeWorldReady());
#endif
    flicker::Poll();
#ifdef GRID2VR_ENABLE_XR_SESSION
    if (runtimeSession && f==0) {
        ComPtr<ID3D11Device> device;
        if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device)))) InitializeXr(device.Get());
    }
#endif
    const unsigned windowState=WindowState(f+1);
    if(runtimeSession) {chaseobserver::Present(f+1,windowState);chasecamera::Present(f+1,windowState);}
    bool foregroundStable=foregroundHold.Observe(GetTickCount64(),windowState==5);
    const bool simulatedBackground=UnattendedStereoWindow(windowState,unattendedStereo,runtimeSession,recordedRequested,
#ifdef GRID2VR_ENABLE_XR_SESSION
        xrRequested
#else
        false
#endif
    );
    bool nativeWindowReady=backgroundHold.Observe(GetTickCount64(),backgroundplay::WindowEligible(windowState,backgroundplay::Enabled()) || simulatedBackground);
    static const bool normalLaunch=std::filesystem::is_regular_file(output/L"normal-launch.request");
    auto normalGarageState=GarageMenuState::Unavailable;
    bool normalLiveXr=false,focusedNormalViews=false;
#ifdef GRID2VR_ENABLE_XR_SESSION
    normalLiveXr=runtimeSession && normalLaunch && !recordedRequested && xrRequested && xrProjection && !xrStopped;
    if(normalLiveXr) {
        // Read current XR focus before evaluating the hold. The existing
        // dashboard suspension and per-frame view-provider guards still apply.
        PollOpponentXr();
        focusedNormalViews=xrSession.ReadyForViews(true);
        if(focusedNormalViews && (windowState==4 || windowState==5))
            normalGarageState=ReadGarageMenu(gameBase,[](uintptr_t at,void* value,size_t size) {
                return ReadMemory(reinterpret_cast<void*>(at),value,size);
            }).state;
    }
#endif
    const bool nativeWorldReady=normalLiveXr && engine::NativeWorldReady();
    const bool normalXrEligible=NormalXrRenderWindow(windowState,runtimeSession,normalLaunch,recordedRequested,
        normalLiveXr,focusedNormalViews,normalGarageState==GarageMenuState::Active,nativeWorldReady);
    bool normalXrStable=normalXrHold.Observe(GetTickCount64(),normalXrEligible);
    bool renderWindowReady=nativeWindowReady || normalXrStable;
    backgroundplay::ObserveWindow(windowState);
    dlccockpit::Present();
    LARGE_INTEGER now{},frequency{}; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&frequency);
    static LONGLONG previous{};
    const double ms=previous ? 1000.0*(now.QuadPart-previous)/frequency.QuadPart : 0;
    previous=now.QuadPart;
    static std::ofstream csv;
    static bool capturePending=false;
    static bool engineInstallPending=false,engineInstallRefused=false;
    static uint64_t nextEngineInstall{};
    if (f==0) { if (!runtimeSession) { csv.open(output/L"frames.csv");csv << "frame,interval_ms,draws,dispatches,private_bytes,working_set_bytes,sync\n"; } Describe(chain); Stack("Present"); }
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb=sizeof(memory);
    if (!runtimeSession) {
        GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory));
        csv << f << ',' << ms << ',' << draws.exchange(0) << ',' << dispatches.exchange(0) << ',' << memory.PrivateUsage << ',' << memory.WorkingSetSize << ',' << sync << '\n';
    } else { draws=0;dispatches=0; }
    if (f%120==0) {
        csv.flush();
        std::error_code error;
        // Executable startup processing continues beyond device creation.
        // Wait for actual race/visibility readiness before modifying native code.
        // This also avoids treating a loading screen as input-test readiness.
        const bool nativeLaunch=std::filesystem::is_regular_file(output/L"race-launch.request") ||
            std::filesystem::is_regular_file(output/L"normal-launch.request");
        const bool localRaceReady=nativeLaunch && NormalLocalRaceReady(gameBase);
        static bool nativeHooksInstalled=false;
        // Desktop player-only sessions have neither XR nor capture startup.
        // Establish owning-thread cleanup before installing any native hooks.
        if(runtimeSession && !runtimeStopped && !nativeHooksInstalled && EnsureExitHandler()) {
            uintptr_t playback{},parent{},table{},begin{},end{},car{};
            const auto readPointer=[](uintptr_t address,uintptr_t& value) {
                return ReadMemory(reinterpret_cast<void*>(address),&value,sizeof(value)) && value>=0x10000 && value<=UINTPTR_MAX-0x10000;
            };
            const bool replayReady=readPointer(gameBase+0xe61010,playback) && readPointer(playback+0xa8,parent) &&
               readPointer(parent,table) && table==gameBase+0xd1266c &&
               readPointer(parent+0xa8,begin) && readPointer(parent+0xac,end) && end-begin==4 && readPointer(begin,car);
            if(nativeLaunch?localRaceReady:(replayReady || NormalSoloRaceReady(gameBase))) {
                backgroundplay::Install(gameBase,output);
                chaseobserver::Install();
                chasecamera::Install();
                // The old input experiment is specific to recorded playback.
                // Normal entry uses the game's own local controller unchanged.
                if(!nativeLaunch && replayReady) playercontrol::Install(gameBase,output);
                nativeHooksInstalled=true;Log("session native hooks reached %s readiness frame=%llu",localRaceReady?"local_race":replayReady?"solo_replay":"solo_race",f+1);
            }
        }
        if (std::filesystem::remove(output/L"capture.request",error)) {
            RecordStartupPhase(1,f+1);
            // Prepare once at a requested Present boundary. Native installation
            // can wait for the live main object without losing this request.
            if (!engineContext) {
                ComPtr<ID3D11Device> device;
                if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device)))) device->GetImmediateContext(&engineContext);
                const bool exitReady=EnsureExitHandler();
                if (!exitReady) Log("session native startup refused reason=exit_handler");
                captureStageTargets=std::filesystem::exists(output/L"stage-target-capture.request");
                traceReplayWork=std::filesystem::exists(output/L"replay-work.request") && std::filesystem::exists(output/L"replay-once.request");
                traceReplaySamples=traceReplayWork && std::filesystem::exists(output/L"replay-samples.request");
                verifyFreshReplayColor=!runtimeSession && traceReplayWork && std::filesystem::exists(output/L"fresh-replay-color.request");
                retainEyeTextures=traceReplayWork && std::filesystem::exists(output/L"eye-textures.request");
                repeatAoDraw=traceReplayWork && std::filesystem::exists(output/L"repeat-ao.request");
                repeatShadowDraw=traceReplayWork && std::filesystem::exists(output/L"repeat-shadow.request");
                repeatShadowBindingControl=repeatShadowDraw && std::filesystem::exists(output/L"normalize-shadow-bindings.request");
                replaytrace::Configure(output,traceReplayWork,realDispatch);
                nativeUiWitnessEnabled=runtimeSession && replaytrace::ScreenUiEnabled() &&
                    std::filesystem::exists(output/L"normal-launch.request") &&
                    std::filesystem::exists(output/L"frontend-scene-audit.request");
                if (nativeUiWitnessEnabled) nativeUiWitness.Diagnose(output);
                lateUiEnabled=runtimeSession && replaytrace::ScreenUiEnabled() &&
                    std::filesystem::exists(output/L"normal-launch.request") &&
                    std::filesystem::exists(output/L"garage-late-ui.request");
                if (lateUiEnabled) lateUi.Diagnose(output);
                recordedRequested=std::filesystem::exists(output/L"recorded-views.request");
                manualStereoStart=std::filesystem::exists(output/L"wait-stereo-start.request");
                if (recordedRequested) {
                    unsigned count{};std::ifstream countFile(output/L"eye-sequence.request");countFile>>count;
                    std::ifstream input(output/L"recorded-views.request");
                    const bool valid=ReadSceneViews(input,count,recordedViews);
                    repeatRecordedViews=valid && runtimeSession && unattendedStereo &&
                        std::filesystem::is_regular_file(output/L"flicker-observation.request") &&
                        std::filesystem::is_regular_file(output/L"tracked-visibility.request") &&
                        !std::filesystem::exists(output/L"xr-session.request") &&
                        !std::filesystem::exists(output/L"xr-projection.request");
                    Log("recorded views loaded=%u count=%zu",valid,recordedViews.size());
                    if(repeatRecordedViews)Log("recorded views repeat=1 bounded_by_diagnostic_worker=1");
                }
                if(std::filesystem::exists(output/L"mirror-preview.request")) {
                    const bool allowed=runtimeSession && retainEyeTextures &&
                        (HeadsetVisibilityCandidate(output) || (unattendedStereo && recordedRequested &&
                        std::filesystem::exists(output/L"tracked-visibility.request") &&
                        !std::filesystem::exists(output/L"xr-session.request") &&
                        !std::filesystem::exists(output/L"xr-projection.request")));
                    mirrorPreview=allowed && eyeMirror.Initialize(device.Get());
                    Log("preview mirror requested=1 initialized=%u",mirrorPreview);
                }
#ifdef GRID2VR_ENABLE_XR_SESSION
                InitializeXr(device.Get());
#else
                if (std::filesystem::exists(output/L"xr-session.request")) Log("openxr session refused reason=not_built");
#endif
            }
            engineInstallPending=true;
        }
        if (runtimeSession && !runtimeStopped) {
            if (std::filesystem::remove(output/L"runtime-stop.request",error)) {
                Log("session stop requested source=file frame=%llu",f+1);
                playercontrol::Stop();
                backgroundplay::Stop();
                dlccockpit::Stop();
                cockpitanimation::Stop();
                opponentlod::runtime::Stop();
                chaseobserver::Stop();
                chasecamera::Stop();
                if (engine::Continuous() && !engine::StopSession()) engine::FailSession();
                runtimeStopped=true;
            }
#ifdef GRID2VR_ENABLE_XR_SESSION
            if (std::filesystem::remove(output/L"runtime-recenter.request",error) && !runtimeStopped && !engine::SessionFailed()) {
                Log("session recenter requested source=file frame=%llu",f+1);
                if (engine::Continuous() && !engine::SuspendSession()) engine::FailSession();
                xrRecentered=false;
                xrSession.ResetScreenAnchor();
            }
#endif
        }
    }
    if (engineInstallPending && !runtimeStopped && !engine::SessionFailed() && GetTickCount64()>=nextEngineInstall) {
        const auto result=engine::Install(gameBase,output,StageBoundary,ProvideViews,CompleteViews,ResetEyeHistory,CancelEyeHistory);
        capturePending=result==engine::InstallResult::Installed;
        engineInstallPending=runtimeSession && result==engine::InstallResult::Waiting;
        engineInstallRefused=result==engine::InstallResult::Refused;
        // Discovery can run during loading, but is bounded and never repeated
        // every frame. Only an absent live object is retryable before hooks.
        nextEngineInstall=GetTickCount64()+1000;
        if (capturePending) RecordStartupPhase(2,f+1);
        if (runtimeSession && engineInstallRefused) engine::FailSession();
    }
#ifdef GRID2VR_ENABLE_XR_SESSION
    const bool recenterReady=runtimeSession && !runtimeStopped && !engine::SessionFailed() &&
        xrProjection && !xrStopped && xrSession.Running() && !xrSession.Failed();
    recenter::Enable(recenterReady);
    if (recenter::Consume() && recenterReady) {
        Log("session recenter requested source=keyboard key=%u modifiers=%u frame=%llu",recenterBinding.key,recenterBinding.modifiers,f+1);
        if (engine::Continuous() && !engine::SuspendSession()) engine::FailSession();
        xrRecentered=false;
        xrSession.ResetScreenAnchor();
    }
#endif
    if (runtimeSession && engine::Continuous()) {
        if (!renderWindowReady && engine::SessionRendering()) engine::SuspendSession();
        capturePending=!runtimeStopped && !engineInstallPending && !engineInstallRefused && !engine::SessionFailed() && !engine::SessionRendering();
    }
    if (capturePending && manualStereoReady && !manualStereoReleased) {
        std::error_code error;
        if (std::filesystem::remove(output/L"stereo-start.request",error)) {
            manualStereoReleased=true;
            foregroundHold.Reset();
            backgroundHold.Reset();
            normalXrHold.Reset();
            foregroundStable=false;
            nativeWindowReady=false;
            normalXrStable=false;
            renderWindowReady=false;
            Log("manual_stereo_start frame=%llu released=1",f+1);
            WindowState(f+1,true);
        }
    }
#ifdef GRID2VR_ENABLE_XR_SESSION
    const bool manualStartReady=runtimeSession?renderWindowReady:!manualStereoStart || (manualStereoReleased && (!xrProjection || foregroundStable));
    if (xrRequested && !xrStopped) {
        if(!normalLiveXr) PollOpponentXr();
        if (runtimeSession && xrProjection && engine::SessionRendering() && !xrSession.Focused()) {
            if (!engine::SuspendSession()) engine::FailSession();
            capturePending=!runtimeStopped && !engine::SessionFailed();
        }
        if (xrSession.Failed() || (runtimeSession && (runtimeStopped || engine::SessionFailed()))) {
            if (xrSession.Failed() && runtimeSession) engine::FailSession();
            ShutdownOpponentXr();
            if (runtimeSession && xrSession.Failed()) engine::FailSession();
        }
    }
    // Keep the compositor frame loop alive during startup without arming eye
    // history before tracking/render eligibility. The warmup uses this frame
    // ID, so rendering may start only at a later Present boundary.
    bool xrWarmup=false;
    if (!runtimeSession && capturePending && xrProjection && xrRequested && !xrStopped && xrSession.Running() &&
        (!xrSession.ReadyForViews(runtimeSession) || !manualStartReady)) {
        xrSession.Sample(f+1);xrWarmup=true;
    }
    const bool xrReady=!xrRequested || (!xrStopped && xrSession.Running() &&
        (!xrProjection || (!xrWarmup && xrSession.ReadyForViews(runtimeSession))));
#else
    constexpr bool xrReady=true;
    const bool manualStartReady=runtimeSession?renderWindowReady:!manualStereoStart || manualStereoReleased;
#endif
    if (runtimeStopped && !stopReceipt.Reported()) {
        const bool reported=stopReceipt.Stop(output/L"runtime-stopped",engine::SessionFailed());
        Log("session stop acknowledged=%u failed=%u",reported,engine::SessionFailed());
    }
    const bool engineReady=capturePending && engine::ReadyForCapture();
    if (engineReady && manualStereoStart && !manualStereoReady &&
#ifdef GRID2VR_ENABLE_XR_SESSION
        (!xrRequested || (!xrStopped && xrSession.ReadyForViews()))
#else
        true
#endif
    ) {
        std::ofstream ready(output/L"stereo-ready.txt");
        ready << "pid=" << GetCurrentProcessId() << " frame=" << f+1 << '\n';ready.close();
        manualStereoReady=ready.good();
        Log("manual_stereo_start frame=%llu ready=%u",f+1,manualStereoReady);
        WindowState(f+1,true);
    }
    const bool recordedReady=!recordedRequested || repeatRecordedViews || recordedIndex<recordedViews.size();
    if (runtimeSession && engineContext) {
        const char* sceneWait=engine::CaptureWaitReason();
        const char* reason=engine::SessionFailed()?"failed":runtimeStopped?"stopped":engineInstallRefused?"engine_install_refused":engineInstallPending?"engine_object":
            engine::SessionRendering()?"rendering":!recordedReady?"recorded_input_ended":
            !renderWindowReady?"foreground":sceneWait?sceneWait:!xrReady?"xr_views":"ready";
        static std::string previousReason;
        const bool changed=previousReason!=reason;
        if (changed || f%120==0) {
            std::ofstream status(output/L"runtime-readiness.json",std::ios::trunc);
            status << "{\"frame\":" << f+1 << ",\"reason\":\"" << reason
                << "\",\"sceneWait\":\"" << (sceneWait?sceneWait:"ready")
                << "\",\"windowState\":" << windowState << ",\"foregroundStable\":" << (foregroundStable?"true":"false")
                << ",\"renderWindowReady\":" << (renderWindowReady?"true":"false") << ",\"backgroundPlay\":" << (backgroundplay::Enabled()?"true":"false")
                << ",\"nativeWindowReady\":" << (nativeWindowReady?"true":"false")
                << ",\"nativeWorldReady\":" << (nativeWorldReady?"true":"false")
                << ",\"garageState\":\"" << (normalGarageState==GarageMenuState::Active?"active":normalGarageState==GarageMenuState::Inactive?"inactive":"unavailable")
                << "\",\"normalXrEligible\":" << (normalXrEligible?"true":"false") << ",\"normalXrStable\":" << (normalXrStable?"true":"false")
                << ",\"normalXrFocusedViews\":" << (focusedNormalViews?"true":"false")
                << ",\"xrReady\":" << (xrReady?"true":"false") << ",\"recordedReady\":" << (recordedReady?"true":"false") << "}\n";
        }
        if (changed) {
            Log("session readiness frame=%llu reason=%s scene=%s window=%u foreground_stable=%u xr_ready=%u recorded_ready=%u",
                f+1,reason,sceneWait?sceneWait:"ready",windowState,foregroundStable,xrReady,recordedReady);
            previousReason=reason;
        }
    }
    if (runtimeSession && engineReady && xrReady && manualStartReady && recordedReady) {
        if (engine::ResumeSession()) capturePending=false;
    } else if (!runtimeSession && engineReady && xrReady && manualStartReady) {
        const auto count=engine::CaptureFrameCount();
        cameraSamples=0; drawSamples=0; captureUntil=f+count;
        engine::CaptureUntil(f+count);
        profile::Start(output,f+1,f+count);
        Describe(chain); Screenshot(chain,f); Stack("requested capture");
        capturePending=false;
    }
#ifdef GRID2VR_ENABLE_XR_SESSION
    if (xrRequested && !xrStopped) {
        if (xrSession.Running() && !xrProjection) xrSession.Sample(f+1);
        if (runtimeSession && xrProjection && xrSession.Running() && !engine::SessionRendering() &&
            !runtimeStopped && !engine::SessionFailed() && xrSession.LastFrame()<f+1) {
            // Native menus/loading are one shared-eye quad. Copy before the
            // optional stereo desktop mirror, retaining no backbuffer reference.
            ComPtr<ID3D11Texture2D> back;
            if (SUCCEEDED(chain->GetBuffer(0,IID_PPV_ARGS(&back)))) xrSession.PresentScreen(f+1,back.Get());
        }
        const auto end=captureUntil.load();
        if (xrSession.Failed() || (end && f>=end)) ShutdownOpponentXr();
    }
#endif
    FrontendPreview(chain,f);
    MirrorPreview(chain,f);
    // Polls, recenter/suspension, stop, shutdown and preview decisions are final.
    PublishOpponentWorld();
    const auto hr=realPresent(chain,sync,flags);
    if (FAILED(hr)) {opponentlod::host::RevokeWorld();Log("Present failed hr=0x%08lx",hr);}
    // Wait for all sampled render callbacks to return, not just the first PPM.
    // This marker ends a probe; rejected hooks or missing snapshots still fail
    // their separate evidence checks.
    static bool probeComplete=false;
    static const bool shortProbe=std::filesystem::exists(output/L"short-probe.request");
    const auto end=captureUntil.load();
    if (shortProbe && !probeComplete && end && f>=end && SUCCEEDED(hr)) {
        csv.flush();
        Log("probe capture window completed frame=%llu end=%llu",f,end);
        std::ofstream marker(output/L"probe-complete");
        marker << "frame=" << f << " capture_end=" << end << '\n';
        probeComplete=marker.good();
    }
    return hr;
}

void PrepareSwapchainChange(bool nativeResize) {
    opponentlod::host::RevokeWorld();
    // The native resize event is queued after DXGI returns. Its owned callback
    // corrects the video size without modifying integrity-checked game code.
    if (nativeResize && runtimeSession && std::filesystem::is_regular_file(output/L"scene-size.request") && !frontendvideo::Install(gameBase,output))
        Log("session frontend_video_reset callback refused");
    nativeUiWitness.End(true);
    FinishLateUi(false);lateUi.Reset();
    previewFrame.Reset();
    if (runtimeSession && engine::Continuous()) {
        const bool suspended=engine::SuspendForSwapchainChange();
        Log("session swapchain prepared=%u frame=%llu",suspended,frame.load());
        if (!suspended) engine::FailSession();
    }
}
HRESULT STDMETHODCALLTYPE Resize(IDXGISwapChain* chain,UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags) {
    opponentlod::host::RevokeWorld();
    const auto incomingWidth=width,incomingHeight=height;
    const auto change=sceneresolution::PrepareResize(gameBase,reinterpret_cast<uintptr_t>(_ReturnAddress()),chain,width,height,format);
    PrepareSwapchainChange(bool(change));
    Log("ResizeBuffers requested=%ux%u effective=%ux%u format=%u enforced=%u",incomingWidth,incomingHeight,width,height,format,bool(change));
    const auto hr=realResize(chain,count,width,height,format,flags);
    sceneresolution::CompleteResize(change,hr);
    sceneresolution::ObserveSwapchain(chain);
    Describe(chain);return hr;
}

void AttachSwapchain(IDXGISwapChain* chain) {
    Describe(chain);
    Hook(chain,8,reinterpret_cast<void*>(Present),realPresent);
    Hook(chain,13,reinterpret_cast<void*>(Resize),realResize);
}

HRESULT STDMETHODCALLTYPE CreateSwapchain(IDXGIFactory* factory,IUnknown* device,DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** chain) {
    opponentlod::host::RevokeWorld();
    Stack("CreateSwapChain");
    const auto change=sceneresolution::PrepareCreate(gameBase,output,device,desc,chain,reinterpret_cast<uintptr_t>(_ReturnAddress()));
    if(change && change.recreation)PrepareSwapchainChange(true);
    const auto hr=realSwap(factory,device,desc,chain);
    Log("CreateSwapChain hr=0x%08lx",hr);
    sceneresolution::CompleteCreate(change,hr,SUCCEEDED(hr) && chain?*chain:nullptr);
    if (SUCCEEDED(hr) && chain && *chain) { std::lock_guard lock(attachMutex); AttachSwapchain(*chain); }
    return hr;
}

void Shader(const void* bytes,SIZE_T length,const char* stage,ID3D11DeviceChild* shader) {
    uint64_t hash=14695981039346656037ull;
    for (SIZE_T i=0;i<length;++i) { hash^=static_cast<const unsigned char*>(bytes)[i]; hash*=1099511628211ull; }
    replaytrace::ShaderIdentity(shader,hash);
    if (runtimeSession) return; // Fingerprints are required; disassembly/readback is diagnostic-only.
    const bool workRequested=std::filesystem::exists(output/L"replay-work.request");
    if (workRequested) Log("shader object=%p stage=%s hash=%016llx",shader,stage,hash);
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(bytes,length,IID_PPV_ARGS(&reflection)))) return;
    CameraBinding camera{};
    if (FindCameraBinding(reflection.Get(),camera)) shader->SetPrivateData(cameraBindingId,sizeof(camera),&camera);
    std::lock_guard lock(shaderMutex);
    if (shaders.size()>=(workRequested?4096u:512u) || !shaders.insert(hash).second) return;
    char name[64]{}; sprintf_s(name,"%s-%016llx",stage,hash);
    // Preserve alternate final-pass code for diagnosis only. Reflection is not
    // authorization to modify an unknown shader; MotionBlurControl still uses
    // its exact allowlist. Extra disassembly is capped at 2 MiB per process.
    D3D11_SHADER_INPUT_BIND_DESC postBinding{};D3D11_SHADER_BUFFER_DESC postBuffer{};
    D3D11_SHADER_VARIABLE_DESC velocity{};
    auto* globals=reflection->GetConstantBufferByName("$Globals");
    const bool postprocess=!strcmp(stage,"ps") &&
        SUCCEEDED(reflection->GetResourceBindingDescByName("$Globals",&postBinding)) &&
        postBinding.Type==D3D_SIT_CBUFFER && postBinding.BindPoint==0 &&
        SUCCEEDED(globals->GetDesc(&postBuffer)) && postBuffer.Size==1296 &&
        SUCCEEDED(globals->GetVariableByName("velocityDecodingParams")->GetDesc(&velocity)) &&
        velocity.StartOffset==0 && velocity.Size==16;
    const bool knownCode=strcmp(stage,"cs")==0 || hash==0x789227e243eff8eaull || hash==0x3f322761392bebfdull ||
                         hash==0xb4b3bc4b691a6570ull || hash==0x9fdaa9230df38251ull || hash==0x5e7f3bd5d1d1a1b5ull ||
                         hash==0xe8bc75f0f617aa9eull || hash==0x158f4162b8000a3full ||
                         hash==0x922a49bc5dcd4775ull || hash==0x287dc592430a7323ull ||
                         hash==0x4fa9edcd16d3b26cull || hash==0x4b2b0f44adfbf5bbull ||
                         hash==0x6e44f31d1ebda08dull || hash==0xed4ddaac90aff677ull ||
                         hash==0xb02f3b98b3596660ull || hash==0xcfc3cf441f08e879ull;
    static size_t extraAssemblyBytes{};
    if (workRequested && (knownCode || (postprocess && extraAssemblyBytes<2*1024*1024))) {
        ComPtr<ID3DBlob> assembly;
        if (SUCCEEDED(D3DDisassemble(bytes,length,0,nullptr,&assembly)) &&
            (knownCode || assembly->GetBufferSize()<=2*1024*1024-extraAssemblyBytes)) {
            std::ofstream code(output/(std::string(name)+".asm"),std::ios::binary);
            code.write(static_cast<const char*>(assembly->GetBufferPointer()),assembly->GetBufferSize());
            if (!knownCode) extraAssemblyBytes+=assembly->GetBufferSize();
        }
    }
    D3D11_SHADER_DESC desc{}; reflection->GetDesc(&desc);
    std::ofstream file(output/L"shaders.txt",std::ios::app);
    file << name << " bytes=" << length << '\n';
    for (UINT i=0;i<desc.ConstantBuffers;++i) {
        auto* buffer=reflection->GetConstantBufferByIndex(i); D3D11_SHADER_BUFFER_DESC cb{};
        if (FAILED(buffer->GetDesc(&cb))) continue;
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        reflection->GetResourceBindingDescByName(cb.Name,&binding);
        file << "  " << cb.Name << " slot=" << binding.BindPoint << " bytes=" << cb.Size << '\n';
        for (UINT j=0;j<cb.Variables;++j) {
            D3D11_SHADER_VARIABLE_DESC variable{};
            if (SUCCEEDED(buffer->GetVariableByIndex(j)->GetDesc(&variable)))
                file << "    " << variable.Name << " offset=" << variable.StartOffset << " size=" << variable.Size
                     << " used=" << ((variable.uFlags&D3D_SVF_USED)!=0) << '\n';
        }
    }
}

using BufferFn=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const D3D11_BUFFER_DESC*,const D3D11_SUBRESOURCE_DATA*,ID3D11Buffer**);
BufferFn realBuffer{};
using Texture2DFn=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const D3D11_TEXTURE2D_DESC*,const D3D11_SUBRESOURCE_DATA*,ID3D11Texture2D**);
Texture2DFn realTexture2D{};
HRESULT STDMETHODCALLTYPE CreateTexture2D(ID3D11Device* device,const D3D11_TEXTURE2D_DESC* desc,const D3D11_SUBRESOURCE_DATA* data,ID3D11Texture2D** texture) {
    const auto hr=realTexture2D(device,desc,data,texture);
    static std::atomic<unsigned> failures{};
    if (runtimeSession && FAILED(hr) && desc && failures.fetch_add(1)<16) {
        Log("session texture_create failed hr=0x%08lx size=%ux%u format=%u samples=%u quality=%u mips=%u array=%u bind=0x%x usage=%u removed=0x%08lx caller=%p",
            hr,desc->Width,desc->Height,desc->Format,desc->SampleDesc.Count,desc->SampleDesc.Quality,
            desc->MipLevels,desc->ArraySize,desc->BindFlags,desc->Usage,device->GetDeviceRemovedReason(),_ReturnAddress());
    }
    if(SUCCEEDED(hr) && texture)focusCockpit.TextureCreated(device,desc,data,*texture,realTexture2D,uintptr_t(_ReturnAddress())-gameBase);
    msaaaudit::TextureCreated(desc,hr,SUCCEEDED(hr) && texture?*texture:nullptr,uintptr_t(_ReturnAddress()));return hr;
}
FocusCockpit::ViewFn realResourceView{};
HRESULT STDMETHODCALLTYPE CreateResourceView(ID3D11Device* device,ID3D11Resource* resource,const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,ID3D11ShaderResourceView** view) {
    return focusCockpit.CreateView(device,resource,desc,view,realResourceView);
}
HRESULT STDMETHODCALLTYPE CreateBuffer(ID3D11Device* device,const D3D11_BUFFER_DESC* desc,const D3D11_SUBRESOURCE_DATA* data,ID3D11Buffer** buffer) {
    const auto hr=realBuffer(device,desc,data,buffer);
    if (SUCCEEDED(hr) && flatMenuUi.Enabled() && desc && data && buffer && *buffer &&
        desc->BindFlags==D3D11_BIND_CONSTANT_BUFFER && desc->ByteWidth==320)
        ScreenUi::Upload(*buffer,data->pSysMem,desc->ByteWidth);
    if (SUCCEEDED(hr) && desc && data && buffer && *buffer)
        spotlightwitness::Upload(*buffer,data->pSysMem,desc->ByteWidth);
    instancecapacity::BufferCreated(desc,hr,SUCCEEDED(hr) && buffer?*buffer:nullptr,uintptr_t(_ReturnAddress()));return hr;
}
HRESULT STDMETHODCALLTYPE CreateVS(ID3D11Device* d,const void* b,SIZE_T n,ID3D11ClassLinkage* l,ID3D11VertexShader** s) {
    const auto hr=realVS(d,b,n,l,s); if (SUCCEEDED(hr) && s && *s) Shader(b,n,"vs",*s); return hr;
}
HRESULT STDMETHODCALLTYPE CreateLayout(ID3D11Device* d,const D3D11_INPUT_ELEMENT_DESC* e,UINT count,const void* b,SIZE_T n,ID3D11InputLayout** layout) {
    const auto hr=realLayout(d,e,count,b,n,layout);
    if(SUCCEEDED(hr) && layout && *layout)flicker::RememberLayout(*layout,e,count);
    return hr;
}
HRESULT STDMETHODCALLTYPE CreatePS(ID3D11Device* d,const void* b,SIZE_T n,ID3D11ClassLinkage* l,ID3D11PixelShader** s) {
    const auto hr=realPS(d,b,n,l,s); if (SUCCEEDED(hr) && s && *s) Shader(b,n,"ps",*s); return hr;
}
HRESULT STDMETHODCALLTYPE CreateCS(ID3D11Device* d,const void* b,SIZE_T n,ID3D11ClassLinkage* l,ID3D11ComputeShader** s) {
    const auto hr=realCS(d,b,n,l,s); if (SUCCEEDED(hr) && s && *s) Shader(b,n,"cs",*s); return hr;
}

struct Mapped { ID3D11DeviceContext* context; ID3D11Resource* resource; UINT subresource; const void* data; UINT bytes; bool camera; };
thread_local std::vector<Mapped> mappings;
HRESULT STDMETHODCALLTYPE Map(ID3D11DeviceContext* c,ID3D11Resource* resource,UINT sub,D3D11_MAP kind,UINT flags,D3D11_MAPPED_SUBRESOURCE* mapped) {
    const auto hr=realMap(c,resource,sub,kind,flags,mapped);
    const bool witness=flicker::Observing();
    const bool screenUi=replaytrace::ScreenUiEnabled() || flatMenuUi.Enabled();
    const bool lighting=spotlightwitness::Enabled();
    const bool legacy=captureUntil.load()>frame.load() && (traceReplayWork || cameraSamples.load()<64);
    if (SUCCEEDED(hr) && mapped && kind!=D3D11_MAP_READ && (witness || legacy || screenUi || lighting)) {
        ComPtr<ID3D11Buffer> buffer;
        if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&buffer)))) {
            D3D11_BUFFER_DESC desc{}; buffer->GetDesc(&desc);
            // These are candidates only. The draw trace separately verifies the
            // shader's named camera binding; unused trailing fields may be absent.
            const bool camera=legacy && (desc.ByteWidth==320 || desc.ByteWidth==368);
            if ((desc.BindFlags&D3D11_BIND_CONSTANT_BUFFER) && (camera || traceReplayWork || witness || (screenUi && desc.ByteWidth==320) ||
                (lighting && spotlightwitness::TracksSize(desc.ByteWidth))) && desc.ByteWidth<=65536 && mappings.size()<32)
                mappings.push_back({c,resource,sub,mapped->pData,desc.ByteWidth,camera});
        }
    }
    return hr;
}
void STDMETHODCALLTYPE Unmap(ID3D11DeviceContext* c,ID3D11Resource* resource,UINT sub) {
    for (auto it=mappings.begin();it!=mappings.end();++it) {
        if (it->context==c && it->resource==resource && it->subresource==sub) {
            if (traceReplayWork) replaytrace::Upload(resource,it->data,it->bytes);
            else spotlightwitness::Upload(resource,it->data,it->bytes);
            if (flatMenuUi.Enabled() && (!traceReplayWork || !replaytrace::ScreenUiEnabled()))
                ScreenUi::Upload(resource,it->data,it->bytes);
            flicker::Upload(resource,it->data,it->bytes);
            const auto sample=it->camera?cameraSamples.fetch_add(1):64;
            if (sample<64) {
                const auto name="constant-"+std::to_string(frame.load())+"-"+std::to_string(sample)+".bin";
                std::ofstream file(output/name,std::ios::binary); file.write(static_cast<const char*>(it->data),it->bytes);
                Log("constant snapshot=%s context=%p resource=%p bytes=%u",name.c_str(),c,resource,it->bytes); Stack("constant Unmap");
            }
            mappings.erase(it); break;
        }
    }
    realUnmap(c,resource,sub);
}
void CameraDraw(ID3D11DeviceContext* c) {
    const auto f=frame.load();
    if (f>=captureUntil.load() || drawSamples.load()>=128) return;
    ComPtr<ID3D11VertexShader> shader; c->VSGetShader(&shader,nullptr,nullptr);
    CameraBinding camera{}; UINT bytes=sizeof(camera);
    if (!shader || FAILED(shader->GetPrivateData(cameraBindingId,&bytes,&camera))) return;
    ComPtr<ID3D11Buffer> buffer; c->VSGetConstantBuffers(camera.slot,1,&buffer);
    if (!buffer) return;
    const auto upload=cameraSamples.load();
    struct Previous { uint64_t frame{}; unsigned upload{}; ID3D11DeviceContext* context{}; ID3D11Buffer* buffer{}; };
    thread_local Previous previous;
    if (previous.frame==f && previous.upload==upload && previous.context==c && previous.buffer==buffer.Get()) return;
    previous={f,upload,c,buffer.Get()};
    if (drawSamples.fetch_add(1)>=128) return;
    D3D11_BUFFER_DESC desc{}; buffer->GetDesc(&desc);
    D3D11_VIEWPORT viewport{}; UINT count=1; c->RSGetViewports(&count,&viewport);
    ComPtr<ID3D11RenderTargetView> target; ComPtr<ID3D11DepthStencilView> depthTarget;
    c->OMGetRenderTargets(1,&target,&depthTarget);
    auto describe=[](ID3D11View* view) {
        D3D11_TEXTURE2D_DESC result{};
        if (view) {
            ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(resource.As(&texture))) texture->GetDesc(&result);
        }
        return result;
    };
    const auto color=describe(target.Get()),depth=describe(depthTarget.Get());
    const auto stamp=engine::CurrentCamera();
    const auto stage=engine::CurrentStage();
    Log("camera draw frame=%llu uploads_seen=%u context=%p shader=%p slot=%u resource=%p allocated=%u declared=%u used_end=%u viewport=%.0fx%.0f target=%p engine_camera=%llu engine_context=%p color=%ux%u/%u/%u depth=%ux%u/%u/%u render_stage=%llu render_slot=%u",
        f,upload,c,shader.Get(),camera.slot,buffer.Get(),desc.ByteWidth,camera.declaredBytes,camera.usedBytes,
        viewport.Width,viewport.Height,target.Get(),stamp.id,reinterpret_cast<void*>(stamp.context),
        color.Width,color.Height,color.Format,color.SampleDesc.Count,depth.Width,depth.Height,depth.Format,depth.SampleDesc.Count,stage.id,stage.slot);
    Stack("camera draw");
}
std::string sampleDraw[3];
void FreshReplayColor(ID3D11DeviceContext* c,const std::string& mark) {
    static bool attempted[3]{};
    static ComPtr<ID3D11RenderTargetView> originalTarget;
    static uint64_t originalPs{},originalVs{};
    const auto pass=engine::CurrentScene().pass;
    if (!verifyFreshReplayColor || attempted[pass] || mark.find("-slot6-")==std::string::npos) return;
    ID3D11RenderTargetView* views[8]{}; c->OMGetRenderTargets(8,views,nullptr);
    ComPtr<ID3D11RenderTargetView> target; target.Attach(views[0]);
    bool extra=false;
    for (unsigned i=1;i<8;++i) if (views[i]) { extra=true; views[i]->Release(); }
    ComPtr<ID3D11Resource> resource; if (target) target->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc{};
    if (resource && SUCCEEDED(resource.As(&texture))) texture->GetDesc(&desc);
    if (desc.Format!=DXGI_FORMAT_R10G10B10A2_UNORM) return;
    // Never skip an earlier color draw and clear geometry it already produced.
    attempted[pass]=true;
    ComPtr<ID3D11PixelShader> ps; ComPtr<ID3D11VertexShader> vs;
    c->PSGetShader(&ps,nullptr,nullptr); c->VSGetShader(&vs,nullptr,nullptr);
    const auto psHash=replaytrace::ShaderFingerprint(ps.Get()),vsHash=replaytrace::ShaderFingerprint(vs.Get());
    CameraBinding camera{};UINT cameraBytes=sizeof(camera);
    const bool sceneShader=vs && SUCCEEDED(vs->GetPrivateData(cameraBindingId,&cameraBytes,&camera)) && camera.slot==3 && camera.usedBytes>=236;
    const bool knownSky=psHash==0xb4b3bc4b691a6570ull && vsHash==0x74f717e243dcef18ull;
    // The first visible material changes under bridges. Seed before that first
    // color draw, require a native camera shader (or the known sky pair), then
    // require the exact same shader pair and target for both replays.
    const bool shaderIdentity=psHash && vsHash && (knownSky || sceneShader) &&
        (!pass || (psHash==originalPs && vsHash==originalVs));
    D3D11_RENDER_TARGET_VIEW_DESC vd{}; if (target) target->GetDesc(&vd);
    if (!target || extra || !shaderIdentity ||
        vd.Format!=desc.Format || vd.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice!=0 || desc.SampleDesc.Count!=1 ||
        desc.MipLevels!=1 || desc.ArraySize!=1 || static_cast<uint64_t>(desc.Width)*desc.Height>3840ull*2160 ||
        c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || (pass && target.Get()!=originalTarget.Get())) {
        Log("fresh_color name=%s pass=%u applied=0 refused=target ps=%016llx vs=%016llx identity=%u",mark.c_str(),pass,psHash,vsHash,shaderIdentity); return;
    }
    if (!pass) {originalTarget=target;originalPs=psHash;originalVs=vsHash;}
    // Different incoming colors expose untouched pixels or reuse of the first
    // image. This one-shot diagnostic is not an eye-rendering implementation.
    const FLOAT seeds[3][4]{{1,0,1,1},{0,1,0,1},{0,0,1,1}};
    c->ClearRenderTargetView(target.Get(),seeds[pass]);
    const bool saved=CaptureTexture(texture.Get(),mark+"-fresh-seed");
    Log("fresh_color name=%s pass=%u applied=1 saved=%u seed=%s ps=%016llx vs=%016llx identity=%u",mark.c_str(),pass,saved,pass==2?"blue":pass?"green":"magenta",psHash,vsHash,shaderIdentity);
}
struct ProducerRepeat {
    ComPtr<ID3D11Texture2D> target,before,after;
    const char* kind{};
};
ProducerRepeat PrepareProducerRepeat(ID3D11DeviceContext* c,const std::string& mark,bool shadow,UINT n,UINT start,INT base=0) {
    static bool attempted[2][3]{};
    const auto pass=engine::CurrentScene().pass;
    if (!(shadow ? repeatShadowDraw:repeatAoDraw) || attempted[shadow][pass] || (pass && !(shadow && repeatShadowBindingControl)) || mark.empty() ||
        mark.find("-slot0-")==std::string::npos || n!=(shadow ? 4u:36u) || start || base) return {};
    ComPtr<ID3D11PixelShader> ps; ComPtr<ID3D11VertexShader> vs;
    c->PSGetShader(&ps,nullptr,nullptr); c->VSGetShader(&vs,nullptr,nullptr);
    if (replaytrace::ShaderFingerprint(ps.Get())!=(shadow ? 0x158f4162b8000a3full:0xe8bc75f0f617aa9eull) ||
        replaytrace::ShaderFingerprint(vs.Get())!=(shadow ? 0x4b2b0f44adfbf5bbull:0x4fa9edcd16d3b26cull)) return {};
    attempted[shadow][pass]=true;
    const char* kind=shadow ? "shadow":"ao";
    ComPtr<ID3D11RenderTargetView> target; ComPtr<ID3D11DepthStencilView> depth;
    ID3D11RenderTargetView* targets[8]{}; c->OMGetRenderTargets(8,targets,&depth);
    target.Attach(targets[0]); bool extraTargets=false;
    for (UINT i=1;i<8;++i) if (targets[i]) { extraTargets=true; targets[i]->Release(); }
    bool depthWrites=false;
    if (depth) {
        ComPtr<ID3D11DepthStencilState> state; UINT reference{}; c->OMGetDepthStencilState(&state,&reference);
        D3D11_DEPTH_STENCIL_DESC ds{};
        if (!state) depthWrites=true;
        else {
            state->GetDesc(&ds);
            auto writes=[](const D3D11_DEPTH_STENCILOP_DESC& face) {
                return face.StencilFailOp!=D3D11_STENCIL_OP_KEEP || face.StencilDepthFailOp!=D3D11_STENCIL_OP_KEEP ||
                    face.StencilPassOp!=D3D11_STENCIL_OP_KEEP;
            };
            depthWrites=(ds.DepthEnable && ds.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ZERO) ||
                (ds.StencilEnable && ds.StencilWriteMask && (writes(ds.FrontFace) || writes(ds.BackFace)));
        }
    }
    ComPtr<ID3D11Predicate> predicate; BOOL value{}; c->GetPredication(&predicate,&value);
    ComPtr<ID3D11GeometryShader> gs; ComPtr<ID3D11HullShader> hs; ComPtr<ID3D11DomainShader> ds;
    c->GSGetShader(&gs,nullptr,nullptr); c->HSGetShader(&hs,nullptr,nullptr); c->DSGetShader(&ds,nullptr,nullptr);
    ID3D11Buffer* streams[4]{}; c->SOGetTargets(4,streams);
    bool streamOutput=false; for (auto* stream:streams) if (stream) { streamOutput=true; stream->Release(); }
    ID3D11UnorderedAccessView* outputs[8]{}; c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,outputs);
    bool unordered=false; for (auto* binding:outputs) if (binding) { unordered=true; binding->Release(); }
    if (!target || extraTargets || depthWrites || (!shadow && depth) || predicate || gs || hs || ds || streamOutput || unordered ||
        c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) { Log("%s_repeat name=%s refused=pipeline",kind,mark.c_str()); return {}; }
    ComPtr<ID3D11Resource> resource; target->GetResource(&resource);
    ProducerRepeat repeat; repeat.kind=kind; if (FAILED(resource.As(&repeat.target))) return {};
    D3D11_TEXTURE2D_DESC desc{}; repeat.target->GetDesc(&desc);
    if (desc.Format!=(shadow ? DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R8G8_UNORM) || desc.SampleDesc.Count!=1 || desc.MipLevels!=1 || desc.ArraySize!=1 ||
        static_cast<uint64_t>(desc.Width)*desc.Height>3840ull*2160) { Log("%s_repeat name=%s refused=texture",kind,mark.c_str()); return {}; }
    desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=0; desc.CPUAccessFlags=0; desc.MiscFlags=0;
    ComPtr<ID3D11Device> device; c->GetDevice(&device);
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&repeat.before)) || FAILED(device->CreateTexture2D(&desc,nullptr,&repeat.after))) {
        Log("%s_repeat name=%s refused=allocation",kind,mark.c_str()); return {};
    }
    c->CopyResource(repeat.before.Get(),repeat.target.Get());
    return repeat;
}
void CompleteProducerRepeat(ID3D11DeviceContext* c,const std::string& mark,ProducerRepeat& repeat,bool shadow,UINT n,UINT start,INT base=0) {
    if (!repeat.before || !repeat.after) return;
    c->CopyResource(repeat.after.Get(),repeat.target.Get());
    CaptureComputeResource(c,repeat.after.Get(),mark+"-repeat-original");
    c->CopyResource(repeat.target.Get(),repeat.before.Get());
    const bool bindingControl=shadow && repeatShadowBindingControl;
    const bool nativeApplied=!bindingControl || replaytrace::ShadowControlBindings(c,true);
    // The fingerprinted shader writes one color target. Other output bindings
    // and depth/stencil writes are refused, so only this target needs restoring.
    if (nativeApplied) {
        if (shadow) realDraw(c,n,start); else realDrawIndexed(c,n,start,base);
        CaptureComputeResource(c,repeat.target.Get(),mark+"-repeat-control");
    }
    const bool bindingsRestored=!bindingControl || replaytrace::ShadowControlBindings(c,false);
    c->CopyResource(repeat.target.Get(),repeat.after.Get());
    CaptureComputeResource(c,repeat.target.Get(),mark+"-repeat-restored");
    Log("%s_repeat name=%s repeated=%u restore_requested=1 injected_draws=%u binding_control=%u native_applied=%u normalized_restored=%u",
        repeat.kind,mark.c_str(),nativeApplied,nativeApplied,bindingControl,nativeApplied,bindingsRestored);
}
void CaptureSamples(ID3D11DeviceContext* c,const std::string& name,const char* phase) {
    if (name.empty() || sampleDraw[engine::CurrentScene().pass]!=name) return;
    ComPtr<ID3D11RenderTargetView> view; c->OMGetRenderTargets(1,&view,nullptr);
    ComPtr<ID3D11Resource> resource; if (view) view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture; if (!resource || FAILED(resource.As(&texture))) return;
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    static MsaaReadback reader; bool restored{};
    const auto hr=reader.Read(c,texture.Get(),realDispatch,[&](ID3D11Texture2D* packed,UINT sample) {
        CaptureComputeResource(c,packed,name+"-"+phase+"-msaa"+std::to_string(sample));
    },restored);
    Log("msaa_readback name=%s phase=%s hr=%08lx restored=%u width=%u height=%u format=%u samples=%u quality=%u",
        name.c_str(),phase,hr,restored,desc.Width,desc.Height,desc.Format,desc.SampleDesc.Count,desc.SampleDesc.Quality);
}
void CaptureWorkMark(ID3D11DeviceContext* c,const std::string& name) {
    if (name.empty() || replaytrace::Compact()) return;
    if (traceReplaySamples && name.find("-slot6-")!=std::string::npos &&
        name!=sampleDraw[engine::CurrentScene().pass] && !name.ends_with("-draw1")) return;
    CaptureSamples(c,name,"after");
    ComPtr<ID3D11RenderTargetView> view; c->OMGetRenderTargets(1,&view,nullptr);
    if (!view) { Log("work capture missing target name=%s",name.c_str()); return; }
    ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (name.find("-slot0-")!=std::string::npos) CaptureComputeResource(c,resource.Get(),name);
    if (SUCCEEDED(resource.As(&texture))) CaptureTexture(texture.Get(),name);
}
void CaptureDynamicTexture(ID3D11DeviceContext* c,ID3D11ShaderResourceView* view,const std::string& name) {
    if (!view) return;
    ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture; if (FAILED(resource.As(&texture))) return;
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    if (!(desc.BindFlags&(D3D11_BIND_RENDER_TARGET|D3D11_BIND_DEPTH_STENCIL))) return;
    static std::set<std::pair<unsigned,ID3D11Resource*>> seen;
    if (seen.size()>=128 || !seen.insert({engine::CurrentScene().pass,resource.Get()}).second) return;
    UINT bpp{};
    switch(desc.Format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: bpp=16; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R32G32_FLOAT: bpp=8; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R16G16_UNORM: bpp=4; break;
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R8G8_UNORM: bpp=2; break;
    case DXGI_FORMAT_R8_UNORM: bpp=1; break;
    }
    uint64_t bytes{};
    for (UINT mip=0;mip<desc.MipLevels;++mip) bytes+=static_cast<uint64_t>(std::max(1u,desc.Width>>mip))*std::max(1u,desc.Height>>mip)*bpp*desc.ArraySize;
    static uint64_t totalBytes{};
    if (!bpp || desc.SampleDesc.Count!=1 || bytes>64*1024*1024 || totalBytes+bytes>256*1024*1024) {
        Log("dynamic texture name=%s refused=1 format=%u samples=%u bytes=%llu",name.c_str(),desc.Format,desc.SampleDesc.Count,bytes); return;
    }
    ComPtr<ID3D11Device> device; c->GetDevice(&device);
    auto copyDesc=desc; copyDesc.Usage=D3D11_USAGE_STAGING; copyDesc.BindFlags=0; copyDesc.MiscFlags=0; copyDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> copy;
    if (FAILED(device->CreateTexture2D(&copyDesc,nullptr,&copy))) { Log("dynamic texture name=%s allocation_failed=1",name.c_str()); return; }
    c->CopyResource(copy.Get(),texture.Get());
    std::ofstream file(output/(name+".surface"),std::ios::binary);
    const uint32_t header[]{desc.Width,desc.Height,static_cast<uint32_t>(desc.Format),desc.ArraySize,desc.MipLevels,bpp};
    file.write(reinterpret_cast<const char*>(header),sizeof(header));
    for (UINT slice=0;slice<desc.ArraySize;++slice) for (UINT mip=0;mip<desc.MipLevels;++mip) {
        const auto subresource=D3D11CalcSubresource(mip,slice,desc.MipLevels);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(c->Map(copy.Get(),subresource,D3D11_MAP_READ,0,&mapped))) { Log("dynamic texture name=%s map_failed=1",name.c_str()); return; }
        const auto w=std::max(1u,desc.Width>>mip),h=std::max(1u,desc.Height>>mip);
        for (UINT row=0;row<h;++row) file.write(static_cast<const char*>(mapped.pData)+row*mapped.RowPitch,w*bpp);
        c->Unmap(copy.Get(),subresource);
    }
    file.close(); totalBytes+=bytes;
    Log("dynamic texture name=%s resource=%p format=%u width=%u height=%u array=%u mips=%u bytes=%llu saved=%u",
        name.c_str(),resource.Get(),desc.Format,desc.Width,desc.Height,desc.ArraySize,desc.MipLevels,bytes,file.good());
}
void CaptureProducerInputs(ID3D11DeviceContext* c,const std::string& mark) {
    ComPtr<ID3D11PixelShader> shader; c->PSGetShader(&shader,nullptr,nullptr);
    const auto hash=replaytrace::ShaderFingerprint(shader.Get());
    const bool material=traceReplaySamples && mark==sampleDraw[engine::CurrentScene().pass] && hash==0x922a49bc5dcd4775ull;
    const UINT count=material ? 8:hash==0xe8bc75f0f617aa9eull ? 3:hash==0x158f4162b8000a3full ? 5:hash==0x789227e243eff8eaull ? 1:0;
    if (!count) return;
    // Hash every use: a first-seen resource cache would hide intervening writes.
    // At most the already bounded early-draw checkpoints are inspected.
    static uint64_t budget=1024ull*1024*1024;
    static uint64_t materialBudget=512ull*1024*1024;
    static uint64_t shadowBudget=256ull*1024*1024;
    std::ofstream file(output/"producer-inputs.jsonl",std::ios::app);
    char identity[17]{}; sprintf_s(identity,"%016llx",hash);
    auto record=[&](const std::string& slot,ID3D11Resource* resource,const D3D11_SHADER_RESOURCE_VIEW_DESC* view) {
        file << "{\"mark\":\"" << mark << "\",\"shader\":\"" << identity << "\",\"slot\":\"" << slot << "\",\"view\":";
        if (view) {
            file << "{\"format\":" << view->Format << ",\"dimension\":" << view->ViewDimension;
            if (view->ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D)
                file << ",\"firstMip\":" << view->Texture2D.MostDetailedMip << ",\"mips\":" << view->Texture2D.MipLevels;
            else if (view->ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2DARRAY)
                file << ",\"firstMip\":" << view->Texture2DArray.MostDetailedMip << ",\"mips\":" << view->Texture2DArray.MipLevels
                    << ",\"firstSlice\":" << view->Texture2DArray.FirstArraySlice << ",\"slices\":" << view->Texture2DArray.ArraySize;
            file << '}';
        } else file << "null";
        file << ",\"texture\":";
        WriteTextureDigest(file,DigestTexture(c,resource,material ? materialBudget:hash==0x158f4162b8000a3full ? shadowBudget:budget));
        file << "}\n";
    };
    ID3D11ShaderResourceView* views[8]{}; c->PSGetShaderResources(0,count,views);
    for (UINT slot=0;slot<count;++slot) {
        ComPtr<ID3D11ShaderResourceView> view; view.Attach(views[slot]);
        ComPtr<ID3D11Resource> resource; D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        if (view) { view->GetResource(&resource); view->GetDesc(&desc); }
        record("ps"+std::to_string(slot),resource.Get(),view ? &desc:nullptr);
    }
    ComPtr<ID3D11RenderTargetView> target; ComPtr<ID3D11DepthStencilView> depth;
    c->OMGetRenderTargets(1,&target,&depth);
    ComPtr<ID3D11Resource> resource; if (target) target->GetResource(&resource);
    record("target-before",resource.Get(),nullptr);
    if (hash==0x158f4162b8000a3full) {
        resource.Reset(); if (depth) depth->GetResource(&resource);
        record("depth-before",resource.Get(),nullptr);
    }
    static std::set<ID3D11DeviceChild*> described;
    auto describe=[&](const char* kind,ID3D11DeviceChild* object,const void* data,size_t size) {
        if (!object || described.size()>=64 || !described.insert(object).second) return;
        std::string words;
        for (size_t offset=0;offset<size;offset+=4) { uint32_t word{}; memcpy(&word,static_cast<const char*>(data)+offset,4); words+=std::to_string(word)+","; }
        Log("producer_state kind=%s object=%p words=%s",kind,object,words.c_str());
    };
    ComPtr<ID3D11RasterizerState> raster; c->RSGetState(&raster);
    D3D11_RASTERIZER_DESC rd{}; if (raster) { raster->GetDesc(&rd); describe("raster",raster.Get(),&rd,sizeof(rd)); }
    ComPtr<ID3D11DepthStencilState> ds; UINT stencil{}; c->OMGetDepthStencilState(&ds,&stencil);
    D3D11_DEPTH_STENCIL_DESC dd{}; if (ds) { ds->GetDesc(&dd); describe("depth",ds.Get(),&dd,sizeof(dd)); }
    ComPtr<ID3D11BlendState> blend; FLOAT factors[4]{}; UINT mask{}; c->OMGetBlendState(&blend,factors,&mask);
    D3D11_BLEND_DESC bd{}; if (blend) { blend->GetDesc(&bd); describe("blend",blend.Get(),&bd,sizeof(bd)); }
}
void CaptureDrawInputs(ID3D11DeviceContext* c,const std::string& mark) {
    if (mark.empty()) return;
    const auto suffix=mark.rfind("-draw");
    if (suffix==std::string::npos) return;
    const auto n=std::stoul(mark.substr(suffix+5));
    if (n>32) return;
    FreshReplayColor(c,mark);
    if (replaytrace::Compact()) {
        // Keep the actual GPU camera buffer for the first scene's early draws.
        // Full mesh, texture and per-draw target readbacks are optional detail.
        ComPtr<ID3D11Buffer> camera;c->VSGetConstantBuffers(3,1,&camera);
        if (camera) CaptureComputeResource(c,camera.Get(),mark+"-v3");
        return;
    }
    auto& selected=sampleDraw[engine::CurrentScene().pass];
    if (traceReplaySamples && selected.empty() && mark.find("-slot6-")!=std::string::npos) {
        ComPtr<ID3D11PixelShader> shader; c->PSGetShader(&shader,nullptr,nullptr);
        if (replaytrace::ShaderFingerprint(shader.Get())==0x922a49bc5dcd4775ull) selected=mark;
    }
    if (traceReplaySamples && mark.find("-slot6-")!=std::string::npos && mark!=selected && n!=1) return;
    CaptureSamples(c,mark,"before");
    CaptureProducerInputs(c,mark);
    if (n>=9 && mark.find("-slot6-")!=std::string::npos) {
        ID3D11ShaderResourceView* textures[12]{}; c->PSGetShaderResources(0,12,textures);
        for (unsigned slot=0;slot<12;++slot) {
            ComPtr<ID3D11ShaderResourceView> view; view.Attach(textures[slot]);
            CaptureDynamicTexture(c,view.Get(),mark+"-psdynamic"+std::to_string(slot));
        }
    }
    for (unsigned stage=0;stage<2;++stage) {
        ID3D11Buffer* buffers[14]{};
        if (stage) c->PSGetConstantBuffers(0,14,buffers); else c->VSGetConstantBuffers(0,14,buffers);
        for (unsigned slot=0;slot<14;++slot) {
            ComPtr<ID3D11Buffer> buffer; buffer.Attach(buffers[slot]);
            if (buffer) CaptureComputeResource(c,buffer.Get(),mark+(stage?"-p":"-v")+std::to_string(slot));
        }
    }
    if (n==1) {
        ComPtr<ID3D11ShaderResourceView> input; c->PSGetShaderResources(0,1,&input);
        ComPtr<ID3D11Resource> resource; if (input) input->GetResource(&resource);
        CaptureComputeResource(c,resource.Get(),mark+"-psrv0");
    }
    // First observed contents per mesh resource/pass; later mutations within
    // that pass are deliberately outside this bounded snapshot's coverage.
    static std::set<std::pair<unsigned,ID3D11Buffer*>> meshes;
    const auto pass=engine::CurrentScene().pass;
    ID3D11Buffer* streams[4]{}; UINT strides[4]{},offsets[4]{};
    c->IAGetVertexBuffers(0,4,streams,strides,offsets);
    for (unsigned slot=0;slot<4;++slot) {
        ComPtr<ID3D11Buffer> buffer; buffer.Attach(streams[slot]);
        if (buffer && meshes.size()<256 && meshes.insert({pass,buffer.Get()}).second)
            CaptureComputeResource(c,buffer.Get(),mark+"-vertex"+std::to_string(slot));
    }
    ComPtr<ID3D11Buffer> index; DXGI_FORMAT format{}; UINT offset{}; c->IAGetIndexBuffer(&index,&format,&offset);
    if (index && meshes.size()<256 && meshes.insert({pass,index.Get()}).second) CaptureComputeResource(c,index.Get(),mark+"-index");
    // Snapshot the small structured light buffers at each early draw too.
    ID3D11ShaderResourceView* lights[4]{}; c->PSGetShaderResources(20,4,lights);
    for (unsigned i=0;i<4;++i) {
        ComPtr<ID3D11ShaderResourceView> view; view.Attach(lights[i]);
        if (!view) continue;
        ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
        CaptureComputeResource(c,resource.Get(),mark+"-psrv"+std::to_string(20+i));
    }
}
void LateDraw(ID3D11DeviceContext* context,unsigned kind,std::span<const UINT> args) {
    if ((!lateUi.Frame() && !nativeUiWitness.Frame()) || engine::CurrentScene().id || !replaytrace::ScreenUiEnabled()) return;
    ComPtr<ID3D11VertexShader> shader;context->VSGetShader(&shader,nullptr,nullptr);
    const auto hash=replaytrace::ShaderFingerprint(shader.Get());
    nativeUiWitness.Observe(context,frame.load(),hash);
    if (!lateUi.Frame()) return;
    struct Call {unsigned kind;std::span<const UINT> args;} call{kind,args};
    const auto draw=[](ID3D11DeviceContext* c,void* opaque) {
        const auto& value=*static_cast<const Call*>(opaque);const auto& a=value.args;
        switch (value.kind) {
        case 0: realDraw(c,a[0],a[1]);break;
        case 1: realDrawIndexed(c,a[0],a[1],INT(a[2]));break;
        case 2: realDrawInstanced(c,a[0],a[1],a[2],a[3]);break;
        case 3: realDrawIndexedInstanced(c,a[0],a[1],a[2],INT(a[3]),a[4]);break;
        }
    };
    if (lateUi.Draw(context,frame.load(),hash,draw,&call)==ScreenUi::Result::Failed) {
        Log("screen_ui late_draw_failed=1 frame=%llu",frame.load());engine::FailSession();
    }
}
bool BeginFlatMenu(ID3D11DeviceContext* context) {
    if (!flatMenuUi.Enabled() || runtimeStopped || engine::SessionFailed() ||
        engine::SessionRendering() || engine::ReplayingEye()) return false;
    ComPtr<ID3D11VertexShader> shader;context->VSGetShader(&shader,nullptr,nullptr);
    const auto hash=replaytrace::ShaderFingerprint(shader.Get());
    const bool applied=flatMenuUi.Apply(context,hash);
    static bool reported{};
    if (applied && !reported) {
        reported=true;Log("session flat_menu camera corrected=1 canvas=16:9 frame=%llu shader=%016llx",frame.load(),hash);
    }
    return applied;
}
void EndFlatMenu(ID3D11DeviceContext* context,bool applied) {
    if (applied && !flatMenuUi.Restore(context)) {
        Log("flat_menu restore_failed=1 frame=%llu",frame.load());engine::FailSession();
    }
}
void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* c,UINT n,UINT start) {
    profile::Scope measure(profile::DrawHook);
    ++draws; CameraDraw(c); const UINT args[]{n,start}; const auto mark=replaytrace::Work(c,"Draw",args);
    CaptureDrawInputs(c,mark);
    auto repeat=PrepareProducerRepeat(c,mark,true,n,start);
    const bool flatMenu=BeginFlatMenu(c);
    {profile::Scope driver(profile::DrawDriver);realDraw(c,n,start);}
    EndFlatMenu(c,flatMenu);
    flicker::AfterDraw(c);
    CompleteProducerRepeat(c,mark,repeat,true,n,start);
    replaytrace::AfterDraw(c); exposureaudit::AfterDraw(c,frame.load()); CaptureWorkMark(c,mark);LateDraw(c,0,args);
}
void STDMETHODCALLTYPE DrawIndexed(ID3D11DeviceContext* c,UINT n,UINT start,INT base) {
    profile::Scope measure(profile::DrawHook);
    ++draws; CameraDraw(c); const UINT args[]{n,start,static_cast<UINT>(base)}; const auto mark=replaytrace::Work(c,"Indexed",args);
    CaptureDrawInputs(c,mark);
    auto repeat=PrepareProducerRepeat(c,mark,false,n,start,base);
    const bool flatMenu=BeginFlatMenu(c);
    {profile::Scope driver(profile::DrawDriver);realDrawIndexed(c,n,start,base);}
    EndFlatMenu(c,flatMenu);
    flicker::AfterDraw(c);
    CompleteProducerRepeat(c,mark,repeat,false,n,start,base);
    replaytrace::AfterDraw(c); exposureaudit::AfterDraw(c,frame.load()); CaptureWorkMark(c,mark);LateDraw(c,1,args);
}
void STDMETHODCALLTYPE DrawInstanced(ID3D11DeviceContext* c,UINT n,UINT count,UINT start,UINT instance) {
    profile::Scope measure(profile::DrawHook);
    ++draws; CameraDraw(c); const UINT args[]{n,count,start,instance}; const auto mark=replaytrace::Work(c,"Instanced",args);
    CaptureDrawInputs(c,mark);
    const bool flatMenu=BeginFlatMenu(c);
    {profile::Scope driver(profile::DrawDriver);realDrawInstanced(c,n,count,start,instance);}
    EndFlatMenu(c,flatMenu);
    flicker::AfterDraw(c);
    replaytrace::AfterDraw(c); exposureaudit::AfterDraw(c,frame.load()); CaptureWorkMark(c,mark);LateDraw(c,2,args);
}
void STDMETHODCALLTYPE DrawIndexedInstanced(ID3D11DeviceContext* c,UINT n,UINT count,UINT start,INT base,UINT instance) {
    profile::Scope measure(profile::DrawHook);
    ++draws; CameraDraw(c); const UINT args[]{n,count,start,static_cast<UINT>(base),instance}; const auto mark=replaytrace::Work(c,"IndexedInstanced",args);
    CaptureDrawInputs(c,mark);
    const bool flatMenu=BeginFlatMenu(c);
    {profile::Scope driver(profile::DrawDriver);realDrawIndexedInstanced(c,n,count,start,base,instance);}
    EndFlatMenu(c,flatMenu);
    flicker::AfterDraw(c);
    replaytrace::AfterDraw(c); exposureaudit::AfterDraw(c,frame.load()); CaptureWorkMark(c,mark);LateDraw(c,3,args);
}
void CaptureComputeResource(ID3D11DeviceContext* c,ID3D11Resource* resource,const std::string& name) {
    static uint64_t workBytes{},boundaryBytes{},bufferBytes{},sampleBytes{},computeBytes{},repeatBytes{};
    // The before/after restoration pair must not compete with optional draw inputs.
    const bool boundary=name.starts_with("stage-");
    const bool sample=name.find("-msaa")!=std::string::npos;
    const bool compute=name.find("-compute")!=std::string::npos;
    const bool repeat=name.find("-repeat-")!=std::string::npos;
    auto& capturedBytes=boundary ? boundaryBytes:sample ? sampleBytes:compute ? computeBytes:repeat ? repeatBytes:workBytes;
    const uint64_t budget=(boundary ? 128ull:sample ? 1024ull:compute ? 384ull:repeat ? 192ull:512ull)*1024*1024;
    if (!resource) { Log("compute resource name=%s unbound=1",name.c_str()); return; }
    ComPtr<ID3D11Device> device; c->GetDevice(&device);
    ComPtr<ID3D11Buffer> source,staging;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&source)))) {
        D3D11_BUFFER_DESC original{}; source->GetDesc(&original);
        if (original.ByteWidth>32*1024*1024 || bufferBytes+original.ByteWidth>128ull*1024*1024) { Log("compute buffer name=%s refused_bound=1",name.c_str()); return; }
        auto desc=original; desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.MiscFlags=0;
        desc.StructureByteStride=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateBuffer(&desc,nullptr,&staging))) { Log("compute buffer name=%s allocation_failed=1",name.c_str()); return; }
        c->CopyResource(staging.Get(),source.Get()); D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) { Log("compute buffer name=%s map_failed=1",name.c_str()); return; }
        std::ofstream file(output/(name+".buffer"),std::ios::binary);
        file.write(static_cast<const char*>(mapped.pData),original.ByteWidth); file.close(); c->Unmap(staging.Get(),0);
        bufferBytes+=original.ByteWidth;
        Log("compute buffer name=%s resource=%p bytes=%u stride=%u misc=%u saved=%u",name.c_str(),source.Get(),original.ByteWidth,original.StructureByteStride,original.MiscFlags,file.good()); return;
    }
    ComPtr<ID3D11Texture2D> texture,copy;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) { Log("compute resource name=%s unsupported=1",name.c_str()); return; }
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    unsigned bpp{};
    switch(desc.Format) {
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: bpp=4; break;
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R8G8_UNORM: bpp=2; break;
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: bpp=1; break;
    default: break;
    }
    const auto bytes=static_cast<uint64_t>(desc.Width)*desc.Height*desc.ArraySize*bpp;
    if (!bpp || desc.MipLevels!=1 || desc.SampleDesc.Count!=1 || bytes>64*1024*1024 || capturedBytes+bytes>budget) {
        Log("compute texture name=%s refused=1 format=%u array=%u samples=%u",name.c_str(),desc.Format,desc.ArraySize,desc.SampleDesc.Count); return;
    }
    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.MiscFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&copy))) { Log("compute texture name=%s allocation_failed=1",name.c_str()); return; }
    c->CopyResource(copy.Get(),texture.Get());
    std::ofstream file(output/(name+".texture"),std::ios::binary);
    const uint32_t header[]{desc.Width,desc.Height,static_cast<uint32_t>(desc.Format),desc.ArraySize,desc.SampleDesc.Count,bpp};
    file.write(reinterpret_cast<const char*>(header),sizeof(header));
    for (UINT slice=0;slice<desc.ArraySize;++slice) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(c->Map(copy.Get(),slice,D3D11_MAP_READ,0,&mapped))) { Log("compute texture name=%s map_failed=1",name.c_str()); return; }
        for (UINT row=0;row<desc.Height;++row) file.write(static_cast<const char*>(mapped.pData)+row*mapped.RowPitch,desc.Width*bpp);
        c->Unmap(copy.Get(),slice);
    }
    file.close(); capturedBytes+=bytes;
    Log("compute texture name=%s resource=%p size=%ux%u format=%u array=%u bytes=%llu saved=%u",name.c_str(),texture.Get(),desc.Width,desc.Height,desc.Format,desc.ArraySize,bytes,file.good());
}
void STDMETHODCALLTYPE Dispatch(ID3D11DeviceContext* c,UINT x,UINT y,UINT z) {
    profile::Scope measure(profile::DispatchHook);
    ++dispatches; const UINT args[]{x,y,z}; const auto mark=replaytrace::Work(c,"Compute",args);
    replaytrace::BeforeCompute(c,mark);
    if (!mark.empty() && replaytrace::Detailed()) {
        if (mark.ends_with("-compute1")) {
            ComPtr<ID3D11UnorderedAccessView> view; c->CSGetUnorderedAccessViews(0,1,&view);
            ComPtr<ID3D11Resource> resource; if (view) view->GetResource(&resource);
            CaptureComputeResource(c,resource.Get(),mark+"-uav0-before");
        }
        for (UINT slot:{0u,3u,7u,12u}) {
            ComPtr<ID3D11Buffer> input; c->CSGetConstantBuffers(slot,1,&input);
            CaptureComputeResource(c,input.Get(),mark+"-cb"+std::to_string(slot));
        }
    }
    {profile::Scope driver(profile::DispatchDriver);realDispatch(c,x,y,z);}
    if (mark.empty()) return;
    if (replaytrace::VplOrderRequested(mark)) {
        ComPtr<ID3D11UnorderedAccessView> native; c->CSGetUnorderedAccessViews(1,1,&native);
        ComPtr<ID3D11Resource> resource; if (native) native->GetResource(&resource);
        if (replaytrace::Detailed()) CaptureComputeResource(c,resource.Get(),mark+"-uav1-native");
        replaytrace::AfterCompute(c,mark,x,y,z,realDispatch);
    }
    if (!replaytrace::Detailed()) return;
    ID3D11UnorderedAccessView* views[8]{}; c->CSGetUnorderedAccessViews(0,8,views);
    for (unsigned slot=0;slot<8;++slot) {
        ComPtr<ID3D11UnorderedAccessView> view; view.Attach(views[slot]); if (!view) continue;
        ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
        CaptureComputeResource(c,resource.Get(),mark+"-uav"+std::to_string(slot));
    }
}
void ClearTrace(ID3D11View* view,const char* kind,UINT flags,const FLOAT* values,UINT8 stencil) {
    static unsigned count{};
    if (!traceReplayWork || frame.load()>=captureUntil.load() || count++>=256) return;
    const auto scene=engine::CurrentScene(); const auto stage=engine::CurrentStage();
    ComPtr<ID3D11Resource> resource; if (view) view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture; D3D11_TEXTURE2D_DESC desc{};
    if (resource && SUCCEEDED(resource.As(&texture))) texture->GetDesc(&desc);
    Log("clear scene=%llu pass=%u slot=%u kind=%s view=%p resource=%p format=%u samples=%u flags=%u value=%.9g,%.9g,%.9g,%.9g stencil=%u",
        scene.id,scene.pass,stage.slot,kind,view,resource.Get(),desc.Format,desc.SampleDesc.Count,flags,
        values[0],values[1],values[2],values[3],stencil);
}
void STDMETHODCALLTYPE ClearColor(ID3D11DeviceContext* c,ID3D11RenderTargetView* view,const FLOAT* color) {
    ClearTrace(view,"color",0,color,0); realClearColor(c,view,color);
}
void STDMETHODCALLTYPE ClearDepth(ID3D11DeviceContext* c,ID3D11DepthStencilView* view,UINT flags,FLOAT depth,UINT8 stencil) {
    const FLOAT values[]{depth,0,0,0}; ClearTrace(view,"depth",flags,values,stencil); realClearDepth(c,view,flags,depth,stencil);
    if(flicker::Capturing())flicker::DepthClear(view,engine::CurrentScene().pass,flags,depth,stencil);
}
bool GameQueryCaller(void* caller) {
    MEMORY_BASIC_INFORMATION region{};
    return flicker::Capturing() && VirtualQuery(caller,&region,sizeof(region)) && uintptr_t(region.AllocationBase)==gameBase;
}
void STDMETHODCALLTYPE QueryBegin(ID3D11DeviceContext* c,ID3D11Asynchronous* query) {
    const auto caller=_ReturnAddress();realQueryBegin(c,query);
    if(GameQueryCaller(caller))flicker::NativeQuery(query,engine::CurrentScene().pass,"begin",uintptr_t(caller)-gameBase);
}
void STDMETHODCALLTYPE QueryEnd(ID3D11DeviceContext* c,ID3D11Asynchronous* query) {
    const auto caller=_ReturnAddress();realQueryEnd(c,query);
    if(GameQueryCaller(caller))flicker::NativeQuery(query,engine::CurrentScene().pass,"end",uintptr_t(caller)-gameBase);
}
HRESULT STDMETHODCALLTYPE QueryData(ID3D11DeviceContext* c,ID3D11Asynchronous* query,void* data,UINT bytes,UINT flags) {
    const auto caller=_ReturnAddress();const auto hr=realQueryData(c,query,data,bytes,flags);
    if(GameQueryCaller(caller))flicker::NativeQuery(query,engine::CurrentScene().pass,"get",uintptr_t(caller)-gameBase,hr,data,bytes);
    return hr;
}
}

// Admission failed, so no trace exists yet: leave one bounded receipt naming
// the failed step for the run folder the launcher already created.
void WriteHostRefusal(const std::filesystem::path& folder,const gameexe::Refusal& refusal) {
    std::string signer;
    for(const auto c:refusal.signer)signer+=c>=0x20 && c<0x7f && c!='"' && c!='\\'?static_cast<char>(c):'?';
    char trust[16]{};sprintf_s(trust,"0x%08lx",static_cast<unsigned long>(refusal.trust));
    std::ofstream file(folder/L"host-admission.json",std::ios::trunc);
    file<<"{\"schema\":1,\"admitted\":false,\"step\":\""<<refusal.step<<"\",\"error\":"<<refusal.error<<",\"size\":"<<refusal.size
        <<",\"sha256\":\""<<refusal.sha256<<"\",\"normalizedSha256\":\""<<refusal.normalized
        <<"\",\"winVerifyTrust\":\""<<trust<<"\",\"signer\":\""<<signer<<"\"}\n";
}

bool Active() {
    static const bool enabled=[] {
        try {
            wchar_t path[32768]{}, value[32768]{};
            GetModuleFileNameW(nullptr,path,32768);
            const auto host=std::filesystem::path(path);
            const auto flag=GetEnvironmentVariableW(L"GRID2VR_DIAGNOSTICS",value,32768);
            if (flag && std::wstring(value)!=L"1") return false;
            if (flag) {
                const auto length=GetEnvironmentVariableW(L"GRID2VR_OUTPUT",value,32768);
                if (!length || length>=32768) return false;
                output=value;
            } else {
                std::ifstream marker(host.parent_path()/L"GRID2VR.diagnostic-session");
                std::string header,folder;
                if (!std::getline(marker,header) || header!="GRID2VR_DIAGNOSTIC_V1" || !std::getline(marker,folder)) return false;
                output=std::filesystem::path(std::u8string(folder.begin(),folder.end()));
            }
            if (!output.is_absolute()) return false;
            gameexe::Refusal refusal;
            if (gameexe::Validate(host.c_str(),true,&refusal).empty()) {WriteHostRefusal(output,refusal);return false;}
            std::filesystem::create_directories(output);
            runtimeSession=std::filesystem::exists(output/L"runtime-session.request");
            focusCockpit.Configure(host.parent_path(),output);
            exposureaudit::Configure(output);
            diagnosticLogging=!runtimeSession || std::filesystem::exists(output/L"diagnostic-logging.request");
            unattendedStereo=runtimeSession && std::filesystem::is_regular_file(output/L"flicker-diagnostic.request") &&
                std::filesystem::is_regular_file(output/L"flicker-unattended.request");
#ifdef GRID2VR_ENABLE_XR_SESSION
            if (runtimeSession && std::filesystem::exists(output/L"xr-projection.request")) {
                // UI camera uploads start before the first Present/replay setup.
                // Only the explicit headset-relative session needs a 16:9 canvas
                // independent of its per-eye pixel aspect.
                flatMenuUi.Configure(output);
                recenterKeepHorizonLevel=!std::filesystem::exists(output/L"runtime-recenter-tilt.request");
                recenterBindingValid=true;
                const auto bindingPath=output/L"runtime-recenter-key.txt";
                if (std::filesystem::exists(bindingPath)) {
                    std::ifstream input(bindingPath); std::string text;
                    recenterBindingValid=std::filesystem::file_size(bindingPath)<=16 &&
                        static_cast<bool>(std::getline(input,text)) && input.peek()==std::char_traits<char>::eof() &&
                        RecenterShortcut::Parse(text,recenterBinding);
                }
                Log("session recenter shortcut configured=%u key=%u modifiers=%u",recenterBindingValid,recenterBinding.key,recenterBinding.modifiers);
            }
#endif
            gameBase=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            if(runtimeSession)dlccockpit::Prepare(gameBase,output);
            if(runtimeSession) {chaseobserver::Prepare(gameBase,output);chasecamera::Prepare(gameBase,output);}
            engine::ObserveFaults(gameBase,output);
            Log("GRID2VR diagnostic supported AVX host; engine replay and game-device OpenXR probe separately opt-in; no headset image submission");
            Log("GRID2VR renderer source revision=%s",SourceRevision());
            return true;
        } catch (...) { return false; }
    }();
    return enabled;
}

void Attach(ID3D11Device* device,ID3D11DeviceContext* context,IDXGISwapChain* chain) {
    opponentlod::host::RevokeWorld();
    std::lock_guard lock(attachMutex);
    static const bool initialized=[] { const auto s=MH_Initialize(); return s==MH_OK || s==MH_ERROR_ALREADY_INITIALIZED; }();
    if (!initialized) return;
    if(!cockpitanimation::Install(gameBase,output)) {Log("cockpit animation qualification refused");return;}
    if(!msaaaudit::Install(gameBase,output)) {Log("MSAA audit startup hook refused");return;}
    if((runtimeSession || msaaaudit::Enabled() || focusCockpit.Enabled()) && !Hook(device,5,reinterpret_cast<void*>(CreateTexture2D),realTexture2D)) {
        focusCockpit.HookFailed();Log("texture observation hook refused");if(msaaaudit::Enabled())return;
    }
    if(focusCockpit.Enabled() && !Hook(device,7,reinterpret_cast<void*>(CreateResourceView),realResourceView)) {
        focusCockpit.HookFailed();Log("Focus cockpit view hook refused; stock retained");
    }
    if(!instancecapacity::Install(gameBase,output)) {Log("instance capacity startup hook refused");return;}
    if(instancecapacity::Enabled() && !Hook(device,3,reinterpret_cast<void*>(CreateBuffer),realBuffer)) {instancecapacity::Disable();Log("instance capacity buffer observation refused");return;}
    Log("session device=%p feature_level=0x%x",device,device->GetFeatureLevel());
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; ComPtr<IDXGIFactory> factory;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC desc{}; adapter->GetDesc(&desc);
        Log("adapter vendor=0x%x device=0x%x dedicated_MB=%zu LUID=%lx:%lx",desc.VendorId,desc.DeviceId,
            desc.DedicatedVideoMemory/1048576,desc.AdapterLuid.HighPart,desc.AdapterLuid.LowPart);
        if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) Hook(factory.Get(),10,reinterpret_cast<void*>(CreateSwapchain),realSwap);
    }
    Hook(device,12,reinterpret_cast<void*>(CreateVS),realVS); Hook(device,15,reinterpret_cast<void*>(CreatePS),realPS);
    Hook(device,18,reinterpret_cast<void*>(CreateCS),realCS);
    if(std::filesystem::is_regular_file(output/L"flicker-diagnostic.request"))
        Hook(device,11,reinterpret_cast<void*>(CreateLayout),realLayout);
    ComPtr<ID3D11DeviceContext> owned;
    if (!context) { device->GetImmediateContext(&owned); context=owned.Get(); }
    Hook(context,12,reinterpret_cast<void*>(DrawIndexed),realDrawIndexed);
    Hook(context,13,reinterpret_cast<void*>(Draw),realDraw);
    Hook(context,14,reinterpret_cast<void*>(Map),realMap);
    Hook(context,15,reinterpret_cast<void*>(Unmap),realUnmap);
    Hook(context,20,reinterpret_cast<void*>(DrawIndexedInstanced),realDrawIndexedInstanced);
    Hook(context,21,reinterpret_cast<void*>(DrawInstanced),realDrawInstanced);
    Hook(context,41,reinterpret_cast<void*>(Dispatch),realDispatch);
    Hook(context,50,reinterpret_cast<void*>(ClearColor),realClearColor);
    Hook(context,53,reinterpret_cast<void*>(ClearDepth),realClearDepth);
    if(std::filesystem::is_regular_file(output/L"flicker-diagnostic.request")) {
        Hook(context,27,reinterpret_cast<void*>(QueryBegin),realQueryBegin);
        Hook(context,28,reinterpret_cast<void*>(QueryEnd),realQueryEnd);
        Hook(context,29,reinterpret_cast<void*>(QueryData),realQueryData);
    }
    if (chain) AttachSwapchain(chain);
}
}
