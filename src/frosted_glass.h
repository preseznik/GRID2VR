#pragma once
#include "vpl_order.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace grid2vr {
// GRID 2's menu cards (endgame garage car list, post-race rewards) use the
// frosted-glass UI shaders in the shipped shaderpack/dx11/ui.pssg. Each
// returns lerp(panel colour, background, FrostedGlassIntensity*0.01), where
// background samples TFrostedGlassBackground, a blurred copy of the game's own
// screen, at the card's position on the game's canvas. In VR that copy is not
// the eye's view, so the scene behind the card shows displaced and moves with
// the head.
//
// The bokeh UI shaders do the same with TBokehReflection: lerp(panel colour,
// max of three samples at the card's canvas position, _BokehReflectIntensity
// *0.01), with the intensity at the same $Globals offsets.
//
// For one matching draw, an owned $Globals copy with the intensity at 0
// makes the game's own shader return its plain panel colour (alpha is the
// panel's own). Every other byte and binding is kept. The two bokeh
// "fake mask" shaders output the background itself and have no intensity;
// they are left native and only counted.
class FrostedGlass {
public:
    struct Shader {
        uint64_t hash;
        UINT intensityOffset; // FrostedGlassIntensity / _BokehReflectIntensity in $Globals.
        UINT constantBytes;   // Declared $Globals size.
        const char* name;
    };
    static constexpr std::array<Shader,6> shaders{{
        {0x7899d0ed7e9690ddull,152,160,"ui_frosted_glass"},
        {0x98f5a937f5cf8201ull,152,160,"ui_frosted_glass_image"},
        {0x25d30096ffcb1ec4ull,168,208,"ui_frosted_glass_text"},
        {0x5eb4ebd096e2b07bull,152,160,"ui_bokeh_reflection"},
        {0xdc6f3d48c65d8c90ull,152,160,"ui_bokeh_image"},
        {0xdcb92a30d574cfe3ull,168,208,"ui_bokeh_text"},
    }};
    // Sample the background with no intensity control; observed only.
    static constexpr std::array<uint64_t,2> maskShaders{0x530c96bb18f7d3b0ull,  // ui_bokeh_fake_mask_image
                                                        0x3d824730db658e94ull}; // ui_bokeh_fake_mask_text
    static bool IsMask(uint64_t hash) {
        for (auto mask:maskShaders) if (mask==hash) return true;
        return false;
    }
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
        UINT bytes{},offset{};
        Microsoft::WRL::ComPtr<ID3D11Buffer> raw,filtered;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    };
    bool Initialize(ID3D11DeviceContext* context);
    Copy* For(UINT bytes,UINT offset);
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
    // One compute shader per intensity offset (152, 168).
    std::array<Microsoft::WRL::ComPtr<ID3D11ComputeShader>,2> zero_;
    std::array<Copy,6> copies_{};
    Microsoft::WRL::ComPtr<ID3D11Buffer> original_;
    Copy* active_{};
    DWORD thread_{};
};
}
