#include "cockpit_exposure.h"
#include <d3dcompiler.h>
using Microsoft::WRL::ComPtr;
namespace grid2vr {
bool CockpitExposure::Apply(ID3D11DeviceContext* context,uint64_t hash) {
    if (hash!=shaderHash || !context || original_ || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    if (context_ && (context_.Get()!=context || thread_!=GetCurrentThreadId())) return false;
    ComPtr<ID3D11RenderTargetView> targets[2];
    ID3D11RenderTargetView* raw[2]{};context->OMGetRenderTargets(2,raw,nullptr);
    targets[0].Attach(raw[0]);targets[1].Attach(raw[1]);
    if (!targets[0] || targets[1]) return false;
    ComPtr<ID3D11Resource> resource;targets[0]->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;if(FAILED(resource.As(&texture)))return false;
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    if (desc.Width!=1 || desc.Height!=1 || desc.MipLevels!=1 || desc.ArraySize!=1 ||
        desc.SampleDesc.Count!=1 || desc.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT) return false;
    for(unsigned slot=0;slot<2;++slot) {
        ComPtr<ID3D11Buffer> buffer;context->PSGetConstantBuffers(slot,1,&buffer);
        D3D11_BUFFER_DESC bd{};if(buffer)buffer->GetDesc(&bd);
        // Runtime captures allocate 448 bytes for the 416-byte reflected frame
        // block. Keep that measured allocation guard; only its first float is read.
        if(bd.ByteWidth!=(slot?448u:1296u) || bd.BindFlags!=D3D11_BIND_CONSTANT_BUFFER || bd.MiscFlags || bd.StructureByteStride) return false;
    }
    ComPtr<ID3D11PixelShader> source;UINT classes=0;context->PSGetShader(&source,nullptr,&classes);
    if(!source || classes) return false;
    if(!filtered_) {
        // Original four-tap reduction and outputs, plus one floor on the ratio.
        // CPU E advances toward E/ratio. encoding.x=E*5/6, so this floor enforces
        // the authored keyValue ceiling while retaining native adaptation below it.
        constexpr char code[]=R"(
cbuffer Globals:register(b0){float4 g[81];}
cbuffer PerFrame:register(b1){float4 encoding[26];}
Texture2D<float4> inputTexture:register(t0);SamplerState inputSampler:register(s3);
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target {
    float2 step=g[2].xy;
    precise float mean=inputTexture.Sample(inputSampler,uv-step).x;
    mean=mean+inputTexture.Sample(inputSampler,uv+step*float2(-1,1)).x;
    mean=mean+inputTexture.Sample(inputSampler,uv+step*float2(1,-1)).x;
    mean=mean+inputTexture.Sample(inputSampler,uv+step).x;
    mean=mean*0.25;
    float key=g[48].w;
    float ceiling=encoding[0].x/key;
    float ratio=min(mean/g[48].y,max(mean/g[48].z,ceiling));
    if(isfinite(ceiling) && ceiling>0 && isfinite(ratio)) ratio=max(ratio,ceiling);
    return float4(key,ratio,ratio>1?g[63].z:g[63].y,1);
})";
        ComPtr<ID3DBlob> bytes;ComPtr<ID3D11Device> device;context->GetDevice(&device);
        if(FAILED(D3DCompile(code,sizeof(code)-1,nullptr,nullptr,nullptr,"main","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&bytes,nullptr)) ||
            FAILED(device->CreatePixelShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&filtered_))) return false;
        context_=context;thread_=GetCurrentThreadId();
    }
    original_=source;context->PSSetShader(filtered_.Get(),nullptr,0);
    return true;
}
bool CockpitExposure::Restore(ID3D11DeviceContext* context) {
    if(!original_ || context_.Get()!=context || thread_!=GetCurrentThreadId())return false;
    ComPtr<ID3D11PixelShader> before;context->PSGetShader(&before,nullptr,nullptr);
    context->PSSetShader(original_.Get(),nullptr,0);
    ComPtr<ID3D11PixelShader> after;context->PSGetShader(&after,nullptr,nullptr);
    const bool result=before==filtered_ && after==original_;original_.Reset();return result;
}
}
