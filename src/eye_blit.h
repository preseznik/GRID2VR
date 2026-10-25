#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>

namespace grid2vr {
// Call the unhooked Draw entry point so this copy cannot enter engine tracing.
using NativeEyeDraw=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
class EyeBlit {
public:
    bool Initialize(ID3D11Device* device,NativeEyeDraw draw);
    bool Draw(unsigned eye,ID3D11Texture2D* source,ID3D11RenderTargetView* target);
private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>,2> sources_;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>,2> views_;
    NativeEyeDraw draw_{};
    DWORD thread_{};
};
}
