#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <utility>

namespace grid2vr {
// Identity of the guarded full-screen shadow draw. Its global draw ordinal
// can differ between eyes when earlier native geometry is culled.
struct ShadowPassIdentity {
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertices;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>,5> inputs;
    std::array<Microsoft::WRL::ComPtr<ID3D11SamplerState>,5> samplers;
    // Slot 6 is replayed from the center separately. Null is D3D11's default
    // sampler, not a missing capture; target identifies a successful Read.
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler6;
    D3D11_VIEWPORT viewport{};
    UINT stride{},offset{};
    bool Read(ID3D11DeviceContext* context) {
        if (!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
        ShadowPassIdentity candidate;
        ID3D11RenderTargetView* colors[8]{};
        context->OMGetRenderTargets(8,colors,&candidate.depth);
        candidate.target.Attach(colors[0]);
        bool extra{};
        for (unsigned i=1;i<8;++i) if (colors[i]) {extra=true;colors[i]->Release();}
        UINT count=1;context->RSGetViewports(&count,&candidate.viewport);
        D3D11_PRIMITIVE_TOPOLOGY topology{};context->IAGetPrimitiveTopology(&topology);
        context->IAGetVertexBuffers(0,1,&candidate.vertices,&candidate.stride,&candidate.offset);
        context->IAGetInputLayout(&candidate.layout);
        if (!candidate.target || extra || count!=1 || topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
            !candidate.vertices || !candidate.layout || candidate.stride!=16 || candidate.offset!=0 ||
            candidate.viewport.TopLeftX!=0 || candidate.viewport.TopLeftY!=0 ||
            !(candidate.viewport.Width>0 && candidate.viewport.Width<=8192 && candidate.viewport.Height>0 && candidate.viewport.Height<=8192) ||
            candidate.viewport.MinDepth!=0 || candidate.viewport.MaxDepth!=1) return false;
        ID3D11ShaderResourceView* boundInputs[5]{};context->PSGetShaderResources(0,5,boundInputs);
        ID3D11SamplerState* boundSamplers[5]{};context->PSGetSamplers(1,5,boundSamplers);
        for (unsigned i=0;i<5;++i) {candidate.inputs[i].Attach(boundInputs[i]);candidate.samplers[i].Attach(boundSamplers[i]);}
        context->PSGetSamplers(6,1,&candidate.sampler6);
        *this=std::move(candidate);return true;
    }
    bool ApplySampler(ID3D11DeviceContext* context) const {
        if (!target || !context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
        auto* binding=sampler6.Get();context->PSSetSamplers(6,1,&binding);
        Microsoft::WRL::ComPtr<ID3D11SamplerState> actual;context->PSGetSamplers(6,1,&actual);
        return actual.Get()==binding;
    }
    bool Matches(const ShadowPassIdentity& other) const {
        if (!target || target!=other.target || depth!=other.depth || vertices!=other.vertices || layout!=other.layout ||
            stride!=other.stride || offset!=other.offset || memcmp(&viewport,&other.viewport,sizeof(viewport))) return false;
        for (unsigned i=0;i<5;++i) if (inputs[i]!=other.inputs[i] || samplers[i]!=other.samplers[i]) return false;
        return true;
    }
};
}
