#pragma once
#include "vpl_order.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>

namespace grid2vr {
// GRID 2's impact/camera-collision "interference" glitch is a screen-space pass
// after UniFinal: flat in VR, it reads as a second picture over the scene.
// Its only pixel shader (Interference.UniFinal, shipped postprocess.pssg) glitches
// a pixel only when the interference map value is below cb0[78].w, the
// interferenceParams gate at byte 1260 of the 1296-byte post-process constants.
// Binding an owned copy whose gate is -FLT_MAX makes the game's own shader take
// its unmodified pass-through branch; every other byte and binding is retained.
class ImpactInterference {
public:
    static constexpr uint64_t shaderHash=0x2d81beb22b54d6ccull;
    static constexpr unsigned gateOffset=1260;
    static constexpr uint32_t gateValue=0xff7fffffu;
    bool Apply(ID3D11DeviceContext* context,NativeDispatch dispatch);
    bool Restore(ID3D11DeviceContext* context);
    bool Pending() const { return original_!=nullptr; }
    // Both eyes follow the centre frame's zero-or-one pass schedule.
    bool PassComplete(unsigned center,unsigned current) const {
        return !Pending() && center<=1 && center==current;
    }
private:
    bool Initialize(ID3D11DeviceContext* context);
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> raw_,filtered_,original_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav_;
    DWORD thread_{};
};
}
