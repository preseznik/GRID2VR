#include "screen_ui.h"
#include <d3dcompiler.h>
#include <fstream>
#include <cstdio>
#include <thread>
using namespace grid2vr;
using Microsoft::WRL::ComPtr;
#define CHECK(x) do { if (!(x)) { printf("Failed line %d: %s\n",__LINE__,#x);return 1; } } while(0)
std::array<uint8_t,320> Read(const std::filesystem::path& path) {
    std::array<uint8_t,320> data{};std::ifstream input(path,std::ios::binary);
    input.read(reinterpret_cast<char*>(data.data()),data.size());return data;
}
int main(int argc,char** argv) {
    CHECK(argc==3);const std::filesystem::path fixtures=argv[1],folder=argv[2];
    std::filesystem::create_directories(folder);
    const auto ortho=Read(fixtures/"native-ui-orthographic.bin"),portrait=Read(fixtures/"native-ui-headset-orthographic.bin");
    for (const auto* name:{"orthographic","perspective"}) {
        const auto old=Read(fixtures/(std::string("native-ui-")+name+".bin"));
        const auto native=Read(fixtures/(std::string("native-ui-headset-")+name+".bin"));
        std::array<uint8_t,320> fixed{};
        for (const auto size:{std::array<unsigned,2>{3072,3264},{4608,4896},{320,340}}) {
            CHECK(FlatScreenUiCamera(native,size[0],size[1],fixed));
            CHECK(!memcmp(fixed.data(),old.data(),308) && !memcmp(fixed.data()+308,native.data()+308,12));
        }
        auto foreign=native;foreign[128]^=1;const auto before=fixed;
        CHECK(!FlatScreenUiCamera(foreign,320,340,fixed) && fixed==before);
        CHECK(!FlatScreenUiCamera(native,1280,720,fixed) && fixed==before);
        foreign=native;foreign[96]^=0x40;
        CHECK(!FlatScreenUiCamera(foreign,320,340,fixed) && fixed==before);
        CHECK(!FlatScreenUiCamera(std::span(native).first(308),320,340,fixed));
    }
    std::ofstream(folder/"headset-resolution.request")<<"320 340 100";
    std::ofstream(folder/"scene-size.request")<<"320 340";
    FlatScreenUi flat;CHECK(flat.Configure(folder) && flat.Enabled());
    {FlatScreenUi bad;std::ofstream(folder/"scene-size.request")<<"640 680";CHECK(!bad.Configure(folder) && !bad.Enabled());}
    std::ofstream(folder/"scene-size.request")<<"320 340";
    const auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    CHECK(create && SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=320;bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA initial{portrait.data(),0,0};ComPtr<ID3D11Buffer> camera;
    CHECK(SUCCEEDED(device->CreateBuffer(&bd,&initial,&camera)));
    auto* binding=camera.Get();context->VSSetConstantBuffers(3,1,&binding);context->PSSetConstantBuffers(3,1,&binding);
    D3D11_TEXTURE2D_DESC td{};td.Width=320;td.Height=340;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R10G10B10A2_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target,second;ComPtr<ID3D11RenderTargetView> rtv,secondRtv;
    CHECK(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&target)) && SUCCEEDED(device->CreateRenderTargetView(target.Get(),nullptr,&rtv)));
    CHECK(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&second)) && SUCCEEDED(device->CreateRenderTargetView(second.Get(),nullptr,&secondRtv)));
    auto* output=rtv.Get();context->OMSetRenderTargets(1,&output,nullptr);
    const D3D11_VIEWPORT vp{0,0,320,340,0,1};context->RSSetViewports(1,&vp);
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=rd.ScissorEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;CHECK(SUCCEEDED(device->CreateRasterizerState(&rd,&raster)));context->RSSetState(raster.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const char code[]=R"(cbuffer Camera:register(b3){float4 c[20];}
float4 vs(uint id:SV_VertexID):SV_Position {
    float2 p[6]={float2(-.5,-1.5),float2(-.5,1.5),float2(.5,-1.5),float2(.5,-1.5),float2(-.5,1.5),float2(.5,1.5)};
    float2 v=p[id%6]+float2(id<6?-6.2:6.2,0);return v.x*c[4]+v.y*c[5]+c[7];
} float4 ps():SV_Target{return float4(1,0,0,1);})";
    ComPtr<ID3DBlob> vs,ps;CHECK(SUCCEEDED(D3DCompile(code,sizeof(code),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vs,nullptr)) &&
        SUCCEEDED(D3DCompile(code,sizeof(code),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,nullptr)));
    ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;
    CHECK(SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex)) &&
        SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel)));
    context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);
    auto stagingDesc=td;stagingDesc.Usage=D3D11_USAGE_STAGING;stagingDesc.BindFlags=0;stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;CHECK(SUCCEEDED(device->CreateTexture2D(&stagingDesc,nullptr,&staging)));
    const auto sample=[&](unsigned x,unsigned y) {
        context->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE map{};UINT value{};
        if(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map))) {
            memcpy(&value,static_cast<uint8_t*>(map.pData)+y*map.RowPitch+x*4,4);context->Unmap(staging.Get(),0);
        }return value;
    };
    const D3D11_RECT full{0,0,320,340};context->RSSetScissorRects(1,&full);const float clear[4]{};
    context->ClearRenderTargetView(rtv.Get(),clear);context->Draw(12,0);
    CHECK(!sample(20,170) && !sample(300,170)); // Both native menu edges really clip before this fix.
    CHECK(!flat.Apply(context.Get(),0xac1fbbd477a609bdull) && !flat.Pending());
    CHECK(flat.Apply(context.Get(),screenUiShaders[0]) && flat.Pending()); // Seed a camera made before the hooks.
    CHECK(!flat.Reset() && flat.Pending());
    ComPtr<ID3D11Buffer> actualVs,actualPs;context->VSGetConstantBuffers(3,1,&actualVs);context->PSGetConstantBuffers(3,1,&actualPs);
    CHECK(actualVs!=camera && actualVs==actualPs);
    context->Draw(12,0);CHECK(flat.Restore(context.Get()) && !flat.Pending());
    CHECK(sample(20,170)==0xc00003ff && sample(300,170)==0xc00003ff && !sample(160,170));
    context->VSGetConstantBuffers(3,1,&actualVs);context->PSGetConstantBuffers(3,1,&actualPs);CHECK(actualVs==camera && actualPs==camera);
    // Viewport bounds stay open; an actual control/list edge still clips.
    for (const D3D11_RECT clip:{D3D11_RECT{0,0,160,340},D3D11_RECT{160,0,320,340},D3D11_RECT{121,0,121,340}}) {
        context->RSSetScissorRects(1,&clip);context->ClearRenderTargetView(rtv.Get(),clear);
        CHECK(flat.Apply(context.Get(),screenUiObservedShaders[0]));context->Draw(12,0);CHECK(flat.Restore(context.Get()));
        CHECK(bool(sample(20,170))==(clip.left==0) && bool(sample(300,170))==(clip.right==320));
        D3D11_RECT restored{};UINT count=1;context->RSGetScissorRects(&count,&restored);CHECK(count==1 && !memcmp(&restored,&clip,sizeof(clip)));
    }
    context->RSSetScissorRects(1,&full);
    auto world=portrait;world[128]^=1;ScreenUi::Upload(camera.Get(),world.data(),320);
    CHECK(!flat.Apply(context.Get(),screenUiShaders[0]) && !flat.Pending());
    ScreenUi::Upload(camera.Get(),portrait.data(),320);
    ID3D11RenderTargetView* mrt[]{rtv.Get(),secondRtv.Get()};context->OMSetRenderTargets(2,mrt,nullptr);
    CHECK(!flat.Apply(context.Get(),screenUiShaders[0]));context->OMSetRenderTargets(1,&output,nullptr);
    bool foreignApplied=true;std::thread other([&]{foreignApplied=flat.Apply(context.Get(),screenUiShaders[0]);});other.join();CHECK(!foreignApplied);
    bool foreignReset=true;std::thread resetOther([&]{foreignReset=flat.Reset();});resetOther.join();CHECK(!foreignReset && flat.Enabled());
    binding=nullptr;context->PSSetConstantBuffers(3,1,&binding);
    CHECK(flat.Apply(context.Get(),screenUiShaders[0]));context->PSGetConstantBuffers(3,1,&actualPs);CHECK(!actualPs);
    CHECK(flat.Restore(context.Get()));context->PSGetConstantBuffers(3,1,&actualPs);CHECK(!actualPs);
    auto readDesc=bd;readDesc.Usage=D3D11_USAGE_STAGING;readDesc.BindFlags=0;readDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> cameraRead;CHECK(SUCCEEDED(device->CreateBuffer(&readDesc,nullptr,&cameraRead)));
    context->CopyResource(cameraRead.Get(),camera.Get());D3D11_MAPPED_SUBRESOURCE cameraMap{};
    CHECK(SUCCEEDED(context->Map(cameraRead.Get(),0,D3D11_MAP_READ,0,&cameraMap)));
    const bool sourceUnchanged=!memcmp(cameraMap.pData,portrait.data(),portrait.size());context->Unmap(cameraRead.Get(),0);CHECK(sourceUnchanged);
    CHECK(flat.Reset() && !flat.Enabled() && !flat.Pending());
    CHECK(SUCCEEDED(device->GetDeviceRemovedReason()));
    puts("Flat UI: captured portrait camera guards, native clipped-edge recovery, scoped bindings, scissors, world/unknown/MRT/thread refusals passed.");
}
