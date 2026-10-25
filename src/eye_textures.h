#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace grid2vr {
// Owned single-sample images: two diagnostic views, or center/left/right.
// Retrieval requires the complete same-frame group. Allocation is reused.
class EyeTextures {
public:
    bool Begin(uint64_t frame,unsigned count=2) {
        count_=0; collecting_=false;
        if (!frame || frame<=frame_ || count<2 || count>3) return false;
        frame_=frame; target_=count; collecting_=true; return true;
    }
    bool Capture(ID3D11DeviceContext* context,uint64_t frame,unsigned eye,ID3D11Texture2D* source);
    bool Captured(uint64_t frame,unsigned count) const { return frame==frame_ && count>0 && count_==count; }
    bool Ready(uint64_t frame) const { return Captured(frame,target_); }
    // Invalidate publication and release owned images between restored sessions.
    // Keep frame_ monotonic so an old scene can never become current again.
    void Reset() { count_=allocated_=0;collecting_=false;images_={};device_.Reset();description_={};resolveContext_.Reset();resolveState_.Reset(); }
    ID3D11Texture2D* Get(uint64_t frame,unsigned eye) const {
        return Ready(frame) && eye<target_ ? images_[eye].Get():nullptr;
    }
private:
    uint64_t frame_{};
    unsigned count_{};
    unsigned target_{2},allocated_{};
    bool collecting_{};
    D3D11_TEXTURE2D_DESC description_{};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> resolveContext_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> resolveState_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>,3> images_;
};
}
