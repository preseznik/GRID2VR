#pragma once
#include "vpl_order.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace grid2vr {
// GRID 2's lens flare is screen-space "ghosts" in the UniFinal post-process
// (shipped shaderpack/dx11/postprocess.pssg): fp_ghostsL, fp_ghostsM and
// fp_ghostsM2 sample the bright image at scaled, mirrored screen coordinates
// (lensFlareParams, $Globals 208..271) and add each sample tinted by
// lensFlareParams2 rows 0-2 ($Globals 272..319). Mirrored about each eye's own
// image centre, the ghosts land in different places in the two eyes.
//
// For one matching draw, an owned $Globals copy with those three tint rows at
// zero removes every ghost. The shaders' other work is unchanged: fp_ghostsM
// still adds its previous result and fp_ghostsM2 still merges the medium and
// large bloom and its own input. Every other byte and binding is kept.
class LensFlareGhosts {
public:
    struct Shader {
        uint64_t hash;
        UINT constantBytes; // Declared $Globals size.
        const char* name;
    };
    static constexpr std::array<Shader,3> shaders{{
        {0xb6247b71227a095full,336,"fp_ghostsL"},
        {0x1b484999d8a78af0ull,336,"fp_ghostsM"},
        {0x280cc622b338b43bull,1008,"fp_ghostsM2"},
    }};
    static constexpr UINT tintOffset=272,tintBytes=48;
    static const Shader* Find(uint64_t hash) {
        for (const auto& shader:shaders) if (shader.hash==hash) return &shader;
        return nullptr;
    }
    // Refuses before changing any binding.
    bool Apply(ID3D11DeviceContext* context,uint64_t hash,NativeDispatch dispatch);
    bool Restore(ID3D11DeviceContext* context);
    bool Pending() const { return original_!=nullptr; }
private:
    struct Copy {
        UINT bytes{};
        Microsoft::WRL::ComPtr<ID3D11Buffer> raw,filtered;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    };
    bool Initialize(ID3D11DeviceContext* context);
    Copy* For(UINT bytes);
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
    std::array<Copy,4> copies_{};
    Microsoft::WRL::ComPtr<ID3D11Buffer> original_;
    Copy* active_{};
    DWORD thread_{};
};
}
