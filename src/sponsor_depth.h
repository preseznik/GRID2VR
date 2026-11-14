#pragma once
#include "sponsor_depth_model.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <atomic>
#include <array>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace grid2vr {
// Opt-in (sponsor-depth-bias.request): stock sponsor textures are recognised at
// creation by the hash of mip 0 and tagged, with their views. A draw that samples a
// tagged view, writes colour and depth, and uses a LESS/GREATER-family depth test runs
// with an owned copy of the bound rasterizer state carrying an extra depth bias toward
// the viewer; the game's own state is rebound right after the draw. Game files, shaders,
// geometry and the depth range are unchanged. Any failure leaves the draw native.
class SponsorDepth {
public:
    struct Token { Microsoft::WRL::ComPtr<ID3D11RasterizerState> original;bool active{}; };
    void Configure(const std::filesystem::path& run) noexcept;
    bool Enabled() const noexcept { return enabled.load(); }
    void HookFailed(const char* reason) noexcept;
    void TextureCreated(const D3D11_TEXTURE2D_DESC* requested,const D3D11_SUBRESOURCE_DATA* data,ID3D11Texture2D* texture) noexcept;
    void ViewCreated(ID3D11Resource* resource,ID3D11ShaderResourceView* view) noexcept;
    // Binding tracking: once PSSetShaderResources, ClearState and SwapDeviceContextState are hooked for the
    // immediate context, draws there with no sponsor view bound skip the readback.
    // Other contexts, and any disagreement found by the periodic audit, use the full check.
    void Track(ID3D11DeviceContext* immediate) noexcept;
    void ResourcesBound(ID3D11DeviceContext* context,UINT start,UINT count,ID3D11ShaderResourceView* const* views) noexcept;
    void StateCleared(ID3D11DeviceContext* context) noexcept;
    // SwapDeviceContextState: the outgoing state's slots are kept with it and come back
    // when it is swapped in again; a state never seen leaves every slot unknown.
    void StateSwapped(ID3D11DeviceContext* context,ID3DDeviceContextState* outgoing,ID3DDeviceContextState* incoming) noexcept;
    Token Begin(ID3D11DeviceContext* context) noexcept;
    void End(ID3D11DeviceContext* context,Token& token) noexcept;
    void Summary() noexcept;
#ifdef GRID2VR_SPONSOR_DEPTH_TEST
    void ConfigureTest(const std::filesystem::path& run,std::vector<std::string> identities,sponsordepth::Settings settings);
    static std::string Mip0Hash(const D3D11_SUBRESOURCE_DATA* data);
#endif
private:
    bool Sponsor(ID3D11ShaderResourceView* view) const noexcept;
    bool BoundSponsor(ID3D11DeviceContext* context) const noexcept;
    ID3D11DeviceContext* tracked{};   // not owned; compared only
    sponsordepth::BoundSlots slots;
    std::atomic<bool> tracking{};
    unsigned auditTick{};
    const char* trackingState="off";
    std::atomic<uint64_t> fastSkips{},audits{},auditMismatches{};
    struct SavedSlots { Microsoft::WRL::ComPtr<ID3DDeviceContextState> state;sponsordepth::BoundSlots slots; };
    std::array<SavedSlots,16> saved{};
    struct Cached { Microsoft::WRL::ComPtr<ID3D11RasterizerState> original,biased;int sign{}; };
    std::atomic<bool> enabled{};
    std::filesystem::path folder;
    sponsordepth::Settings settings;
    std::vector<std::string> identities;
    std::mutex cacheMutex,logMutex;
    std::vector<Cached> cache;
    // Published slots are immutable. Retaining views prevents pointer reuse;
    // after the bound is reached, uncached views use the original tag lookup.
    static constexpr unsigned viewCacheLimit=128;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>,viewCacheLimit> retainedViews{};
    std::atomic<unsigned> retainedViewCount{};
    std::atomic<bool> viewCacheFull{};
    std::string refusal;
    std::atomic<unsigned> candidates{},withoutData{},matched{},taggedViews{},biasedDraws{},
        skippedTarget{},skippedDepthTest{},stateFailures{},cacheFull{},cachedStates{},summaries{};
};
}
