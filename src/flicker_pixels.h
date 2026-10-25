#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <string>

namespace grid2vr::flicker {
// Passive copies around one existing draw. No substitute draw, mask shader,
// binding change or depth change; at most three 8 MiB staging textures.
class PixelWitness {
public:
    void Reset();
    void Before(ID3D11DeviceContext* context);
    void After(ID3D11DeviceContext* context);
    std::string Finish(ID3D11DeviceContext* context,ID3D11Texture2D* finalSource);
private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> target_,before_,after_;
    D3D11_TEXTURE2D_DESC desc_{};
    const char* status_{"not_captured"};
};
}
