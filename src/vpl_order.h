#pragma once
#include <d3d11.h>
#include <wrl/client.h>

namespace grid2vr {
using NativeDispatch = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT);
// Diagnostic canonicalization for the fingerprinted shader's 16x32 VPL tiles.
// Sorts only the zero-terminated active prefix; leaves the terminator/tail intact.
class VplOrder {
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
public:
    HRESULT Apply(ID3D11DeviceContext* context,ID3D11UnorderedAccessView* view,
                  UINT groupsX,UINT groupsY,UINT groupsZ,NativeDispatch dispatch,bool& restored);
};
}
