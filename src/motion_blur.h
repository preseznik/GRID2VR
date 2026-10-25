#pragma once
#include "vpl_order.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>

namespace grid2vr {
class MotionBlurControl {
public:
    static bool KnownShader(uint64_t hash) {
        return hash==0x6e44f31d1ebda08dull || hash==0xed4ddaac90aff677ull ||
            hash==0xb02f3b98b3596660ull || hash==0xcfc3cf441f08e879ull;
    }
    bool Apply(ID3D11DeviceContext* context,uint64_t shader,NativeDispatch dispatch);
    bool Restore(ID3D11DeviceContext* context);
    bool Pending() const { return original_!=nullptr; }
    // Native pause/overlay frames may omit blur. Both eyes must follow the
    // center frame's zero-or-one pass schedule, with no temporary binding left.
    bool PassComplete(unsigned centerDraws,unsigned currentDraws) const {
        return !Pending() && centerDraws<=1 && currentDraws==centerDraws;
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
