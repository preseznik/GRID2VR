#include "cockpit_exposure.h"
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <vector>
#include <iostream>
using namespace grid2vr;
using Microsoft::WRL::ComPtr;
int main(int argc,char** argv) {
    auto dll=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(dll,"D3D11CreateDevice"));
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> c;
    if(!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&c)))return 1;
    constexpr char vsCode[]="struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};V main(uint i:SV_VertexID){V o;float2 uv=float2((i<<1)&2,i&2);o.p=float4(uv*float2(2,-2)+float2(-1,1),0,1);o.uv=uv;return o;}";
    constexpr char psCode[]="float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return float4(.25,.5,.75,1);}";
    ComPtr<ID3DBlob> bytes;ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    if(FAILED(D3DCompile(vsCode,sizeof(vsCode)-1,nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&bytes,nullptr)) ||
       FAILED(device->CreateVertexShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&vs)))return 2;
    bytes.Reset();
    if(FAILED(D3DCompile(psCode,sizeof(psCode)-1,nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&bytes,nullptr)) ||
       FAILED(device->CreatePixelShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&ps)))return 3;
    // Optional independent comparison against the locally extracted, exact game shader.
    ComPtr<ID3D11PixelShader> native;
    if(argc==2) {
        std::ifstream file(argv[1],std::ios::binary);std::vector<char> code{std::istreambuf_iterator<char>(file),{}};
        uint64_t hash=14695981039346656037ull;for(auto b:code){hash^=static_cast<unsigned char>(b);hash*=1099511628211ull;}
        if(hash!=CockpitExposure::shaderHash || FAILED(device->CreatePixelShader(code.data(),code.size(),nullptr,&native)))return 4;
    }
    c->VSSetShader(vs.Get(),nullptr,0);c->PSSetShader(ps.Get(),nullptr,0);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT viewport{0,0,1,1,0,1};c->RSSetViewports(1,&viewport);
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target,staging;ComPtr<ID3D11RenderTargetView> rtv;
    if(FAILED(device->CreateTexture2D(&td,nullptr,&target)) || FAILED(device->CreateRenderTargetView(target.Get(),nullptr,&rtv)))return 5;
    auto* rt=rtv.Get();c->OMSetRenderTargets(1,&rt,nullptr);
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(FAILED(device->CreateTexture2D(&td,nullptr,&staging)))return 6;
    std::array<float,324> globals{};globals[8]=globals[9]=.25f;
    std::array<float,112> encoding{};
    ComPtr<ID3D11Buffer> cb[2];D3D11_BUFFER_DESC bd{};bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    for(unsigned i=0;i<2;++i){bd.ByteWidth=i?448:1296;if(FAILED(device->CreateBuffer(&bd,nullptr,&cb[i])))return 7;auto* b=cb[i].Get();c->PSSetConstantBuffers(i,1,&b);}
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;if(FAILED(device->CreateSamplerState(&sd,&sampler)))return 8;
    auto* s=sampler.Get();c->PSSetSamplers(3,1,&s);
    auto read=[&](){
        std::array<float,4> result{};c->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        if(FAILED(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))return result;
        for(unsigned i=0;i<4;++i)result[i]=DirectX::PackedVector::XMConvertHalfToFloat(static_cast<uint16_t*>(mapped.pData)[i]);
        c->Unmap(staging.Get(),0);return result;
    };
    CockpitExposure control;
    if(!CockpitExposure::CameraSupported(0x3ca3d70a,0x466a6000) || !CockpitExposure::CameraSupported(0x3ca3d70a,0x450fc000) ||
        CockpitExposure::CameraSupported(0x3ca3d70a,0x7f800000) || CockpitExposure::CameraSupported(0x3ca3d70b,0x466a6000) ||
        CockpitExposure::CameraSupported(0x3ca3d70a,0x42c80000) || control.Apply(c.Get(),1))return 9;
    // Bright exterior, dark cabin, shade/tunnel, black frame, changing track key.
    const std::array<std::array<float,4>,6> cases{{{.6f,.5f,1.1f,.682594f},{.1f,1.5f,1.1f,1.363636f},
        {.01f,.5f,1.1f,.454545f},{2.f,.5f,1.1f,2.275313f},{0.f,.5f,1.1f,.454545f},{.01f,.25f,.4f,.625f}}};
    for(unsigned n=0;n<cases.size();++n) {
        auto test=cases[n];globals[193]=.405f;globals[194]=.879f;globals[195]=test[2];globals[253]=2;globals[254]=3;encoding[0]=test[1];
        c->UpdateSubresource(cb[0].Get(),0,nullptr,globals.data(),0,0);c->UpdateSubresource(cb[1].Get(),0,nullptr,encoding.data(),0,0);
        std::array<float,64> pixels{};for(unsigned p=0;p<16;++p)pixels[p*4]=test[0];
        // A nonuniform four-quadrant fixture catches sampling-channel/UV mistakes.
        if(n==0)for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)pixels[(y*4+x)*4]=.3f+.2f*float((y>=2?2:0)+(x>=2?1:0));
        td.Width=td.Height=4;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;
        D3D11_SUBRESOURCE_DATA data{pixels.data(),64,0};ComPtr<ID3D11Texture2D> input;ComPtr<ID3D11ShaderResourceView> srv;
        if(FAILED(device->CreateTexture2D(&td,&data,&input)) || FAILED(device->CreateShaderResourceView(input.Get(),nullptr,&srv)))return 10;
        auto* v=srv.Get();c->PSSetShaderResources(0,1,&v);
        std::array<float,4> before{};
        if(native){c->PSSetShader(native.Get(),nullptr,0);c->Draw(3,0);before=read();c->PSSetShader(ps.Get(),nullptr,0);}
        if(!control.Apply(c.Get(),CockpitExposure::shaderHash) || !control.Pending() || control.Apply(c.Get(),CockpitExposure::shaderHash))return 11;
        c->Draw(3,0);auto actual=read();
        if(!control.Restore(c.Get()) || control.Pending() || std::abs(actual[0]-test[2])>.002 ||
            std::abs(actual[1]-test[3])>.002 || actual[2]!=(test[3]>1?3:2) || actual[3]!=1)return 12+n;
        ComPtr<ID3D11PixelShader> restored;c->PSGetShader(&restored,nullptr,nullptr);if(restored!=ps)return 20;
        if(native && (n==0 || n==3) && before!=actual)return 21;
        for(unsigned i=0;i<2;++i){ComPtr<ID3D11Buffer> actualCb;c->PSGetConstantBuffers(i,1,&actualCb);if(actualCb!=cb[i])return 22;}
        ComPtr<ID3D11ShaderResourceView> actualSrv;c->PSGetShaderResources(0,1,&actualSrv);if(actualSrv!=srv)return 23;
    }
    if(!control.PassComplete(0,0) || !control.PassComplete(1,1) || control.PassComplete(1,0) || control.PassComplete(2,2))return 24;
    // Wrong render target or constant-buffer layout must leave the shader unchanged.
    c->OMSetRenderTargets(0,nullptr,nullptr);if(control.Apply(c.Get(),CockpitExposure::shaderHash))return 25;
    c->OMSetRenderTargets(1,&rt,nullptr);ID3D11Buffer* empty=nullptr;c->PSSetConstantBuffers(1,1,&empty);
    if(control.Apply(c.Get(),CockpitExposure::shaderHash))return 26;
    std::cout<<"Cockpit exposure GPU fixtures, native comparisons="<<(native?2:0)<<", binding restoration and guards passed\n";
    return 0;
}
