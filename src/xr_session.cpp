#include "xr_session.h"
#include "tracked_views.h"
#include <dxgi.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace grid2vr {
namespace {
bool LevelScreenAnchor(const XrPosef& head,XrPosef& anchor) {
    std::array<double,4> q{};
    if (!TrackingPosition(head.position) || !TrackingOrientation(head.orientation,q)) return false;
    const auto x=q[0],y=q[1],z=q[2],w=q[3];
    // Project the head's forward axis onto the horizontal plane. Pitch and
    // roll never tilt the screen; the resulting pose is retained in LOCAL.
    double forwardX=-2*(x*z+w*y),forwardZ=-(1-2*(x*x+y*y));
    double length=std::hypot(forwardX,forwardZ);
    if (length<1e-6) {
        // Straight up/down has no forward heading; use the horizontal right
        // axis to choose a stable level heading for this initial placement.
        const double rightX=1-2*(y*y+z*z),rightZ=2*(x*z-w*y);
        forwardX=rightZ;forwardZ=-rightX;length=std::hypot(forwardX,forwardZ);
    }
    if (length<1e-6) return false;
    forwardX/=length;forwardZ/=length;
    const double yaw=std::atan2(-forwardX,-forwardZ);
    anchor={};anchor.orientation.y=static_cast<float>(std::sin(yaw*.5));anchor.orientation.w=static_cast<float>(std::cos(yaw*.5));
    anchor.position={head.position.x+static_cast<float>(2*forwardX),head.position.y,head.position.z+static_cast<float>(2*forwardZ)};
    return TrackingPosition(anchor.position);
}
}
void XrSessionProbe::Log(const char* format,...) {
    char line[1024]{};va_list args;va_start(args,format);
    vsnprintf_s(line,sizeof(line),_TRUNCATE,format,args);va_end(args);
    if (!diagnosticLogging_ && (!strncmp(line,"pose ",5) || !strncmp(line,"fov ",4) ||
        !strncmp(line,"sample ",7) || !strncmp(line,"images submitted=",17) || !strncmp(line,"screen submitted=",17))) return;
    log_<<GetTickCount64()<<" tid="<<GetCurrentThreadId()<<" frame="<<frame_<<' '<<line<<'\n';log_.flush();
}
bool XrSessionProbe::Check(XrResult result,const char* operation) {
    const bool frameOperation=!strcmp(operation,"wait_frame") || !strcmp(operation,"begin_frame") ||
        !strcmp(operation,"locate_views") || !strcmp(operation,"locate_head") || !strcmp(operation,"poll_event") ||
        !strncmp(operation,"end_frame_",10);
    if (diagnosticLogging_ || !frameOperation || result!=XR_SUCCESS) Log("xr operation=%s result=%d",operation,result);
    if (result==XR_SUCCESS) return true;
    failed_=true;return false;
}
bool XrSessionProbe::Initialize(ID3D11Device* device,const std::filesystem::path& runtime,const std::filesystem::path& output) {
    if (instance_ || thread_ || !device) return false;
    diagnosticLogging_=!std::filesystem::exists(output/L"runtime-session.request") || std::filesystem::exists(output/L"diagnostic-logging.request");
    thread_=GetCurrentThreadId();log_.open(output/L"openxr-session.log",
        std::filesystem::exists(output/L"runtime-session.request")?sessionLogSegmentBytes:0);
    const bool vdxr=runtime.filename()==L"virtualdesktop-openxr-32.json";
    const bool steamVr=runtime.filename()==L"steamxr_win32.json";
    if (!log_ || !runtime.is_absolute() || (!vdxr && !steamVr) || !std::filesystem::is_regular_file(runtime)) {
        failed_=true;Log("session refused reason=runtime_manifest");return false;
    }
    // Steam can spawn the AVX process without inheriting launcher variables.
    // Select the loader runtime inside this process, then restore its old value.
    const DWORD length=GetEnvironmentVariableW(L"XR_RUNTIME_JSON",nullptr,0);
    std::vector<wchar_t> previous(length?length:1);
    if (length && !GetEnvironmentVariableW(L"XR_RUNTIME_JSON",previous.data(),length)) {failed_=true;return false;}
    if (!SetEnvironmentVariableW(L"XR_RUNTIME_JSON",runtime.c_str())) {failed_=true;return false;}
    const char* extension=XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(create.applicationInfo.applicationName,"GRID2VR game-device session probe");
    create.applicationInfo.apiVersion=XR_MAKE_VERSION(1,0,0);
    create.enabledExtensionCount=1;create.enabledExtensionNames=&extension;
    const auto created=xrCreateInstance(&create,&instance_);
    const bool environmentRestored=SetEnvironmentVariableW(L"XR_RUNTIME_JSON",length?previous.data():nullptr)!=FALSE;
    Log("runtime_selection process_only=1 environment_restored=%u",environmentRestored);
    if (!Check(created,"create_instance") || !environmentRestored) {failed_=true;Shutdown();return false;}
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (!Check(xrGetInstanceProperties(instance_,&properties),"runtime_properties") ||
        (vdxr ? strcmp(properties.runtimeName,"VirtualDesktopXR")!=0 : strncmp(properties.runtimeName,"SteamVR/",8)!=0)) {
        failed_=true;Log("session refused reason=runtime_identity");Shutdown();return false;
    }
    Log("runtime name=%s version=%llu",properties.runtimeName,properties.runtimeVersion);
    // The bounded trace may rotate during driving; retain the selected provider.
    std::ofstream identity(output/L"openxr-runtime.json");
    identity << "{\"schema\":1,\"selected\":\"" << (vdxr?"VDXR":"SteamVR")
        << "\",\"name\":" << std::quoted(properties.runtimeName)
        << ",\"version\":" << properties.runtimeVersion
        << ",\"manifest\":" << std::quoted(runtime.filename().string())
        << ",\"processOnly\":true,\"environmentRestored\":true}\n";
    XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO};get.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system{};
    PFN_xrGetD3D11GraphicsRequirementsKHR requirementsFn{};
    if (!Check(xrGetSystem(instance_,&get,&system),"get_system") ||
        !Check(xrGetInstanceProcAddr(instance_,"xrGetD3D11GraphicsRequirementsKHR",reinterpret_cast<PFN_xrVoidFunction*>(&requirementsFn)),"get_requirements") || !requirementsFn) {failed_=true;Shutdown();return false;}
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!Check(requirementsFn(instance_,system,&requirements),"graphics_requirements")) {Shutdown();return false;}
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
    const bool compatible=SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
        SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)) &&
        !memcmp(&desc.AdapterLuid,&requirements.adapterLuid,sizeof(LUID)) && device->GetFeatureLevel()>=requirements.minFeatureLevel;
    Log("device game=%p compatible=%u luid=%08lx:%08lx required=%08lx:%08lx feature=0x%x minimum=0x%x",
        device,compatible,desc.AdapterLuid.HighPart,desc.AdapterLuid.LowPart,requirements.adapterLuid.HighPart,
        requirements.adapterLuid.LowPart,device->GetFeatureLevel(),requirements.minFeatureLevel);
    if (!compatible) {failed_=true;Shutdown();return false;}
    uint32_t count{};
    std::array<XrViewConfigurationView,2> views{{{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
    if (!Check(xrEnumerateViewConfigurationViews(instance_,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,views.data()),"stereo_views") || count!=2) {failed_=true;Shutdown();return false;}
    for (unsigned eye=0;eye<2;++eye) Log("eye index=%u recommended=%ux%u samples=%u",eye,views[eye].recommendedImageRectWidth,views[eye].recommendedImageRectHeight,views[eye].recommendedSwapchainSampleCount);
    std::vector<XrEnvironmentBlendMode> blend;
    if (!Check(xrEnumerateEnvironmentBlendModes(instance_,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&count,nullptr),"blend_count")) {Shutdown();return false;}
    blend.resize(count);
    if (!count || !Check(xrEnumerateEnvironmentBlendModes(instance_,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,count,&count,blend.data()),"blend_modes") ||
        std::find(blend.begin(),blend.end(),XR_ENVIRONMENT_BLEND_MODE_OPAQUE)==blend.end()) {failed_=true;Shutdown();return false;}
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};binding.device=device;
    XrSessionCreateInfo session{XR_TYPE_SESSION_CREATE_INFO};session.next=&binding;session.systemId=system;
    if (!Check(xrCreateSession(instance_,&session,&session_),"create_game_session")) {Shutdown();return false;}
    device_=device;
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};space.poseInReferenceSpace.orientation.w=1;
    space.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!Check(xrCreateReferenceSpace(session_,&space,&local_),"create_local_space")) {Shutdown();return false;}
    space.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Check(xrCreateReferenceSpace(session_,&space,&head_),"create_head_space")) {Shutdown();return false;}
    const bool performance=performance_.Initialize(session_,head_,local_,device,output);
    Log("session initialized=1 images_submitted=0 performance_display=%u",performance);return true;
}
bool XrSessionProbe::Poll() {
    if (!OnThread() || !instance_ || failed_) return false;
    for (unsigned i=0;i<32;++i) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};const auto result=xrPollEvent(instance_,&event);
        if (result==XR_EVENT_UNAVAILABLE) return running_;
        if (!Check(result,"poll_event")) return false;
        if (event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& state=*reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (state.session!=session_) {failed_=true;return false;}
            state_=state.state;
            if (!Visible()) sampledVisible_=false;
            if (!Focused()) sampledFocused_=false;
            Log("session state=%d",state.state);
            if (state.state==XR_SESSION_STATE_READY && !running_) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};begin.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                running_=Check(xrBeginSession(session_,&begin),"begin_session");
            } else if (state.state==XR_SESSION_STATE_STOPPING) {
                if (frameOpen_) EndFrame();
                if (running_) Check(xrEndSession(session_),"end_session");
                running_=false;
            } else if (state.state==XR_SESSION_STATE_EXITING || state.state==XR_SESSION_STATE_LOSS_PENDING) {
                failed_=true;running_=false;return false;
            }
        } else if (event.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {failed_=true;return false;}
    }
    failed_=true;Log("session refused reason=event_flood");return false;
}
bool XrSessionProbe::InitializeImages(NativeEyeDraw draw,unsigned width,unsigned height) {
    if (!OnThread() || !session_ || failed_ || !draw || imagesAttempted_ || frameOpen_ || frame_) return false;
    imagesAttempted_=true;imagesReady_=images_.Initialize(session_,device_.Get(),draw,width,height);
    Log("images initialized=%u width=%u height=%u failure=%s result=%d",imagesReady_,width,height,images_.Failure()?images_.Failure():"none",images_.LastResult());
    if (!imagesReady_) failed_=true;
    return imagesReady_;
}
bool XrSessionProbe::BeginFrame(uint64_t frame) {
    if (!OnThread() || !running_ || failed_ || frameOpen_ || frame<=frame_) return false;
    frame_=frame;
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};XrFrameState state{XR_TYPE_FRAME_STATE};
    if (!Check(xrWaitFrame(session_,&wait,&state),"wait_frame")) return false;
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};const auto begun=xrBeginFrame(session_,&begin);
    if (XR_FAILED(begun)) return Check(begun,"begin_frame");
    frameOpen_=true;displayTime_=state.predictedDisplayTime;
    if (!Check(begun,"begin_frame")) {EndFrame();return false;}
    located_={};located_.frame=frame;located_.displayTime=displayTime_;located_.shouldRender=state.shouldRender==XR_TRUE;
    // A READY warmup can transiently report shouldRender before the session
    // becomes visible. Require a fresh eligible sample in a visible state.
    sampledVisible_=Visible();
    sampledFocused_=Focused();
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime=displayTime_;locate.space=local_;
    XrViewState viewState{XR_TYPE_VIEW_STATE};uint32_t count{};
    auto& views=located_.eyes;
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    bool valid=Check(xrLocateViews(session_,&locate,&viewState,2,&count,views.data()),"locate_views") && count==2 &&
        Check(xrLocateSpace(head_,local_,displayTime_,&head),"locate_head");
    if (count!=2) failed_=true;
    constexpr XrViewStateFlags viewFlags=XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_TRACKED_BIT|XR_VIEW_STATE_POSITION_TRACKED_BIT;
    constexpr XrSpaceLocationFlags headFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT|XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    valid=valid && (viewState.viewStateFlags&viewFlags)==viewFlags && (head.locationFlags&headFlags)==headFlags;
    located_.tracked=valid;located_.head=head.pose;
    Log("sample tracked=%u should_render=%u display_time=%lld period=%lld view_flags=%llu head_flags=%llu",
        valid,state.shouldRender,displayTime_,state.predictedDisplayPeriod,viewState.viewStateFlags,head.locationFlags);
    auto pose=[&](const char* kind,unsigned eye,const XrPosef& p) {
        Log("pose kind=%s eye=%u qx=%.9g qy=%.9g qz=%.9g qw=%.9g x=%.9g y=%.9g z=%.9g",kind,eye,
            p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w,p.position.x,p.position.y,p.position.z);
    };
    if (valid) {
        pose("head",0,head.pose);
        for (unsigned i=0;i<2;++i) {
            pose("eye",i,views[i].pose);const auto& f=views[i].fov;
            Log("fov eye=%u left=%.9g right=%.9g down=%.9g up=%.9g",i,f.angleLeft,f.angleRight,f.angleDown,f.angleUp);
        }
        ++samples_;
    }
    if (failed_) {EndFrame();return false;}
    return true;
}
bool XrSessionProbe::Sample(uint64_t frame) {
    if (!BeginFrame(frame)) return false;
    const bool tracked=located_.tracked;
    const bool ended=EndFrame();return tracked && ended;
}
bool XrSessionProbe::FinishFrame(uint64_t frame,bool rendererAccepted,ID3D11Texture2D* left,ID3D11Texture2D* right) {
    if (!OnThread() || !frameOpen_) return false;
    if (frame!=frame_) {failed_=true;EndFrame();return false;}
    const XrCompositionLayerProjection* layer=nullptr;
    if (rendererAccepted && located_.tracked && located_.shouldRender && !failed_) {
        if (!imagesReady_ || !images_.Copy(frame,local_,located_.eyes,left,right)) {
            failed_=true;
            Log("images copy=0 failure=%s result=%d",images_.Failure()?images_.Failure():"not_initialized",images_.LastResult());
        } else layer=images_.Projection(frame);
    }
    const bool ended=EndFrame(reinterpret_cast<const XrCompositionLayerBaseHeader*>(layer));return ended && !failed_;
}
bool XrSessionProbe::PresentScreen(uint64_t frame,ID3D11Texture2D* backbuffer) {
    if (!BeginFrame(frame)) return false;
    const XrCompositionLayerQuad* layer=nullptr;
    if (Visible() && located_.tracked && located_.shouldRender && !failed_) {
        if (!screenAnchorReady_) {
            screenAnchorReady_=LevelScreenAnchor(located_.head,screenAnchor_);
            if (screenAnchorReady_) Log("screen anchor local=1 yaw_only=1 x=%.9g y=%.9g z=%.9g qy=%.9g qw=%.9g",
                screenAnchor_.position.x,screenAnchor_.position.y,screenAnchor_.position.z,screenAnchor_.orientation.y,screenAnchor_.orientation.w);
        }
        if (!imagesReady_ || !screenAnchorReady_ || !images_.CopyScreen(frame,local_,screenAnchor_,backbuffer)) {
            failed_=true;
            Log("screen copy=0 failure=%s result=%d",images_.Failure()?images_.Failure():"not_initialized",images_.LastResult());
        } else layer=images_.Screen(frame);
    }
    const bool ended=EndFrame(reinterpret_cast<const XrCompositionLayerBaseHeader*>(layer));return ended && !failed_;
}
bool XrSessionProbe::EndFrame(const XrCompositionLayerBaseHeader* layer) {
    if (!OnThread() || !frameOpen_) return false;
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=displayTime_;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    std::array<const XrCompositionLayerBaseHeader*,2> layers{layer,nullptr};
    if(layer && performance_.Enabled()) {
        if(!performance_.FollowsView() && !screenAnchorReady_)
            screenAnchorReady_=LevelScreenAnchor(located_.head,screenAnchor_);
        layers[1]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(performance_.Layer(GetTickCount64(),screenAnchorReady_?&screenAnchor_:nullptr));
    }
    end.layerCount=layer?(layers[1]?2:1):0;end.layers=layer?layers.data():nullptr;
    const bool screen=layer && layer->type==XR_TYPE_COMPOSITION_LAYER_QUAD;
    const bool ok=Check(xrEndFrame(session_,&end),layer?(screen?"end_frame_screen":"end_frame_projection"):"end_frame_zero_layers");
    if (ok && layer) { if(screen)++screensSubmitted_;else ++imagesSubmitted_;performance_.Submitted(GetTickCount64()); }
    if (layer) Log("%s submitted=%u frame=%llu display_time=%lld",screen?"screen":"images",ok,frame_,displayTime_);
    frameOpen_=false;return ok;
}
void XrSessionProbe::Shutdown() {
    if (!OnThread()) return;
    if (frameOpen_) EndFrame();
    if (!performance_.Shutdown()) {
        failed_=true;Log("session shutdown=0 reason=performance_images_pending");return;
    }
    if (imagesAttempted_ && !images_.Shutdown()) {
        failed_=true;Log("session shutdown=0 reason=images_pending failure=%s",images_.Failure()?images_.Failure():"unknown");return;
    }
    imagesReady_=false;
    if (head_) {Check(xrDestroySpace(head_),"destroy_head_space");head_=XR_NULL_HANDLE;}
    if (local_) {Check(xrDestroySpace(local_),"destroy_local_space");local_=XR_NULL_HANDLE;}
    // DestroySession also terminates a running session; it does not require
    // forcing SteamVR down or blocking the game while waiting for STOPPING.
    if (session_) {Check(xrDestroySession(session_),"destroy_session");session_=XR_NULL_HANDLE;}
    running_=false;
    if (instance_) {Check(xrDestroyInstance(instance_),"destroy_instance");instance_=XR_NULL_HANDLE;}
    device_.Reset();Log("session shutdown=1 samples=%u failed=%u images_submitted=%u screens_submitted=%u",samples_,failed_,imagesSubmitted_,screensSubmitted_);
    log_.FlushNow();
}
}
