#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>

namespace grid2vr {
// Diagnostic desktop preview: an exact copy of an already completed eye.
// The caller owns same-frame publication and must not present a partial pair.
// No source or backbuffer references survive Copy; there is no scaling or gamma conversion.
class EyeMirror {
public:
    bool Initialize(ID3D11Device* device);
    bool Copy(ID3D11DeviceContext* context,ID3D11Texture2D* source,ID3D11Texture2D* destination);
private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
    DWORD thread_{};
};
}
