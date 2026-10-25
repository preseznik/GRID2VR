#include "vpl_order.h"
#include <d3dcompiler.h>
#include <cstring>

namespace grid2vr {
using Microsoft::WRL::ComPtr;
namespace {
constexpr char SortShader[]=R"(
RWTexture2D<uint> indices : register(u0);
groupshared uint values[512];
groupshared uint length;
[numthreads(512,1,1)]
void main(uint3 tile:SV_GroupID,uint i:SV_GroupIndex) {
    uint width,height;
    indices.GetDimensions(width,height);
    uint capacity=min(32u,height-tile.y*32u)*16u;
    uint2 location=tile.xy*uint2(16,32)+uint2(i&15,i>>4);
    uint value=i<capacity ? indices[location]:65536u;
    if (i==0) length=capacity;
    GroupMemoryBarrierWithGroupSync();
    if (value==0) InterlockedMin(length,i);
    GroupMemoryBarrierWithGroupSync();
    // An unterminated tile is not a supported list. Do not rewrite it.
    if (length==capacity) return;
    values[i]=i<length ? value:65536;
    GroupMemoryBarrierWithGroupSync();
    for (uint width=2;width<=512;width<<=1) {
        for (uint stride=width>>1;stride>0;stride>>=1) {
            uint a=values[i], b=values[i^stride];
            uint next=((i&stride)==0)==((i&width)==0) ? min(a,b):max(a,b);
            GroupMemoryBarrierWithGroupSync();
            values[i]=next;
            GroupMemoryBarrierWithGroupSync();
        }
    }
    if (i<length) indices[location]=values[i];
}
)";
}
HRESULT VplOrder::Apply(ID3D11DeviceContext* context,ID3D11UnorderedAccessView* view,
    UINT groupsX,UINT groupsY,UINT groupsZ,NativeDispatch dispatch,bool& restored) {
    restored=false;
    if (!context || !view || !dispatch || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        !groupsX || groupsX>512 || !groupsY || groupsY>256 || groupsZ!=1) return E_INVALIDARG;
    D3D11_UNORDERED_ACCESS_VIEW_DESC vd{}; view->GetDesc(&vd);
    ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (vd.Format!=DXGI_FORMAT_R16_UINT || vd.ViewDimension!=D3D11_UAV_DIMENSION_TEXTURE2D ||
        vd.Texture2D.MipSlice!=0 || FAILED(resource.As(&texture))) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    if (desc.Format!=DXGI_FORMAT_R16_UINT || desc.ArraySize!=1 || desc.MipLevels!=1 ||
        desc.SampleDesc.Count!=1 || desc.Width!=groupsX*16 ||
        desc.Height<=(groupsY-1)*32 || desc.Height>groupsY*32) return E_INVALIDARG;
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support{DXGI_FORMAT_R16_UINT,0};
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2,&support,sizeof(support))) ||
        !(support.OutFormatSupport2&D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD)) return E_NOTIMPL;
    if (!shader_) {
        ComPtr<ID3DBlob> code,error;
        auto hr=D3DCompile(SortShader,strlen(SortShader),"grid2vr-vpl-order",nullptr,nullptr,"main","cs_5_0",
                          D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
        if (FAILED(hr)) return hr;
        hr=device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader_);
        if (FAILED(hr)) return hr;
    }
    ComPtr<ID3D11Device> owner; shader_->GetDevice(&owner);
    if (owner.Get()!=device.Get()) return E_INVALIDARG;
    ComPtr<ID3D11ComputeShader> original;
    ID3D11ClassInstance* instances[256]{}; UINT count=256;
    context->CSGetShader(&original,instances,&count);
    for (UINT i=0;i<count;++i) if (instances[i]) instances[i]->Release();
    if (count) return E_NOTIMPL;
    ID3D11UnorderedAccessView* previous[8]{}; context->CSGetUnorderedAccessViews(0,8,previous);
    ID3D11UnorderedAccessView* targets[8]{view};
    const UINT preserve[8]{UINT(-1),UINT(-1),UINT(-1),UINT(-1),UINT(-1),UINT(-1),UINT(-1),UINT(-1)};
    // Unbind first: moving native UAV1 to UAV0 in one call can alias a binding.
    ID3D11UnorderedAccessView* empty[8]{}; context->CSSetUnorderedAccessViews(0,8,empty,preserve);
    context->CSSetUnorderedAccessViews(0,8,targets,preserve);
    context->CSSetShader(shader_.Get(),nullptr,0);
    dispatch(context,groupsX,groupsY,1);
    context->CSSetUnorderedAccessViews(0,8,empty,preserve);
    context->CSSetUnorderedAccessViews(0,8,previous,preserve);
    context->CSSetShader(original.Get(),nullptr,0);
    ComPtr<ID3D11ComputeShader> actual; context->CSGetShader(&actual,nullptr,nullptr);
    ID3D11UnorderedAccessView* bound[8]{}; context->CSGetUnorderedAccessViews(0,8,bound);
    restored=actual.Get()==original.Get();
    for (unsigned i=0;i<8;++i) {
        restored=restored && previous[i]==bound[i];
        if (previous[i]) previous[i]->Release();
        if (bound[i]) bound[i]->Release();
    }
    return restored ? device->GetDeviceRemovedReason():E_FAIL;
}
}
