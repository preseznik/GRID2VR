#include "eye_textures.h"
#include "eye_mirror.h"
#include "eye_blit.h"
#include "resolution_limits.h"
#include <cstdio>

using Microsoft::WRL::ComPtr;
#define CHECK(value) do { if(!(value)) {printf("Failed line %d: %s\n",__LINE__,#value);return false;} } while(0)
void STDMETHODCALLTYPE DrawNative(ID3D11DeviceContext* context,UINT count,UINT first) {context->Draw(count,first);}

bool CheckColor(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* texture,unsigned channel) {
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    const auto width=desc.Width,height=desc.Height;
    desc.Width=desc.Height=1;desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;CHECK(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)));
    // Read only four pixels, avoiding a large CPU readback allocation.
    for(unsigned y:{0u,height-1}) for(unsigned x:{0u,width-1}) {
        const D3D11_BOX box{x,y,0,x+1,y+1,1};
        context->CopySubresourceRegion(staging.Get(),0,0,0,0,texture,0,&box);
        D3D11_MAPPED_SUBRESOURCE mapped{};CHECK(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)));
        const auto* pixel=static_cast<const unsigned char*>(mapped.pData);
        bool correct=true;
        for(unsigned c=0;c<4;++c) correct&=pixel[c]==((c==channel || c==3)?255:0);
        context->Unmap(staging.Get(),0);CHECK(correct);
    }
    return true;
}

bool Run(ID3D11Device* device,ID3D11DeviceContext* context,unsigned scale) {
    const grid2vr::resolution::Plan plan{3072,3264,scale};CHECK(plan.Valid());
    grid2vr::EyeTextures eyes;grid2vr::EyeMirror mirror;grid2vr::EyeBlit blit;
    CHECK(mirror.Initialize(device) && blit.Initialize(device,DrawNative));
    D3D11_TEXTURE2D_DESC desc{};desc.Width=plan.SceneWidth();desc.Height=plan.SceneHeight();
    desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> scene,output;ComPtr<ID3D11RenderTargetView> sceneView,outputView;
    CHECK(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&scene)) && SUCCEEDED(device->CreateRenderTargetView(scene.Get(),nullptr,&sceneView)));
    desc.Width=plan.width;desc.Height=plan.height;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    CHECK(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&output)) && SUCCEEDED(device->CreateRenderTargetView(output.Get(),nullptr,&outputView)));
    CHECK(eyes.Begin(1,3));
    for(unsigned i=0;i<3;++i) {
        float color[4]{0,0,0,1};color[i]=1;
        context->ClearRenderTargetView(sceneView.Get(),color);
        CHECK(eyes.Capture(context,1,i,scene.Get()));
        CHECK(eyes.Ready(1)==(i==2));
    }
    for(unsigned i=0;i<3;++i) {
        CHECK(mirror.Copy(context,eyes.Get(1,i),scene.Get()));
        CHECK(CheckColor(device,context,scene.Get(),i));
        if(i) {
            CHECK(blit.Draw(i-1,eyes.Get(1,i),outputView.Get()));
            CHECK(CheckColor(device,context,output.Get(),i));
        }
    }
    CHECK(SUCCEEDED(device->GetDeviceRemovedReason()));
    printf("%u%%: %ux%u scene, three same-frame copies and preview, both eyes downsampled to %ux%u; corner colors verified.\n",
        scale,plan.SceneWidth(),plan.SceneHeight(),plan.width,plan.height);
    context->ClearState();context->Flush();
    return true;
}

int main() {
    const auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create=module?reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice")):nullptr;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    if(!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))) return 1;
    return Run(device.Get(),context.Get(),150) && Run(device.Get(),context.Get(),200)?0:2;
}
