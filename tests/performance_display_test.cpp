#include "performance_display.h"
#include <cstdio>
#include <fstream>
#include <limits>
using Microsoft::WRL::ComPtr;
namespace {
ID3D11Device* device{};ComPtr<ID3D11Texture2D> image;
bool Near(float a,float b) {return std::abs(a-b)<.0001f;}
unsigned phase{},acquires{},releases{};bool error{},timeout{},createFail{};
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession,uint32_t n,uint32_t* count,int64_t* out) {
    *count=1;if(n)out[0]=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* out) {
    if(createFail)return XR_ERROR_RUNTIME_FAILURE;
    if(info->width!=512 || info->height!=64 || info->usageFlags!=XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)error=true;
    D3D11_TEXTURE2D_DESC d{};d.Width=info->width;d.Height=info->height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if(FAILED(device->CreateTexture2D(&d,nullptr,&image)))return XR_ERROR_RUNTIME_FAILURE;
    *out=10;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain,uint32_t n,uint32_t* count,XrSwapchainImageBaseHeader* out) {
    *count=1;if(n)reinterpret_cast<XrSwapchainImageD3D11KHR*>(out)->texture=image.Get();return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain,const XrSwapchainImageAcquireInfo*,uint32_t* out) {
    if(phase)error=true;phase=1;++acquires;*out=0;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain,const XrSwapchainImageWaitInfo*) {
    if(phase!=1)error=true;if(timeout)return XR_TIMEOUT_EXPIRED;phase=2;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain,const XrSwapchainImageReleaseInfo*) {
    if(phase!=2)error=true;phase=0;++releases;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain) {if(phase)error=true;image.Reset();return XR_SUCCESS;}
