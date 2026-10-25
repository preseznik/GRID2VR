#include "msaa_readback.h"
#include <d3dcompiler.h>
#include <cstring>

namespace grid2vr {
using Microsoft::WRL::ComPtr;
namespace {
constexpr char Shader[]=R"(
Texture2DMS<uint4> input : register(t0);
RWTexture2D<uint> output : register(u0);
cbuffer Parameters : register(b0) { uint sampleIndex; uint3 padding; }
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID) {
    uint width,height,samples; input.GetDimensions(width,height,samples);
    if (id.x>=width || id.y>=height) return;
    uint4 v=input.Load(id.xy,sampleIndex);
    output[id.xy]=v.x | (v.y<<10) | (v.z<<20) | (v.w<<30);
}
)";
}
HRESULT MsaaReadback::Read(ID3D11DeviceContext* context,ID3D11Texture2D* source,NativeDispatch dispatch,
    const std::function<void(ID3D11Texture2D*,UINT)>& consume,bool& restored) {
    restored=false;
    if (!context || !source || !dispatch || !consume || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    if (desc.Format!=DXGI_FORMAT_R10G10B10A2_UNORM || desc.ArraySize!=1 || desc.MipLevels!=1 ||
        (desc.SampleDesc.Count!=2 && desc.SampleDesc.Count!=4 && desc.SampleDesc.Count!=8) ||
        static_cast<uint64_t>(desc.Width)*desc.Height>3840ull*2160) return E_INVALIDARG;
    ComPtr<ID3D11Device> device,owner; context->GetDevice(&device); source->GetDevice(&owner);
    if (owner.Get()!=device.Get()) return E_INVALIDARG;
    UINT support{}; auto hr=device->CheckFormatSupport(DXGI_FORMAT_R10G10B10A2_UINT,&support);
    if (FAILED(hr) || !(support&D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD)) return E_NOTIMPL;
    ComPtr<ID3D11Predicate> predicate; BOOL predicateValue{}; context->GetPredication(&predicate,&predicateValue);
    if (predicate) return E_NOTIMPL;
    ComPtr<ID3D11ComputeShader> original;
    ID3D11ClassInstance* instances[256]{}; UINT count=256; context->CSGetShader(&original,instances,&count);
    for (UINT i=0;i<count;++i) if (instances[i]) instances[i]->Release();
    if (count) return E_NOTIMPL;
    if (!shader_) {
        ComPtr<ID3DBlob> code;
        hr=D3DCompile(Shader,strlen(Shader),"grid2vr-msaa-readback",nullptr,nullptr,"main","cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,nullptr);
        if (FAILED(hr)) return hr;
        hr=device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader_);
        if (FAILED(hr)) return hr;
    }
    shader_->GetDevice(&owner); if (owner.Get()!=device.Get()) return E_INVALIDARG;
    auto copyDesc=desc; copyDesc.Format=DXGI_FORMAT_R10G10B10A2_TYPELESS;
    copyDesc.Usage=D3D11_USAGE_DEFAULT; copyDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE; copyDesc.CPUAccessFlags=0; copyDesc.MiscFlags=0;
    ComPtr<ID3D11Texture2D> copy,packed;
    if (FAILED(hr=device->CreateTexture2D(&copyDesc,nullptr,&copy))) return hr;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format=DXGI_FORMAT_R10G10B10A2_UINT; vd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DMS;
    ComPtr<ID3D11ShaderResourceView> input;
    if (FAILED(hr=device->CreateShaderResourceView(copy.Get(),&vd,&input))) return hr;
    auto packedDesc=desc; packedDesc.Format=DXGI_FORMAT_R32_UINT; packedDesc.SampleDesc={1,0};
    packedDesc.Usage=D3D11_USAGE_DEFAULT; packedDesc.BindFlags=D3D11_BIND_UNORDERED_ACCESS; packedDesc.CPUAccessFlags=0; packedDesc.MiscFlags=0;
    if (FAILED(hr=device->CreateTexture2D(&packedDesc,nullptr,&packed))) return hr;
    ComPtr<ID3D11UnorderedAccessView> target;
    if (FAILED(hr=device->CreateUnorderedAccessView(packed.Get(),nullptr,&target))) return hr;
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth=16; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> parameters;
    if (FAILED(hr=device->CreateBuffer(&bd,nullptr,&parameters))) return hr;
    context->CopyResource(copy.Get(),source);
    ComPtr<ID3D11ShaderResourceView> oldInput; context->CSGetShaderResources(0,1,&oldInput);
    ComPtr<ID3D11Buffer> oldParameters; context->CSGetConstantBuffers(0,1,&oldParameters);
    ID3D11UnorderedAccessView* oldTargets[8]{}; context->CSGetUnorderedAccessViews(0,8,oldTargets);
    ID3D11UnorderedAccessView* empty[8]{}; ID3D11UnorderedAccessView* targets[8]{target.Get()};
    const UINT preserve[8]{UINT(-1),UINT(-1),UINT(-1),UINT(-1),UINT(-1),UINT(-1),UINT(-1),UINT(-1)};
    context->CSSetUnorderedAccessViews(0,8,empty,preserve);
    auto* srv=input.Get(); context->CSSetShaderResources(0,1,&srv);
    auto* cb=parameters.Get(); context->CSSetConstantBuffers(0,1,&cb);
    context->CSSetShader(shader_.Get(),nullptr,0);
    context->CSSetUnorderedAccessViews(0,8,targets,preserve);
    // Always restore bindings, including if a readback callback throws.
    auto restore=[&]() {
        context->CSSetUnorderedAccessViews(0,8,empty,preserve);
        auto* previous=oldInput.Get(); context->CSSetShaderResources(0,1,&previous);
        auto* buffer=oldParameters.Get(); context->CSSetConstantBuffers(0,1,&buffer);
        context->CSSetUnorderedAccessViews(0,8,oldTargets,preserve);
        context->CSSetShader(original.Get(),nullptr,0);
        ComPtr<ID3D11ComputeShader> actual; context->CSGetShader(&actual,nullptr,nullptr);
        ComPtr<ID3D11ShaderResourceView> actualInput; context->CSGetShaderResources(0,1,&actualInput);
        ComPtr<ID3D11Buffer> actualCB; context->CSGetConstantBuffers(0,1,&actualCB);
        ID3D11UnorderedAccessView* actualTargets[8]{}; context->CSGetUnorderedAccessViews(0,8,actualTargets);
        restored=actual.Get()==original.Get() && actualInput.Get()==oldInput.Get() && actualCB.Get()==oldParameters.Get();
        for (UINT i=0;i<8;++i) {
            restored=restored && actualTargets[i]==oldTargets[i];
            if (oldTargets[i]) oldTargets[i]->Release();
            if (actualTargets[i]) actualTargets[i]->Release();
        }
    };
    try {
        for (UINT sample=0;sample<desc.SampleDesc.Count;++sample) {
            const UINT values[]{sample,0,0,0}; context->UpdateSubresource(parameters.Get(),0,nullptr,values,0,0);
            dispatch(context,(desc.Width+7)/8,(desc.Height+7)/8,1);
            consume(packed.Get(),sample);
        }
    } catch (...) { restore(); throw; }
    restore();
    return restored ? device->GetDeviceRemovedReason():E_FAIL;
}
}
