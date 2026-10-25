#pragma once
#include "vpl_order.h"
#include <functional>

namespace grid2vr {
// Diagnostic only: expose exact RGB10A2 samples through a UINT view of an owned
// MSAA copy. Native targets stay bound and untouched. The callback must not bind
// pipeline state; its single-sample R32_UINT texture is overwritten next sample.
class MsaaReadback {
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
public:
    HRESULT Read(ID3D11DeviceContext* context,ID3D11Texture2D* source,NativeDispatch dispatch,
                 const std::function<void(ID3D11Texture2D*,UINT)>& consume,bool& restored);
};
}
