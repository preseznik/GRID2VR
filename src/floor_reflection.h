#pragma once
#include "vpl_order.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace grid2vr {
// The garage floor (garage_floor_reflect and garage_floor_reflect_refract in
// shipped shaderpack/dx11/shaderpack.pssg) samples a planar reflection map at
// the pixel's screen position (SV_Position / viewport, TReflectionMap or
// TReflectionMap2 by CameraParams leftEye). That map is rendered for the
// game's own camera, so in a posed, wider headset eye the reflected cars and
// ceiling lights land in the wrong place on the floor.
//
// Both shaders blend lerp(surface, reflection, fresnel) with
// fresnel=lerp(FresnelMin,FresnelMax,f)*specular, and add a glow of
// max(reflection-1,0)*0.2. For one matching draw, an owned $Globals copy with
// FresnelMin=FresnelMax=0 (bytes 240..247, the same in both layouts) and a
// black texture in both reflection slots make the game's own shader return
// its surface without any reflection. Every other byte and binding is kept.
class FloorReflection {
public:
    struct Shader {
        uint64_t hash;
        UINT reflectionSlot; // TReflectionMap; TReflectionMap2 is the next slot.
        UINT constantBytes;  // Declared $Globals size.
    };
    static constexpr std::array<Shader,2> shaders{{
        {0xd9571afb84cec177ull,4,288}, // garage_floor_reflect.fx.fp
        {0x79da3d0d7215a660ull,3,304}, // garage_floor_reflect_refract.fx.fp
    }};
    static constexpr unsigned fresnelOffset=240;
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
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> black_;
    std::array<Copy,4> copies_{};
    Microsoft::WRL::ComPtr<ID3D11Buffer> original_;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>,2> originalViews_{};
    Copy* active_{};
    UINT slot_{};
    DWORD thread_{};
};
}
