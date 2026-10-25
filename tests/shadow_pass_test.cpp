#include "shadow_pass.h"
#include <d3dcompiler.h>
#include <cstdio>
using Microsoft::WRL::ComPtr;
using grid2vr::ShadowPassIdentity;
int main() {
    const auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    if (!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))) return 1;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=8;desc.Height=4;desc.ArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture,input;ComPtr<ID3D11RenderTargetView> target,otherTarget;
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&texture)) || FAILED(device->CreateTexture2D(&desc,nullptr,&input)) ||
        FAILED(device->CreateRenderTargetView(texture.Get(),nullptr,&target)) || FAILED(device->CreateRenderTargetView(input.Get(),nullptr,&otherTarget)) ||
        FAILED(device->CreateShaderResourceView(input.Get(),nullptr,&view))) return 2;
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=64;buffer.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    ComPtr<ID3D11Buffer> vertices;if (FAILED(device->CreateBuffer(&buffer,nullptr,&vertices))) return 3;
    const char shader[]="float4 main(float4 p:POSITION):SV_Position { return p; }";
    ComPtr<ID3DBlob> code;ComPtr<ID3D11InputLayout> layout;
    const D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    if (FAILED(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&code,nullptr)) ||
        FAILED(device->CreateInputLayout(&element,1,code->GetBufferPointer(),code->GetBufferSize(),&layout))) return 4;
    auto* rtv=target.Get();context->OMSetRenderTargets(1,&rtv,nullptr);
    auto* vb=vertices.Get();UINT stride=16,offset=0;context->IASetVertexBuffers(0,1,&vb,&stride,&offset);
    context->IASetInputLayout(layout.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    D3D11_VIEWPORT viewport{0,0,8,4,0,1};context->RSSetViewports(1,&viewport);
    auto* srv=view.Get();context->PSSetShaderResources(0,1,&srv);
    ShadowPassIdentity original,again;
    if (!original.Read(context.Get()) || !again.Read(context.Get()) || !original.Matches(again)) return 5;
    // An unrelated earlier draw does not change pass identity.
    context->Draw(0,0);
    if (!again.Read(context.Get()) || !original.Matches(again)) return 6;
    viewport.Width=7;context->RSSetViewports(1,&viewport);
    if (!again.Read(context.Get()) || original.Matches(again)) return 7;
    viewport.Width=8;context->RSSetViewports(1,&viewport);
    srv=nullptr;context->PSSetShaderResources(0,1,&srv);
    if (!again.Read(context.Get()) || original.Matches(again)) return 8;
    rtv=otherTarget.Get();context->OMSetRenderTargets(1,&rtv,nullptr);
    if (!again.Read(context.Get()) || original.Matches(again)) return 9;
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (again.Read(context.Get())) return 10;
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    offset=16;context->IASetVertexBuffers(0,1,&vb,&stride,&offset);
    if (again.Read(context.Get())) return 11;
    offset=0;context->IASetVertexBuffers(0,1,&vb,&stride,&offset);
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU=samplerDesc.AddressV=samplerDesc.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MaxAnisotropy=1;samplerDesc.ComparisonFunc=D3D11_COMPARISON_ALWAYS;samplerDesc.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> explicitSampler;
    if (FAILED(device->CreateSamplerState(&samplerDesc,&explicitSampler))) return 12;
    // Replay and restore both null/default and explicit center bindings across
    // two eyes. Slot 6 may differ; the other pass identity must still match.
    for (bool defaultCenter:{true,false}) {
        auto* centerBinding=defaultCenter?nullptr:explicitSampler.Get();
        context->PSSetSamplers(6,1,&centerBinding);
        ShadowPassIdentity center;
        if (!center.Read(context.Get()) || center.sampler6.Get()!=centerBinding) return 13;
        for (unsigned eye=0;eye<2;++eye) {
            auto* before=defaultCenter?explicitSampler.Get():nullptr;
            context->PSSetSamplers(6,1,&before);
            ShadowPassIdentity restore,missing;
            if (!restore.Read(context.Get()) || !center.Matches(restore)) return 14;
            if (missing.ApplySampler(context.Get()) || center.ApplySampler(nullptr)) return 15;
            ComPtr<ID3D11SamplerState> actual;context->PSGetSamplers(6,1,&actual);
            if (actual.Get()!=before || !center.ApplySampler(context.Get())) return 16;
            actual.Reset();context->PSGetSamplers(6,1,&actual);
            if (actual.Get()!=centerBinding || !restore.ApplySampler(context.Get())) return 17;
            actual.Reset();context->PSGetSamplers(6,1,&actual);
            if (actual.Get()!=before) return 18;
        }
        center={};
        if (center.ApplySampler(context.Get())) return 19;
    }
    puts("Shadow identity guards and two-eye replay/restore of explicit or null/default samplers passed.");
}