int main(int argc,char** argv) {
    if(argc!=2)return 1;
    grid2vr::SubmittedFrameRate rate;rate.Frame(1000);for(unsigned i=1;i<=60;++i)rate.Frame(1000+i*1000/60);
    if(std::abs(rate.Fps()-60)>.01)return 2;
    grid2vr::GpuEngineUsage usage;const auto adapter=L"luid_0x00000000_0x00001234";
    usage.Add(L"pid_1_luid_0x00000000_0x00001234_phys_0_eng_0_engtype_3D",adapter,40,true);
    usage.Add(L"pid_2_luid_0x00000000_0x00001234_phys_0_eng_0_engtype_3D",adapter,30,true);
    usage.Add(L"pid_1_luid_0x00000000_0x00001234_phys_0_eng_1_engtype_Copy",adapter,50,true);
    usage.Add(L"pid_1_luid_0x00000000_0x00005678_phys_0_eng_0_engtype_3D",adapter,100,true);
    usage.Add(L"pid_3_luid_0x00000000_0x00001234_phys_0_eng_0_engtype_3D",adapter,90,false);
    if(usage.Percent()!=70 || grid2vr::GpuEngineUsage{}.Percent()!=-1)return 3;
    const auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    ComPtr<ID3D11Device> owner;ComPtr<ID3D11DeviceContext> context;
    if(!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&owner,nullptr,&context)))return 4;
    device=owner.Get();const auto path=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(path);
    std::filesystem::remove(path/L"performance-display.request");
    std::filesystem::remove(path/L"performance-display-fixed.request");
    std::filesystem::remove(path/L"vr-hud.request");
    grid2vr::PerformanceDisplay off;if(off.Initialize(3,5,4,device,path) || !off.Shutdown())return 5;
    std::ofstream(path/L"performance-display.request")<<"1\n";
    grid2vr::PerformanceDisplay panel;if(!panel.Initialize(3,5,4,device,path))return 6;
    const D3D11_QUERY_DESC predDesc{D3D11_QUERY_OCCLUSION_PREDICATE,0};ComPtr<ID3D11Predicate> pred;
    if(FAILED(device->CreatePredicate(&predDesc,&pred)))return 7;
    context->SetPredication(pred.Get(),TRUE);
    timeout=true;if(panel.Layer(1000) || !panel.Enabled() || acquires!=1 || releases)return 8;
    timeout=false;const auto* layer=panel.Layer(1010);
    if(!layer || layer->space!=5 || layer->eyeVisibility!=XR_EYE_VISIBILITY_BOTH || acquires!=1 || releases!=1)return 9;
    ComPtr<ID3D11Predicate> restored;BOOL value{};context->GetPredication(&restored,&value);
    if(restored.Get()!=pred.Get() || !value)return 10;
    context->SetPredication(nullptr,FALSE);
    D3D11_TEXTURE2D_DESC d{};image->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> read;if(FAILED(device->CreateTexture2D(&d,nullptr,&read)))return 11;
    context->CopyResource(read.Get(),image.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped)))return 12;
    unsigned bright{};for(unsigned y=0;y<64;++y)for(unsigned x=0;x<512;++x)
        if(reinterpret_cast<const uint32_t*>(static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch)[x]==0xffeeeeee)++bright;
    context->Unmap(read.Get(),0);if(bright<100)return 13;
    panel.Submitted(1010);panel.Submitted(2010);if(!panel.Layer(2010) || acquires!=2 || releases!=2)return 14;
    if(!panel.Shutdown() || image || error)return 15;
    std::ofstream(path/L"performance-display-fixed.request")<<"1\n";
    grid2vr::PerformanceDisplay fixed;
    if(!fixed.Initialize(3,5,4,device,path) || fixed.FollowsView() || fixed.Layer(3000))return 17;
    const XrPosef anchor{{0,.70710678f,0,.70710678f},{-2,1,0}};
    // Without a HUD request the stereo HUD uses its 2 m default: the fixed
    // strip comes 5% nearer than that plane, along the line from the head
    // (0,1,0) to the anchor, at the same angular size and lower placement.
    const auto* placed=fixed.Layer(3001,&anchor);
    if(!placed || placed->space!=4 || !Near(placed->pose.position.y,1-.6f*.95f) ||
       !Near(placed->pose.position.x,-1.9f) || !Near(placed->pose.position.z,0) || placed->pose.orientation.y!=anchor.orientation.y ||
       !Near(placed->size.width,.76f) || !Near(placed->size.height,.095f) || !Near(fixed.Distance(),1.9f) || !fixed.Shutdown() || error)return 18;
    // A 1 m HUD (the launcher default) and a far one; an invalid request keeps the default.
    struct Case {const char* hud;float distance,x,y,width;};
    for(const auto& c:{Case{"1 0 0\n",.95f,-.95f,1-.6f*.475f,.38f},Case{"3 0 0\n",2,-2,.4f,.8f},Case{"x\n",1.9f,-1.9f,1-.6f*.95f,.76f}}) {
        std::ofstream(path/L"vr-hud.request")<<c.hud;
        grid2vr::PerformanceDisplay hud;if(!hud.Initialize(3,5,4,device,path))return 19;
        const auto* l=hud.Layer(3002,&anchor);
        if(!l || !Near(hud.Distance(),c.distance) || !Near(l->pose.position.x,c.x) || !Near(l->pose.position.y,c.y) ||
           !Near(l->pose.position.z,0) || !Near(l->size.width,c.width) || !hud.Shutdown() || error)return 20;
    }
    std::filesystem::remove(path/L"performance-display-fixed.request");
    // Following the view: 1.5 m and 45 cm down, unless the HUD is nearer than 1.58 m.
    for(const auto& c:{Case{"1 1 0\n",.95f,0,-.285f,.38f},Case{"1.5 1 0\n",1.425f,0,-.4275f,.57f},
                       Case{"2 1 0\n",1.5f,0,-.45f,.6f},Case{"20 0 0\n",1.5f,0,-.45f,.6f}}) {
        std::ofstream(path/L"vr-hud.request")<<c.hud;
        grid2vr::PerformanceDisplay hud;if(!hud.Initialize(3,5,4,device,path) || !hud.FollowsView())return 21;
        const auto* l=hud.Layer(3003);
        if(!l || l->space!=5 || !Near(hud.Distance(),c.distance) || !Near(l->pose.position.z,-c.distance) || l->pose.position.x!=0 ||
           !Near(l->pose.position.y,c.y) || !Near(l->size.width,c.width) || !Near(l->size.height,c.width/8) || !hud.Shutdown() || error)return 22;
    }
    std::filesystem::remove(path/L"vr-hud.request");
    createFail=true;grid2vr::PerformanceDisplay failed;if(failed.Initialize(3,5,4,device,path) || !failed.Shutdown())return 16;
    std::filesystem::remove(path/L"performance-display.request");
    puts("Performance panel: aggregation, FPS, pixels, opt-out, timeout retry, state restoration, in front of the HUD and shutdown passed.");return 0;
}
