#include "xr_eye_images.h"
#include <map>
#include <cstdio>
using Microsoft::WRL::ComPtr;
namespace {
struct Chain {std::array<ComPtr<ID3D11Texture2D>,2> textures;unsigned phase{};};
std::map<XrSwapchain,Chain> chains;
ID3D11Device* gameDevice{};
unsigned creates{},acquires{},releases{},draws{};
int failure{};bool protocolError{};
void Reset(int mode) {chains.clear();creates=acquires=releases=draws=0;failure=mode;protocolError=false;}
void STDMETHODCALLTYPE DrawNative(ID3D11DeviceContext* context,UINT count,UINT start) {
    if (!count || start || (chains[XrSwapchain(10)].phase!=2 && chains[XrSwapchain(11)].phase!=2)) protocolError=true;
    ++draws;context->Draw(count,start);
}
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession session,uint32_t capacity,uint32_t* count,int64_t* out) {
    if (session!=3) protocolError=true;*count=1;
    if (capacity) out[0]=failure==1?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* out) {
    ++creates;if (failure==2 && creates==2) return XR_ERROR_RUNTIME_FAILURE;
    if (info->type!=XR_TYPE_SWAPCHAIN_CREATE_INFO || info->sampleCount!=1 || info->arraySize!=1 || info->mipCount!=1 ||
        info->usageFlags!=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT || info->format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) protocolError=true;
    *out=9+creates;auto& chain=chains[*out];
    D3D11_TEXTURE2D_DESC desc{};desc.Width=info->width;desc.Height=info->height;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=static_cast<DXGI_FORMAT>(info->format);desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    for (auto& texture:chain.textures) if (FAILED(gameDevice->CreateTexture2D(&desc,nullptr,&texture))) return XR_ERROR_RUNTIME_FAILURE;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain chain,uint32_t capacity,uint32_t* count,XrSwapchainImageBaseHeader* images) {
    *count=2;if (!capacity) return XR_SUCCESS;
    if (capacity!=2 || !chains.contains(chain)) {protocolError=true;return XR_ERROR_VALIDATION_FAILURE;}
    auto* out=reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
    for (unsigned i=0;i<2;++i) {if (out[i].type!=XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) protocolError=true;out[i].texture=chains[chain].textures[i].Get();}
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain chain,const XrSwapchainImageAcquireInfo*,uint32_t* index) {
    if ((failure==3 && chain==11) || (failure==7 && chain==10)) return XR_ERROR_RUNTIME_FAILURE;
    auto& c=chains[chain];if (c.phase) {protocolError=true;return XR_ERROR_CALL_ORDER_INVALID;}
    ++acquires;c.phase=1;*index=failure==5?9:0;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain chain,const XrSwapchainImageWaitInfo* info) {
    auto& c=chains[chain];if (c.phase!=1 || info->timeout!=100000000) {protocolError=true;return XR_ERROR_CALL_ORDER_INVALID;}
    if ((failure==4 && chain==11) || (failure==8 && chain==10)) return XR_TIMEOUT_EXPIRED;
    c.phase=2;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain chain,const XrSwapchainImageReleaseInfo*) {
    auto& c=chains[chain];if (c.phase!=2) {protocolError=true;return XR_ERROR_CALL_ORDER_INVALID;}
    ++releases;if ((failure==6 && chain==11) || (failure==9 && chain==10)) return XR_ERROR_RUNTIME_FAILURE;
    c.phase=0;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain chain) {
    if (!chains.erase(chain)) {protocolError=true;return XR_ERROR_HANDLE_INVALID;}
    return XR_SUCCESS;
}
int main() {
    const auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    if (!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))) return 1;
    gameDevice=device.Get();
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=2;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R10G10B10A2_UNORM;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    std::array<ComPtr<ID3D11Texture2D>,2> sources;
    for (unsigned eye=0;eye<2;++eye) {
        const UINT color=eye?0xfff00000:0xc00003ff;const UINT pixels[]{color,color,color,color};D3D11_SUBRESOURCE_DATA data{pixels,8,0};
        if (FAILED(device->CreateTexture2D(&desc,&data,&sources[eye]))) return 2;
    }
    std::array<XrView,2> views{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};
    for (unsigned eye=0;eye<2;++eye) {views[eye].pose.orientation.w=1;views[eye].pose.position.x=eye?.032f:-.032f;views[eye].fov={-.8f,.8f,.7f,-.7f};}
    for (int mode=0;mode<=6;++mode) {
        Reset(mode);grid2vr::XrEyeImages images;
        const bool initialized=images.Initialize(3,device.Get(),DrawNative,2,2);
        if (initialized!=(mode!=1 && mode!=2)) return 3;
        if (initialized) {
            if (images.Projection(1)) return 4;
            const bool copied=images.Copy(1,4,views,sources[0].Get(),sources[1].Get());
            if (copied!=(mode==0)) {printf("Copy result mode=%d failure=%s\n",mode,images.Failure());return 5;}
            const auto* layer=images.Projection(1);
            if (mode==0) {
                if (!layer || layer->viewCount!=2 || layer->space!=4 || layer->views[0].subImage.swapchain!=10 || layer->views[1].subImage.swapchain!=11 ||
                    layer->views[0].pose.position.x!=-.032f || layer->views[1].pose.position.x!=.032f || images.Projection(2) || releases!=2 || draws!=2) return 6;
                // Verify the independent images actually received different eyes.
                D3D11_TEXTURE2D_DESC read=desc;read.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;read.BindFlags=0;read.Usage=D3D11_USAGE_STAGING;read.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> staging;if (FAILED(device->CreateTexture2D(&read,nullptr,&staging))) return 7;
                for (unsigned eye=0;eye<2;++eye) {
                    context->CopyResource(staging.Get(),chains[10+eye].textures[0].Get());D3D11_MAPPED_SUBRESOURCE map{};
                    if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map))) return 8;
                    const UINT pixel=*static_cast<const UINT*>(map.pData);context->Unmap(staging.Get(),0);
                    if (pixel!=(eye?0xffff0000:0xff0000ff)) return 9;
                }
                if (images.Copy(1,4,views,sources[0].Get(),sources[1].Get()) || images.Projection(1)) return 10;
            } else if (layer || !images.Failure()) return 11;
            if (mode==4 && releases!=1) return 12; // Never release the timed-out right image.
        }
        if (!images.Shutdown() || !chains.empty() || protocolError) {printf("Shutdown mode=%d\n",mode);return 13;}
    }
    Reset(0);grid2vr::XrEyeImages images;
    if (!images.Initialize(3,device.Get(),DrawNative,2,2) || images.Copy(1,4,views,sources[0].Get(),sources[0].Get()) || acquires || !images.Shutdown()) return 14;
    // Exercise the actual blit and projection extents at every launcher scale,
    // with unlike source/output dimensions and distinct left/right source pixels.
    for (unsigned scale=25;scale<=200;scale+=25) {
        Reset(0);grid2vr::XrEyeImages scaled;
        const unsigned width=1280*scale/100,height=720*scale/100;
        if (!scaled.Initialize(3,device.Get(),DrawNative,width,height) || !scaled.Copy(1,4,views,sources[0].Get(),sources[1].Get())) return 15;
        const auto* projection=scaled.Projection(1);
        if (!projection || projection->viewCount!=2 || acquires!=2 || releases!=2 || draws!=2) return 16;
        D3D11_TEXTURE2D_DESC read=desc;read.Width=read.Height=1;read.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        read.BindFlags=0;read.Usage=D3D11_USAGE_STAGING;read.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;if (FAILED(device->CreateTexture2D(&read,nullptr,&staging))) return 17;
        for (unsigned eye=0;eye<2;++eye) {
            const auto extent=projection->views[eye].subImage.imageRect.extent;
            if (extent.width!=width || extent.height!=height) return 18;
            for (unsigned corner=0;corner<3;++corner) {
                const unsigned x=corner==0?0:corner==1?width/2:width-1,y=corner==0?0:corner==1?height/2:height-1;
                const D3D11_BOX box{x,y,0,x+1,y+1,1};
                context->CopySubresourceRegion(staging.Get(),0,0,0,0,chains[10+eye].textures[0].Get(),0,&box);
                D3D11_MAPPED_SUBRESOURCE map{};if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map))) return 19;
                const UINT pixel=*static_cast<const UINT*>(map.pData);context->Unmap(staging.Get(),0);
                if (pixel!=(eye?0xffff0000:0xff0000ff)) return 20;
            }
        }
        if (!scaled.Shutdown() || !chains.empty() || protocolError) return 21;
    }
    // A portrait headset target above the old 4 MP limit must retain its exact
    // dimensions while upscaling the two independent scene sources.
    Reset(0);grid2vr::XrEyeImages headset;
    if (!headset.Initialize(3,device.Get(),DrawNative,2304,2560) || !headset.Copy(1,4,views,sources[0].Get(),sources[1].Get())) return 30;
    const auto* projection=headset.Projection(1);
    if (!projection || projection->views[0].subImage.imageRect.extent.width!=2304 || projection->views[1].subImage.imageRect.extent.height!=2560 ||
        !headset.Shutdown() || !chains.empty() || protocolError) return 31;
    Reset(0);grid2vr::XrEyeImages menu;
    const XrPosef anchor{{0,0,0,1},{1,1.6f,-2}};
    D3D11_TEXTURE2D_DESC backDesc=desc;backDesc.Width=4;backDesc.Height=2;backDesc.BindFlags=D3D11_BIND_RENDER_TARGET;
    const UINT red[8]{0xc00003ff,0xc00003ff,0xc00003ff,0xc00003ff,0xc00003ff,0xc00003ff,0xc00003ff,0xc00003ff};
    const D3D11_SUBRESOURCE_DATA backData{red,16,0};ComPtr<ID3D11Texture2D> back;
    if (FAILED(device->CreateTexture2D(&backDesc,&backData,&back)) || !menu.Initialize(3,device.Get(),DrawNative,2,2) ||
        !menu.CopyScreen(1,4,anchor,back.Get())) return 22;
    const auto* screen=menu.Screen(1);
    if (!screen || menu.Projection(1) || screen->eyeVisibility!=XR_EYE_VISIBILITY_BOTH || screen->space!=4 ||
        screen->subImage.swapchain!=10 || memcmp(&screen->pose,&anchor,sizeof(anchor)) || screen->size.width!=2.4f || screen->size.height!=1.2f ||
        acquires!=1 || releases!=1 || draws!=1) return 23;
    D3D11_TEXTURE2D_DESC read=desc;read.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;read.BindFlags=0;read.Usage=D3D11_USAGE_STAGING;read.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(device->CreateTexture2D(&read,nullptr,&staging))) return 24;
    context->CopyResource(staging.Get(),chains[10].textures[0].Get());
    if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return 25;
    const UINT pixel=*static_cast<const UINT*>(mapped.pData);context->Unmap(staging.Get(),0);
    if (pixel!=0xff0000ff || !menu.Copy(2,4,views,sources[0].Get(),sources[1].Get()) || menu.Screen(2) || !menu.Projection(2) ||
        !menu.CopyScreen(3,4,anchor,back.Get()) || !menu.Screen(3) || menu.Projection(3) || !menu.Shutdown() || !chains.empty() || protocolError) return 26;
    for (int mode=7;mode<=9;++mode) {
        Reset(mode);grid2vr::XrEyeImages failedMenu;
        if (!failedMenu.Initialize(3,device.Get(),DrawNative,2,2) || failedMenu.CopyScreen(1,4,anchor,back.Get()) ||
            failedMenu.Screen(1) || !failedMenu.Failure() || (mode==8 && releases)) return 27;
        if (!failedMenu.Shutdown() || !chains.empty() || protocolError) return 28;
    }
    Reset(0);grid2vr::XrEyeImages invalidMenu;
    auto invalidAnchor=anchor;invalidAnchor.orientation.w=0;
    if (!invalidMenu.Initialize(3,device.Get(),DrawNative,2,2) || invalidMenu.CopyScreen(1,4,invalidAnchor,back.Get()) ||
        invalidMenu.Screen(1) || acquires || !invalidMenu.Shutdown() || protocolError) return 29;
    puts("XR eye images: headset scales, stereo pixels, shared-eye menu pixels/aspect, transitions, protocol and cleanup passed without a runtime.");
}
