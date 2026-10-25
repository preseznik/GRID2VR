#include "eye_textures.h"
#include <d3dcompiler.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
#define CHECK(value) do { if(!(value)) {printf("Failed line %d: %s\n",__LINE__,#value);return 1;} } while(0)
bool Read(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* image,DXGI_FORMAT format,unsigned mode) {
    D3D11_TEXTURE2D_DESC d{};image->GetDesc(&d);
    if(d.Width!=8 || d.Height!=4 || d.Format!=format || d.SampleDesc.Count!=1 || d.SampleDesc.Quality)return false;
    d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;if(FAILED(device->CreateTexture2D(&d,nullptr,&staging)))return false;
    context->CopyResource(staging.Get(),image);D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))return false;
    bool valid=true;
    for(unsigned y=0;y<4;++y) for(unsigned x=0;x<8;++x) {
        uint32_t word{};memcpy(&word,static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch+x*4,4);
        const bool rgb10=format==DXGI_FORMAT_R10G10B10A2_UNORM;
        std::array<unsigned,4> actual=rgb10?std::array<unsigned,4>{word&1023,(word>>10)&1023,(word>>20)&1023,word>>30}:
            std::array<unsigned,4>{word&255,(word>>8)&255,(word>>16)&255,word>>24};
        if(format==DXGI_FORMAT_B8G8R8A8_UNORM)std::swap(actual[0],actual[2]);
        const unsigned half=rgb10?512:128,alpha=rgb10?3:255;
        const std::array<unsigned,4> expected{mode==1?0:half,mode==0?0:half,mode==2?0:half,alpha};
        for(unsigned c=0;c<4;++c)valid&=std::abs(int(actual[c])-int(expected[c]))<=1;
    }
    context->Unmap(staging.Get(),0);return valid;
}
int main() {
    const auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    CHECK(create && SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
    constexpr char shader[]=R"(
cbuffer Parameters:register(b0) { uint mode; uint3 padding; }
float4 vs(uint id:SV_VertexID):SV_Position { return float4(id==1?3:-1,id==2?-3:1,0,1); }
float4 ps(uint sampleIndex:SV_SampleIndex):SV_Target {
    float a=sampleIndex&1,b=1-a;
    return mode==0?float4(a,0,b,1):mode==1?float4(0,a,b,1):float4(a,b,0,1);
}
)";
    ComPtr<ID3DBlob> vb,pb;
    CHECK(SUCCEEDED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vb,nullptr)));
    CHECK(SUCCEEDED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&pb,nullptr)));
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    CHECK(SUCCEEDED(device->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,&vs)));
    CHECK(SUCCEEDED(device->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,&ps)));
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> constants;
    CHECK(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&constants)));
    D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION_PREDICATE,0};ComPtr<ID3D11Predicate> predicate;
    CHECK(SUCCEEDED(device->CreatePredicate(&qd,&predicate)));
    context->Begin(predicate.Get());context->End(predicate.Get());context->Flush();
    HRESULT status=S_FALSE;BOOL queryValue{};
    for(unsigned i=0;i<100 && status==S_FALSE;++i) {status=context->GetData(predicate.Get(),&queryValue,sizeof(queryValue),0);if(status==S_FALSE)Sleep(1);}
    CHECK(status==S_OK && !queryValue);
    grid2vr::EyeTextures eyes;uint64_t frame{};
    for(const auto format:{DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM}) {
        for(const auto samples:{2u,4u,8u}) {
            UINT levels{};CHECK(SUCCEEDED(device->CheckMultisampleQualityLevels(format,samples,&levels)) && levels);
            D3D11_TEXTURE2D_DESC d{};d.Width=8;d.Height=4;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=samples;
            d.Format=format;d.BindFlags=D3D11_BIND_RENDER_TARGET;ComPtr<ID3D11Texture2D> source;ComPtr<ID3D11RenderTargetView> target;
            CHECK(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&source)) && SUCCEEDED(device->CreateRenderTargetView(source.Get(),nullptr,&target)));
            CHECK(eyes.Begin(++frame,3));
            const D3D11_VIEWPORT viewport{0,0,8,4,0,1};
            for(unsigned pass=0;pass<3;++pass) {
                context->SetPredication(nullptr,FALSE);
                const UINT value[]{pass,0,0,0};context->UpdateSubresource(constants.Get(),0,nullptr,value,0,0);
                auto* rtv=target.Get();auto* cb=constants.Get();context->OMSetRenderTargets(1,&rtv,nullptr);context->PSSetConstantBuffers(0,1,&cb);
                context->RSSetViewports(1,&viewport);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->Draw(3,0);
                context->SetPredication(predicate.Get(),TRUE);
                CHECK(eyes.Capture(context.Get(),frame,pass,source.Get()));
                ComPtr<ID3D11RenderTargetView> actualTarget;context->OMGetRenderTargets(1,&actualTarget,nullptr);CHECK(actualTarget==target);
                ComPtr<ID3D11PixelShader> actualPs;context->PSGetShader(&actualPs,nullptr,nullptr);CHECK(actualPs==ps);
                ComPtr<ID3D11VertexShader> actualVs;context->VSGetShader(&actualVs,nullptr,nullptr);CHECK(actualVs==vs);
                ComPtr<ID3D11Buffer> actualCb;context->PSGetConstantBuffers(0,1,&actualCb);CHECK(actualCb==constants);
                ComPtr<ID3D11Predicate> actualPredicate;BOOL predicateValue{};context->GetPredication(&actualPredicate,&predicateValue);
                CHECK(actualPredicate==predicate && predicateValue==TRUE);
                D3D11_VIEWPORT actual{};UINT count=1;context->RSGetViewports(&count,&actual);CHECK(count==1 && !memcmp(&actual,&viewport,sizeof(viewport)));
                CHECK(pass==2 || !eyes.Get(frame,pass));
            }
            CHECK(eyes.Ready(frame));context->SetPredication(nullptr,FALSE);
            // The native source survives resolve. Clearing it afterward must
            // not change any of the three independently resolved images.
            auto resolved=d;resolved.SampleDesc={1,0};resolved.BindFlags=0;ComPtr<ID3D11Texture2D> reference;
            CHECK(SUCCEEDED(device->CreateTexture2D(&resolved,nullptr,&reference)));
            context->ResolveSubresource(reference.Get(),0,source.Get(),0,format);CHECK(Read(device.Get(),context.Get(),reference.Get(),format,2));
            const FLOAT black[]{0,0,0,0};context->ClearRenderTargetView(target.Get(),black);
            for(unsigned pass=0;pass<3;++pass)CHECK(Read(device.Get(),context.Get(),eyes.Get(frame,pass),format,pass));
        }
    }
    puts("2x/4x/8x eye resolve averages distinct samples, preserves RGB10/RGBA/BGRA colours and native source/state, and publishes only complete same-frame groups.");
    return 0;
}
