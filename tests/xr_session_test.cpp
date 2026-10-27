#include "xr_session.h"
#include <dxgi.h>
#include <cstdio>
#include <thread>
#include <cmath>
using Microsoft::WRL::ComPtr;
namespace {
ID3D11Device* expectedDevice{};
LUID adapterLuid{};
XrSessionState eventState=XR_SESSION_STATE_READY;
bool invalidTracking{},badAdapter{},badSpace{},badLocate{},discard{},missingFunction{},protocolError{};
unsigned instances{},sessions{},spaces{},waits{},ends{},phase{};
std::array<ComPtr<ID3D11Texture2D>,3> eyeImages;
std::array<unsigned,3> imagePhases{};
unsigned swapchains{},projectionEnds{},screenEnds{},performanceEnds{},performanceAcquires{},lastLayerCount{};
bool performanceCreateFail{},performanceAcquireFail{},performanceTimeout{};
bool fixedPerformance{};
XrPosef lastPerformancePose{};
bool hidden{};
const char* runtimeName="SteamVR/test";
std::wstring expectedRuntimePath;
unsigned recommendedWidth=100,recommendedHeight=120;
XrPosef headPose{{0,0,0,1},{0,0,0}},lastScreenPose{};
XrQuaternionf Multiply(const XrQuaternionf& a,const XrQuaternionf& b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
XrQuaternionf Orientation(float yaw,float pitch,float roll) {
    return Multiply(Multiply({0,std::sin(yaw/2),0,std::cos(yaw/2)},
        {std::sin(pitch/2),0,0,std::cos(pitch/2)}),{0,0,std::sin(roll/2),std::cos(roll/2)});
}
bool Near(float a,float b) {return std::abs(a-b)<1e-5f;}
void STDMETHODCALLTYPE DrawNative(ID3D11DeviceContext* c,UINT count,UINT first) {c->Draw(count,first);}
void Reset() {
    eventState=XR_SESSION_STATE_READY;invalidTracking=badAdapter=badSpace=badLocate=discard=missingFunction=protocolError=false;
    instances=sessions=spaces=waits=ends=phase=0;
    eyeImages={};imagePhases={};swapchains=projectionEnds=screenEnds=0;hidden=false;
    performanceEnds=performanceAcquires=lastLayerCount=0;
    performanceCreateFail=performanceAcquireFail=performanceTimeout=false;
    fixedPerformance=false;lastPerformancePose={};
    headPose={{0,0,0,1},{0,0,0}};lastScreenPose={};
}
XrResult XRAPI_CALL Requirements(XrInstance,XrSystemId,XrGraphicsRequirementsD3D11KHR* r) {
    r->adapterLuid=adapterLuid;if(badAdapter) r->adapterLuid.LowPart^=1;
    r->minFeatureLevel=D3D_FEATURE_LEVEL_11_0;return XR_SUCCESS;
}
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo*,XrInstance* out) {
    wchar_t selected[1024]{};GetEnvironmentVariableW(L"XR_RUNTIME_JSON",selected,1024);
    if (expectedRuntimePath!=selected) protocolError=true;
    *out=1;++instances;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance,XrInstanceProperties* out) {strcpy_s(out->runtimeName,runtimeName);return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrGetSystem(XrInstance,const XrSystemGetInfo*,XrSystemId* out) {*out=2;return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance,const char* name,PFN_xrVoidFunction* out) {
    if(strcmp(name,"xrGetD3D11GraphicsRequirementsKHR")) return XR_ERROR_FUNCTION_UNSUPPORTED;
    *out=missingFunction?nullptr:reinterpret_cast<PFN_xrVoidFunction>(Requirements);return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(XrInstance,XrSystemId,XrViewConfigurationType,uint32_t capacity,uint32_t* count,XrViewConfigurationView* out) {
    *count=2;if(capacity<2) return XR_ERROR_SIZE_INSUFFICIENT;
    for(unsigned i=0;i<2;++i) {out[i].recommendedImageRectWidth=recommendedWidth;out[i].recommendedImageRectHeight=recommendedHeight;out[i].recommendedSwapchainSampleCount=1;out[i].maxImageRectWidth=8192;out[i].maxImageRectHeight=8192;}
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateEnvironmentBlendModes(XrInstance,XrSystemId,XrViewConfigurationType,uint32_t capacity,uint32_t* count,XrEnvironmentBlendMode* out) {
    *count=1;if(capacity) out[0]=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateSession(XrInstance,const XrSessionCreateInfo* info,XrSession* out) {
    const auto& binding=*static_cast<const XrGraphicsBindingD3D11KHR*>(info->next);
    if(binding.type!=XR_TYPE_GRAPHICS_BINDING_D3D11_KHR || binding.device!=expectedDevice) {protocolError=true;return XR_ERROR_GRAPHICS_DEVICE_INVALID;}
    *out=3;++sessions;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession,const XrReferenceSpaceCreateInfo* info,XrSpace* out) {
    if(badSpace && info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_VIEW) return XR_ERROR_RUNTIME_FAILURE;
    *out=info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL?4:5;++spaces;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrPollEvent(XrInstance,XrEventDataBuffer* out) {
    if(eventState==XR_SESSION_STATE_UNKNOWN) return XR_EVENT_UNAVAILABLE;
    XrEventDataSessionStateChanged event{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};event.session=3;event.state=eventState;
    memcpy(out,&event,sizeof(event));eventState=XR_SESSION_STATE_UNKNOWN;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrBeginSession(XrSession,const XrSessionBeginInfo*) {return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrEndSession(XrSession) {if(phase) protocolError=true;return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrWaitFrame(XrSession,const XrFrameWaitInfo*,XrFrameState* out) {
    if(phase) protocolError=true;phase=1;++waits;out->predictedDisplayTime=12345+waits;out->predictedDisplayPeriod=100;out->shouldRender=hidden?XR_FALSE:XR_TRUE;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrBeginFrame(XrSession,const XrFrameBeginInfo*) {
    if(phase!=1) protocolError=true;phase=2;return discard?XR_FRAME_DISCARDED:XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrLocateViews(XrSession,const XrViewLocateInfo* info,XrViewState* state,uint32_t,uint32_t* count,XrView* out) {
    if(phase!=2 || info->displayTime!=12345+waits || info->space!=4) protocolError=true;
    *count=2;state->viewStateFlags=invalidTracking?0:15;
    for(unsigned i=0;i<2;++i) {out[i].pose.orientation.w=1;out[i].pose.position.x=i?.032f:-.032f;out[i].fov={-.8f,.8f,.7f,-.7f};}
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace space,XrSpace base,XrTime time,XrSpaceLocation* out) {
    if(phase!=2 || time!=12345+waits || space!=5 || base!=4) protocolError=true;
    if(badLocate) return XR_ERROR_RUNTIME_FAILURE;
    out->locationFlags=15;out->pose=headPose;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEndFrame(XrSession,const XrFrameEndInfo* info) {
    if(phase!=2 || info->displayTime!=12345+waits || info->environmentBlendMode!=XR_ENVIRONMENT_BLEND_MODE_OPAQUE) protocolError=true;
    lastLayerCount=info->layerCount;
    if (info->layerCount) {
        if (info->layerCount>2 || !info->layers || hidden || invalidTracking) protocolError=true;
        else if(info->layers[0]->type==XR_TYPE_COMPOSITION_LAYER_QUAD) {
            const auto* q=reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[0]);
            if(q->space!=4 || q->eyeVisibility!=XR_EYE_VISIBILITY_BOTH || q->subImage.swapchain!=10 ||
                q->pose.orientation.x!=0 || q->pose.orientation.z!=0 || !Near(q->pose.orientation.y*q->pose.orientation.y+q->pose.orientation.w*q->pose.orientation.w,1) ||
                q->size.width!=2.4f || q->size.height<=0 || imagePhases[0] || imagePhases[1]) protocolError=true;
            lastScreenPose=q->pose;
            ++screenEnds;
        } else {
            const auto* p=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
            if (p->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION || p->space!=4 || p->viewCount!=2 || !p->views || imagePhases[0] || imagePhases[1]) protocolError=true;
            else for (unsigned i=0;i<2;++i) if (p->views[i].subImage.swapchain!=10+i || p->views[i].pose.position.x!=(i?.032f:-.032f)) protocolError=true;
        }
        if(info->layers && info->layers[0]->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION) ++projectionEnds;
        if(info->layerCount==2 && info->layers) {
            const auto* meter=reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[1]);
            if(!meter || meter->type!=XR_TYPE_COMPOSITION_LAYER_QUAD || meter->space!=(fixedPerformance?4:5) ||
                meter->eyeVisibility!=XR_EYE_VISIBILITY_BOTH || meter->subImage.swapchain!=12 || !eyeImages[2] ||
                meter->subImage.imageRect.extent.width!=512 || meter->subImage.imageRect.extent.height!=64 ||
                !Near(meter->size.width,fixedPerformance?.8f:.6f) || !Near(meter->size.height,fixedPerformance?.1f:.075f) ||
                meter->pose.orientation.x!=0 || meter->pose.orientation.z!=0 || imagePhases[2]==2)
                protocolError=true;
            if(!fixedPerformance && (meter->pose.orientation.y!=0 || meter->pose.orientation.w!=1 ||
                meter->pose.position.x!=0 || !Near(meter->pose.position.y,-.45f) || !Near(meter->pose.position.z,-1.5f)))protocolError=true;
            lastPerformancePose=meter->pose;
            ++performanceEnds;
        }
    } else if (info->layers) protocolError=true;
    phase=0;++ends;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession,uint32_t capacity,uint32_t* count,int64_t* out) {
    *count=1;if(capacity) out[0]=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* out) {
    // The optional panel is created before eye images. Give it a separate,
    // stable handle instead of making eye identity depend on creation order.
    const bool meter=info->usageFlags==XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    if(meter && performanceCreateFail) return XR_ERROR_RUNTIME_FAILURE;
    const unsigned eye=meter?2:(!eyeImages[0]?0:1);
    if(eyeImages[eye] || swapchains>=eyeImages.size()) return XR_ERROR_LIMIT_REACHED;
    if(meter && (info->width!=512 || info->height!=64)) protocolError=true;
    D3D11_TEXTURE2D_DESC d{};d.Width=info->width;d.Height=info->height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=static_cast<DXGI_FORMAT>(info->format);d.BindFlags=D3D11_BIND_RENDER_TARGET;
    if(FAILED(expectedDevice->CreateTexture2D(&d,nullptr,&eyeImages[eye]))) return XR_ERROR_RUNTIME_FAILURE;
    *out=10+eye;++swapchains;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain chain,uint32_t capacity,uint32_t* count,XrSwapchainImageBaseHeader* out) {
    if(chain<10 || chain>12 || !eyeImages[chain-10]) {protocolError=true;return XR_ERROR_HANDLE_INVALID;}
    *count=1;if(capacity) reinterpret_cast<XrSwapchainImageD3D11KHR*>(out)->texture=eyeImages[chain-10].Get();return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain chain,const XrSwapchainImageAcquireInfo*,uint32_t* index) {
    if(chain<10 || chain>12 || !eyeImages[chain-10]) {protocolError=true;return XR_ERROR_HANDLE_INVALID;}
    if(phase!=2) protocolError=true;
    if(chain==12) {++performanceAcquires;if(performanceAcquireFail)return XR_ERROR_RUNTIME_FAILURE;}
    auto& p=imagePhases[chain-10];if(phase!=2 || p) protocolError=true;p=1;*index=0;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain chain,const XrSwapchainImageWaitInfo*) {
    if(chain<10 || chain>12 || !eyeImages[chain-10]) {protocolError=true;return XR_ERROR_HANDLE_INVALID;}
    auto& p=imagePhases[chain-10];if(p!=1) protocolError=true;
    if(chain==12 && performanceTimeout) return XR_TIMEOUT_EXPIRED;
    p=2;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain chain,const XrSwapchainImageReleaseInfo*) {
    if(chain<10 || chain>12 || !eyeImages[chain-10]) {protocolError=true;return XR_ERROR_HANDLE_INVALID;}
    auto& p=imagePhases[chain-10];if(p!=2) protocolError=true;p=0;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain chain) {
    if(chain<10 || chain>12 || !eyeImages[chain-10]) {protocolError=true;return XR_ERROR_HANDLE_INVALID;}
    if(imagePhases[chain-10] || !eyeImages[chain-10]) protocolError=true;
    eyeImages[chain-10].Reset();--swapchains;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace) {if(!spaces) protocolError=true;--spaces;return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySession(XrSession) {if(spaces || !sessions || phase || swapchains) protocolError=true;--sessions;return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance) {if(spaces || sessions || !instances) protocolError=true;--instances;return XR_SUCCESS;}

int main(int argc,char** argv) {
    if(argc!=2) return 1;
    auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    if(!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))) return 2;
    expectedDevice=device.Get();ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
    if(FAILED(device.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc))) return 3;
    adapterLuid=desc.AdapterLuid;
    const auto folder=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(folder);
    const auto performanceRequest=folder/L"performance-display.request";
    std::filesystem::remove(performanceRequest);
    const auto manifest=folder/L"steamxr_win32.json";std::ofstream(manifest)<<"test-only; fake runtime functions are linked";
    expectedRuntimePath=manifest.wstring();
    SetEnvironmentVariableW(L"XR_RUNTIME_JSON",L"preserve-test-value");
    const auto resolutionRequest=folder/L"headset-resolution.request",sceneRequest=folder/L"scene-size.request";
    recommendedWidth=800;recommendedHeight=900;
    for(unsigned test=0;test<6;++test) {
        Reset();grid2vr::XrSessionProbe probe;
        std::ofstream(resolutionRequest)<<(test==2?"801 900 50":test==3?"800 900 301":test==4?"800 900 50 extra":"800 900 50");
        std::ofstream(sceneRequest)<<(test==1?"800 900":"400 450");
        const bool initialized=probe.Initialize(device.Get(),manifest,folder);
        if(initialized!=(test==0 || test==5)) return 100;
        if(test==0 && !probe.InitializeImages(DrawNative,800,900)) return 101;
        if(test==5 && (probe.InitializeImages(DrawNative,400,450) || !probe.Failed())) return 102;
        probe.Shutdown();
        if(instances || sessions || spaces || protocolError) return 103;
    }
    std::filesystem::remove(resolutionRequest);std::filesystem::remove(sceneRequest);
    recommendedWidth=100;recommendedHeight=120;
    for(unsigned test=0;test<6;++test) {
        Reset();grid2vr::XrSessionProbe probe;
        badAdapter=test==1;badSpace=test==2;discard=test==3;missingFunction=test==5;
        const bool initialized=probe.Initialize(device.Get(),manifest,folder);
        wchar_t previous[64]{};GetEnvironmentVariableW(L"XR_RUNTIME_JSON",previous,64);
        if(wcscmp(previous,L"preserve-test-value") || initialized==(badAdapter||badSpace||missingFunction) || (!initialized && !probe.Failed())) return 4;
        if(initialized) {
            if(probe.Sample(1) || !probe.Poll()) return 5;
            bool otherThread=true;std::thread other([&]{otherThread=probe.Sample(1);});other.join();
            if(otherThread || waits) return 6;
            if(test==3) {if(probe.Sample(1) || !probe.Failed() || ends!=1) return 7;}
            else {
                if(!probe.Sample(1) || probe.Sample(1) || probe.Samples()!=1 || ends!=1) return 8;
                if(test==0) {
                    invalidTracking=true;if(probe.Sample(2) || probe.Samples()!=1 || ends!=2) return 9;
                    invalidTracking=false;badLocate=true;if(probe.Sample(3) || !probe.Failed() || ends!=3) return 10;
                } else {
                    eventState=XR_SESSION_STATE_STOPPING;if(probe.Poll() || probe.Running()) return 11;
                }
            }
        }
        probe.Shutdown();
        if(instances || sessions || spaces || phase || protocolError) return 12;
    }
    // Both providers use the same game-device/session lifecycle. A mismatched
    // provider identity or 64-bit VDXR manifest must still refuse and clean up.
    struct RuntimeCase { const wchar_t* filename; const char* name; bool accepted; };
    for (const auto test:{RuntimeCase{L"virtualdesktop-openxr-32.json","VirtualDesktopXR",true},
            RuntimeCase{L"virtualdesktop-openxr-32.json","SteamVR/test",false},
            RuntimeCase{L"steamxr_win32.json","VirtualDesktopXR",false},
            RuntimeCase{L"virtualdesktop-openxr.json","VirtualDesktopXR",false}}) {
        Reset();runtimeName=test.name;
        const auto selected=folder/test.filename;std::ofstream(selected)<<"fake linked runtime";
        expectedRuntimePath=selected.wstring();grid2vr::XrSessionProbe probe;
        if (probe.Initialize(device.Get(),selected,folder)!=test.accepted) return 40;
        if (test.accepted && (!probe.Poll() || !probe.Sample(1))) return 41;
        probe.Shutdown();
        wchar_t previous[64]{};GetEnvironmentVariableW(L"XR_RUNTIME_JSON",previous,64);
        if (wcscmp(previous,L"preserve-test-value") || instances || sessions || spaces || phase || protocolError) return 42;
    }
    runtimeName="VirtualDesktopXR";
    const auto vdxrManifest=folder/L"virtualdesktop-openxr-32.json";
    expectedRuntimePath=vdxrManifest.wstring();
    Reset();grid2vr::XrSessionProbe stereo;
    if (!stereo.Initialize(device.Get(),vdxrManifest,folder) || !stereo.InitializeImages(DrawNative,2,2) || !stereo.Poll()) return 13;
    D3D11_TEXTURE2D_DESC image{};image.Width=image.Height=2;image.MipLevels=image.ArraySize=image.SampleDesc.Count=1;
    image.Format=DXGI_FORMAT_R10G10B10A2_UNORM;image.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> left,right;
    if (FAILED(device->CreateTexture2D(&image,nullptr,&left)) || FAILED(device->CreateTexture2D(&image,nullptr,&right))) return 14;
    if (!stereo.BeginFrame(1) || !stereo.Views() || stereo.Views()->frame!=1 || stereo.Views()->displayTime!=12346 || !stereo.Views()->tracked ||
        stereo.Sample(2) || !stereo.FinishFrame(1,false) || projectionEnds || stereo.Views()) return 15;
    if (!stereo.BeginFrame(2) || !stereo.FinishFrame(2,true,left.Get(),right.Get()) || projectionEnds!=1 || stereo.Failed()) return 16;
    invalidTracking=true;
    if (!stereo.BeginFrame(3) || stereo.Views()->tracked || !stereo.FinishFrame(3,true,left.Get(),right.Get()) || projectionEnds!=1) return 17;
    invalidTracking=false;hidden=true;
    if (!stereo.BeginFrame(4) || stereo.Views()->shouldRender || !stereo.FinishFrame(4,true,left.Get(),right.Get()) || projectionEnds!=1) return 18;
    hidden=false;
    if (!stereo.BeginFrame(5) || stereo.FinishFrame(6,true,left.Get(),right.Get()) || !stereo.Failed() || ends!=5) return 19;
    stereo.Shutdown();
    if(instances || sessions || spaces || phase || swapchains || protocolError) return 20;
    runtimeName="SteamVR/test";expectedRuntimePath=manifest.wstring();
    Reset();grid2vr::XrSessionProbe warmup;
    if(!warmup.Closed()) return 43;
    if (!warmup.Initialize(device.Get(),manifest,folder) || !warmup.InitializeImages(DrawNative,2,2) || !warmup.Poll() || warmup.ReadyForViews()) return 21;
    invalidTracking=true;hidden=true;
    if (warmup.Sample(1) || warmup.ReadyForViews() || ends!=1 || projectionEnds) return 22;
    invalidTracking=false;
    if (!warmup.Sample(2) || warmup.ReadyForViews() || ends!=2 || projectionEnds) return 23;
    hidden=false;
    if (!warmup.Sample(3) || warmup.ReadyForViews() || ends!=3 || projectionEnds) return 24;
    // Native SteamVR witness: eligible READY sample, then SYNCHRONIZED and
    // shouldRender=false. Neither may arm the engine's first stereo group.
    eventState=XR_SESSION_STATE_SYNCHRONIZED;hidden=true;
    if (!warmup.Poll() || warmup.ReadyForViews() || !warmup.Sample(4) || warmup.ReadyForViews()) return 27;
    eventState=XR_SESSION_STATE_VISIBLE;hidden=false;
    if (!warmup.Poll() || warmup.ReadyForViews() || !warmup.Sample(5) || !warmup.ReadyForViews()) return 28;
    eventState=XR_SESSION_STATE_SYNCHRONIZED;
    if (!warmup.Poll() || warmup.ReadyForViews()) return 29;
    eventState=XR_SESSION_STATE_FOCUSED;
    if (!warmup.Poll() || warmup.ReadyForViews() || !warmup.Sample(6) || !warmup.ReadyForViews()) return 30;
    if (warmup.BeginFrame(6) || !warmup.BeginFrame(7) || warmup.ReadyForViews() ||
        !warmup.FinishFrame(7,true,left.Get(),right.Get()) || projectionEnds!=1) return 25;
    // SteamVR's dashboard leaves the game VISIBLE and tracked/shouldRender.
    // Sustained gameplay must wait for focus and a fresh focused warmup sample.
    if (!warmup.Focused() || !warmup.ReadyForViews(true)) return 31;
    eventState=XR_SESSION_STATE_VISIBLE;
    if (!warmup.Poll() || warmup.Focused() || !warmup.ReadyForViews() || warmup.ReadyForViews(true) ||
        !warmup.Sample(8) || warmup.ReadyForViews(true) || projectionEnds!=1) return 32;
    eventState=XR_SESSION_STATE_FOCUSED;
    if (!warmup.Poll() || !warmup.Focused() || warmup.ReadyForViews(true) ||
        !warmup.Sample(9) || !warmup.ReadyForViews(true)) return 33;
    if (!warmup.BeginFrame(10) || !warmup.FinishFrame(10,true,left.Get(),right.Get()) ||
        projectionEnds!=2 || warmup.Failed()) return 34;
    std::thread foreignShutdown([&]{warmup.Shutdown();});foreignShutdown.join();
    if(warmup.Closed() || !warmup.Running() || !instances || !sessions || !swapchains) return 44;
    warmup.Shutdown();
    if(!warmup.Closed() || warmup.ReadyForViews() || instances || sessions || spaces || phase || swapchains || protocolError) return 26;
    // First Present can keep the same XR session alive through startup, a
    // stereo race, a native menu, an invisible headset and stereo resumption.
    Reset();grid2vr::XrSessionProbe menus;
    if (!menus.Initialize(device.Get(),manifest,folder) || !menus.InitializeImages(DrawNative,2,2) || !menus.Poll()) return 45;
    if (!menus.PresentScreen(1,left.Get()) || menus.LastFrame()!=1 || screenEnds || menus.ReadyForViews(true)) return 46;
    eventState=XR_SESSION_STATE_FOCUSED;
    constexpr float pi=3.14159265358979323846f;
    headPose={Orientation(pi/2,pi/6,pi/4),{1,1.6f,3}};
    if (!menus.Poll() || !menus.PresentScreen(2,left.Get()) || screenEnds!=1 || !menus.ReadyForViews(true) ||
        menus.BeginFrame(2) || !menus.BeginFrame(3) || !menus.FinishFrame(3,true,left.Get(),right.Get()) || projectionEnds!=1) return 47;
    if (!Near(lastScreenPose.position.x,-1) || !Near(lastScreenPose.position.y,1.6f) || !Near(lastScreenPose.position.z,3) ||
        !Near(lastScreenPose.orientation.y,std::sqrt(.5f)) || !Near(lastScreenPose.orientation.w,std::sqrt(.5f))) return 52;
    const auto anchored=lastScreenPose;
    headPose={Orientation(-pi/3,-pi/9,pi*.4f),{8,2,-4}};
    if (!menus.PresentScreen(4,left.Get()) || screenEnds!=2 || projectionEnds!=1 || menus.Views() || menus.Failed()) return 48;
    if (memcmp(&anchored,&lastScreenPose,sizeof(anchored))) return 53;
    hidden=true;
    if (!menus.PresentScreen(5,left.Get()) || screenEnds!=2 || menus.Failed()) return 49;
    hidden=false;
    if (!menus.PresentScreen(6,left.Get()) || screenEnds!=3 || !menus.ReadyForViews(true) ||
        !menus.BeginFrame(7) || !menus.FinishFrame(7,true,left.Get(),right.Get()) || projectionEnds!=2 || menus.Failed() || waits!=ends) return 50;
    if (memcmp(&anchored,&lastScreenPose,sizeof(anchored))) return 54;
    std::thread foreignRecenter([&]{menus.ResetScreenAnchor();});foreignRecenter.join();
    if (!menus.PresentScreen(8,left.Get()) || memcmp(&anchored,&lastScreenPose,sizeof(anchored))) return 55;
    menus.ResetScreenAnchor();
    if (!menus.PresentScreen(9,left.Get()) || !Near(lastScreenPose.position.x,8+std::sqrt(3.f)) ||
        !Near(lastScreenPose.position.y,2) || !Near(lastScreenPose.position.z,-5) ||
        !Near(lastScreenPose.orientation.y,-.5f) || !Near(lastScreenPose.orientation.w,std::sqrt(.75f))) return 56;
    // A near-vertical initial view must still produce a level, finite screen.
    headPose={Orientation(0,pi/2,0),{0,1,0}};menus.ResetScreenAnchor();
    if (!menus.PresentScreen(10,left.Get()) || !Near(lastScreenPose.position.x,0) ||
        !Near(lastScreenPose.position.y,1) || !Near(lastScreenPose.position.z,-2) ||
        !Near(lastScreenPose.orientation.y,0) || !Near(lastScreenPose.orientation.w,1)) return 57;
    menus.Shutdown();
    if (!menus.Closed() || instances || sessions || spaces || phase || swapchains || protocolError) return 51;
    // Optional performance composition must leave the native screen/projection
    // first, use its own VIEW-space chain, and never submit without a base image.
    std::ofstream(performanceRequest)<<"1\n";
    for(unsigned test=0;test<4;++test) {
        Reset();grid2vr::XrSessionProbe metered;
        performanceCreateFail=test==1;performanceAcquireFail=test==2;performanceTimeout=test==3;
        if(!metered.Initialize(device.Get(),manifest,folder) || !metered.InitializeImages(DrawNative,2,2) || !metered.Poll()) return 58;
        eventState=XR_SESSION_STATE_FOCUSED;
        if(!metered.Poll() || !metered.BeginFrame(1) || !metered.FinishFrame(1,false) ||
            lastLayerCount || performanceAcquires || performanceEnds) return 59;
        if(!metered.PresentScreen(2,left.Get()) || screenEnds!=1 || metered.Failed()) return 60;
        if(test==0) {
            if(lastLayerCount!=2 || performanceEnds!=1 || performanceAcquires!=1) return 61;
        } else if(lastLayerCount!=1 || performanceEnds || performanceAcquires!=(test==1?0u:1u)) return 62;
        performanceTimeout=false;
        if(!metered.BeginFrame(3) || !metered.FinishFrame(3,true,left.Get(),right.Get()) ||
            projectionEnds!=1 || metered.Failed()) return 63;
        if(test==0 || test==3) {
            if(lastLayerCount!=2 || performanceEnds!=(test==0?2u:1u) ||
                performanceAcquires<1 || performanceAcquires>(test==0?2u:1u)) return 64;
        } else if(lastLayerCount!=1 || performanceEnds || performanceAcquires!=(test==1?0u:1u)) return 65;
        hidden=true;
        if(!metered.BeginFrame(4) || !metered.FinishFrame(4,true,left.Get(),right.Get()) || lastLayerCount || metered.Failed()) return 66;
        metered.Shutdown();
        if(!metered.Closed() || instances || sessions || spaces || phase || swapchains || protocolError ||
            eyeImages[0] || eyeImages[1] || eyeImages[2] || imagePhases[0] || imagePhases[1] || imagePhases[2]) return 67;
    }
    // Fixed placement is stable across head movement, tracking gaps and
    // projection/menu transitions, then moves only on explicit recenter.
    const auto fixedRequest=folder/L"performance-display-fixed.request";
    std::ofstream(fixedRequest)<<"1\n";
    Reset();fixedPerformance=true;grid2vr::XrSessionProbe fixed;
    if(!fixed.Initialize(device.Get(),manifest,folder) || !fixed.InitializeImages(DrawNative,2,2) || !fixed.Poll())return 68;
    eventState=XR_SESSION_STATE_FOCUSED;headPose={Orientation(pi/2,.3f,-.4f),{1,2,3}};
    if(!fixed.Poll() || !fixed.BeginFrame(1) || !fixed.FinishFrame(1,true,left.Get(),right.Get()) || lastLayerCount!=2 ||
        !Near(lastPerformancePose.position.x,-1) || !Near(lastPerformancePose.position.y,1.4f) || !Near(lastPerformancePose.position.z,3))return 69;
    const auto placed=lastPerformancePose;
    headPose={Orientation(-.7f,-.3f,.5f),{2,1,4}};
    if(!fixed.PresentScreen(2,left.Get()) || memcmp(&placed,&lastPerformancePose,sizeof(placed)))return 70;
    invalidTracking=true;
    if(!fixed.BeginFrame(3) || !fixed.FinishFrame(3,true,left.Get(),right.Get()) || lastLayerCount)return 71;
    invalidTracking=false;
    if(!fixed.BeginFrame(4) || !fixed.FinishFrame(4,true,left.Get(),right.Get()) || memcmp(&placed,&lastPerformancePose,sizeof(placed)))return 72;
    fixed.ResetScreenAnchor();headPose={{0,0,0,1},{0,1,0}};
    if(!fixed.BeginFrame(5) || !fixed.FinishFrame(5,true,left.Get(),right.Get()) ||
        !Near(lastPerformancePose.position.x,0) || !Near(lastPerformancePose.position.y,.4f) || !Near(lastPerformancePose.position.z,-2) ||
        !Near(lastPerformancePose.orientation.y,0) || !Near(lastPerformancePose.orientation.w,1))return 73;
    if(!fixed.PresentScreen(6,left.Get()) || protocolError || fixed.Failed())return 74;
    fixed.Shutdown();
    if(!fixed.Closed() || instances || sessions || spaces || phase || swapchains || protocolError)return 75;
    std::filesystem::remove(fixedRequest);
    std::filesystem::remove(performanceRequest);
    puts("Game-device/frame identity, stereo/menu transitions, following/fixed performance display, recenter, failures and cleanup pass.");
}
