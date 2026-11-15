#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace grid2vr {
// Diagnostic only (ProJict 18da44dc), requested by mirror-source.request.
// GRID 2 has no mirror camera: the wing-mirror glass is ordinary car glass,
// and the car_basic / pfx_mesh_car_paint shaders reflect by blending the
// current screen image (_TScreenReflection, sampled at the reflected
// direction projected on screen) with the car's environment cube
// (_TEnvironmentMap). Both inputs are HDR-encoded, so a solid colour decodes
// to a blinding value; black decodes to zero under any encoding. For one
// matching draw this binds black in one input only (the screen image, or the
// cube once mirror-source-cube.request exists), then restores it. Captures
// in each phase show which input fills the mirrors. Nothing else is changed.
class MirrorSourceProbe {
public:
    static constexpr UINT none=~0u;
    struct Shader {
        uint64_t hash;
        UINT screenSlot; // _TScreenReflection (Texture2D), or none.
        UINT cubeSlot;   // _TEnvironmentMap (TextureCube), or none.
    };
    // Every shipped shaderpack/dx11/shaderpack.pssg pixel shader that
    // declares either input (39; SHA-256 8c86662f…).
    static constexpr std::array<Shader,39> shaders{{
        {0x781abd804b93a4b0ull,none,7}, // car_basic.cg.fp_carbon_fiber.FBF1B90E
        {0xb4b3bc4b691a6570ull,none,4}, // car_basic.cg.fp_light_pod.4359E7BF
        {0xf22b9b0b154da7deull,none,4}, // car_basic.cg.fp_wheel.047156E8
        {0x4c26144a4da6a897ull,8,7}, // car_basic.cg.fp_plastic.6BDE810C
        {0xf5b2d15ea3ab03dbull,none,3}, // car_basic.cg.fp_animated_light_pod.E8884D69
        {0xaf809e88e6c0b78bull,none,4}, // car_basic.cg.fp_light_glass.409929DD
        {0x33e97048f6d61f6cull,none,5}, // car_basic.cg.fp_light_glass.4359E7BF
        {0x29314f85a3e8c74bull,4,3}, // car_basic.cg.fp_glass.6C07276F
        {0x5036b5f4a70e7144ull,8,7}, // car_basic.cg.fp_paint.6C07276F
        {0x2b4339129a75f03aull,none,4}, // car_basic.cg.fp_plastic.409929DD
        {0xbb9f3f92e5284d58ull,8,7}, // car_basic.cg.fp_carbon_fiber.6C07276F
        {0x67ee7fc197d98ea8ull,none,7}, // car_basic.cg.fp_plastic.4359E7BF
        {0xbf584ebeb243d572ull,none,3}, // car_basic.cg.fp_basic_cheap.49C56708
        {0x1de47a16e81ec021ull,none,4}, // car_basic.cg.fp_animated_light_pod.409929DD
        {0x1d507b4693487b64ull,6,5}, // car_basic.cg.fp_glass_wet.F4B69BE2
        {0x45e0b0fe2598602aull,none,3}, // car_basic.cg.fp_animated_light_pod.4359E7BF
        {0x4ff3b0f9f68a2953ull,none,1}, // pfx_mesh_car_paint.cg.fp.4359E7BF
        {0xb2b20a8d695630a9ull,10,9}, // car_basic.cg.fp_plastic_wet.F4B69BE2
        {0xb598a405e2ff2502ull,none,4}, // car_basic.cg.fp_wheel.E8884D69
        {0x99be86683b6f5407ull,none,3}, // car_basic.cg.fp_glass.F350D1ED
        {0xb1032cca40b6554aull,4,3}, // car_basic.cg.fp_glass.6BDE810C
        {0xbd95611039454751ull,8,7}, // car_basic.cg.fp_paint.6BDE810C
        {0x8710ea348c27d7fbull,8,7}, // car_basic.cg.fp_carbon_fiber.6BDE810C
        {0x0d2e8ab16f07aabfull,none,7}, // car_basic.cg.fp_plastic.FBF1B90E
        {0xa973128d6a09f940ull,none,3}, // car_basic.cg.fp_glass.409929DD
        {0xe8513731601b437eull,none,5}, // car_basic.cg.fp_paint.409929DD
        {0x3ba64961c2b4a5b0ull,none,3}, // car_basic.cg.fp_glass.4359E7BF
        {0x8882fc9009f90562ull,none,7}, // car_basic.cg.fp_paint.4359E7BF
        {0x595323a7cd54848dull,none,4}, // car_basic.cg.fp_wheel.409929DD
        {0x2874dbafa9f21d12ull,none,1}, // pfx_mesh_car_paint.cg.fp.FBF1B90E
        {0xcce0e320e4e7c560ull,none,4}, // car_basic.cg.fp_light_glass_cheap.F350D1ED
        {0x26fecba8aea2a914ull,none,7}, // car_basic.cg.fp_carbon_fiber.4359E7BF
        {0xfc849cd87cf4f21eull,none,4}, // car_basic.cg.fp_light_pod.E8884D69
        {0xda9f2ec75c9f8df7ull,8,7}, // car_basic.cg.fp_plastic.6C07276F
        {0xd075a3791f81b242ull,10,9}, // car_basic.cg.fp_paint_wet.F4B69BE2
        {0xe79606d6ba226f2dull,none,5}, // car_basic.cg.fp_light_glass.E8884D69
        {0xb1841e31a7f08180ull,10,9}, // car_basic.cg.fp_carbon_fiber_wet.F4B69BE2
        {0x6e20be966255a9b5ull,none,7}, // car_basic.cg.fp_paint.FBF1B90E
        {0x402f7420ca7fe434ull,none,3}, // car_basic.cg.fp_light_pod_cheap.F350D1ED
    }};
    static const Shader* Find(uint64_t hash) {
        for (const auto& shader:shaders) if (shader.hash==hash) return &shader;
        return nullptr;
    }
    enum class Result { Applied,Refused,Failed };
    // Refused: the bound inputs aren't the expected kind (no binding changed).
    // Failed: the tint views couldn't be made or bound (bindings restored).
    Result Apply(ID3D11DeviceContext* context,uint64_t hash);
    void BlankCube(bool cube) { cube_=cube; }
    // Reads back mip 0 of every face of the cube bound for this matching draw
    // into <folder>/mirror-cube-<n>-face<i>.bin plus a line in `summary`.
    // Each distinct cube resource is dumped once, at most four in total.
    bool DumpCube(ID3D11DeviceContext* context,uint64_t hash,const std::filesystem::path& folder,std::string& summary);
    unsigned Dumped() const { return dumped_; }
    bool BlankingCube() const { return cube_; }
    bool Restore(ID3D11DeviceContext* context);
    bool Pending() const { return pending_; }
private:
    bool Initialize(ID3D11DeviceContext* context);
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> flat_,cube_view_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>,2> original_{};
    std::array<UINT,2> slots_{none,none};
    bool pending_{},cube_{};
    std::array<ID3D11Resource*,4> dumpedResources_{}; // Identity only; never dereferenced.
    unsigned dumped_{};
    DWORD thread_{};
};
}
