#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace grid2vr {
// Depth clamping for colour+depth draws while the raised VR cockpit near plane is active
// (CockpitNearPlane, prototype). Each draw's rasterizer state is swapped for a cached copy
// with DepthClipEnable=FALSE, so interior geometry nearer than the near plane (roof,
// pillars) is drawn at the nearest depth instead of being cut away; the depth buffer keeps
// one projection, so the game's depth consumers (shadow mask, AO, fog) are unchanged.
// Depth-only passes (shadow maps: no render target) keep their own state.
class DepthClamp {
public:
    struct Token { Microsoft::WRL::ComPtr<ID3D11RasterizerState> original; bool active{}; };
    static constexpr size_t cacheLimit=64;
    void SetActive(bool active) noexcept { active_.store(active,std::memory_order_release); }
    bool Active() const noexcept { return active_.load(std::memory_order_acquire); }
    Token Begin(ID3D11DeviceContext* context) noexcept {
        Token token;
        if (!Active() || !context) return token;
        try {
            using Microsoft::WRL::ComPtr;
            ComPtr<ID3D11RenderTargetView> target; ComPtr<ID3D11DepthStencilView> depth;
            context->OMGetRenderTargets(1,&target,&depth);
            if (!target || !depth) return token;
            ComPtr<ID3D11RasterizerState> original; context->RSGetState(&original);
            ComPtr<ID3D11RasterizerState> clamped;
            {
                std::lock_guard lock(mutex_);
                const auto found=std::find_if(cache_.begin(),cache_.end(),[&](const Entry& e){return e.original.Get()==original.Get();});
                if (found!=cache_.end()) clamped=found->clamped;
                else {
                    D3D11_RASTERIZER_DESC desc{}; desc.FillMode=D3D11_FILL_SOLID; desc.CullMode=D3D11_CULL_BACK; desc.DepthClipEnable=TRUE;
                    if (original) original->GetDesc(&desc);
                    if (desc.DepthClipEnable) {
                        if (cache_.size()>=cacheLimit) { ++cacheFull_; return token; }
                        desc.DepthClipEnable=FALSE;
                        ComPtr<ID3D11Device> device; context->GetDevice(&device);
                        if (!device || FAILED(device->CreateRasterizerState(&desc,&clamped)) || !clamped) { ++failures_; return token; }
                    }
                    cache_.push_back({original,clamped}); // null clamped: already unclipped
                }
            }
            if (!clamped) return token;
            context->RSSetState(clamped.Get());
            token.original=original; token.active=true; ++draws_;
        } catch (...) { token={}; }
        return token;
    }
    void End(ID3D11DeviceContext* context,Token& token) noexcept {
        if (!token.active || !context) return;
        context->RSSetState(token.original.Get()); token={};
    }
    uint64_t Draws() const noexcept { return draws_.load(); }
    uint64_t Failures() const noexcept { return failures_.load(); }
    uint64_t CacheFull() const noexcept { return cacheFull_.load(); }
    size_t States() noexcept { std::lock_guard lock(mutex_); return cache_.size(); }
private:
    struct Entry { Microsoft::WRL::ComPtr<ID3D11RasterizerState> original,clamped; };
    std::atomic<bool> active_{};
    std::atomic<uint64_t> draws_{},failures_{},cacheFull_{};
    std::mutex mutex_;
    std::vector<Entry> cache_;
};
inline DepthClamp& GlobalDepthClamp() { static DepthClamp clamp; return clamp; }
}
