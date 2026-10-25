#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <bit>
#include <cmath>

namespace grid2vr {
// A draw-local replacement for the fingerprinted 1x1 exposure-meter pass.
// Bloom/final composition and all native buffers stay untouched.
class CockpitExposure {
public:
    static constexpr uint64_t shaderHash=0xccaee125ba648054ull;
    static bool CameraSupported(uint32_t nearPlane,uint32_t farPlane) {
        // Our camera assets uniquely use .02. Native world far clips vary by
        // route (Barcelona is 2300); exclude the fixed 100-unit menu subpass.
        const auto farClip=std::bit_cast<float>(farPlane);
        return nearPlane==0x3ca3d70au && std::isfinite(farClip) && farClip>100 && farClip<=15000;
    }
    bool Apply(ID3D11DeviceContext* context,uint64_t hash);
    bool Restore(ID3D11DeviceContext* context);
    bool Pending() const { return original_!=nullptr; }
    bool PassComplete(unsigned center,unsigned current) const {
        return !Pending() && center<=1 && center==current;
    }
private:
    Microsoft::WRL::ComPtr<ID3D11PixelShader> filtered_,original_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    DWORD thread_{};
};
}
