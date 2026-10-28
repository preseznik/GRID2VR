#pragma once
#include "screen_ui_projection.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <filesystem>
#include <set>
#include <tuple>

namespace grid2vr {
// Flat native menus only. Correct the two verified UI camera families before
// clipping, using owned bindings; restore every changed binding after the draw.
class FlatScreenUi {
public:
    bool Configure(const std::filesystem::path& folder);
    bool Enabled() const { return width_!=0; }
    bool Apply(ID3D11DeviceContext* context,uint64_t shader);
    bool Restore(ID3D11DeviceContext* context);
    bool Reset();
    bool Pending() const { return pending_; }
private:
    unsigned width_{},height_{},seedAttempts_{};
    ID3D11DeviceContext* owner_{};
    DWORD thread_{};
    Microsoft::WRL::ComPtr<ID3D11Buffer> replacement_,originalVs_,originalPs_;
    std::array<D3D11_RECT,D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors_{};
    UINT scissorCount_{};
    bool pending_{},changedPs_{},changedScissors_{};
};
// Verified native UI shaders/cameras only. Post-VS projection preserves native
// artwork varyings and original canvas clipping. Scoped to one triangle draw.
class ScreenUi {
public:
    enum class Result { Skipped, Applied, Failed };
    static void Upload(ID3D11Resource* resource,const void* data,UINT bytes);
    Result Apply(ID3D11DeviceContext* context,uint64_t vertexShader,const SceneViews& views,unsigned eye);
    bool Restore(ID3D11DeviceContext* context);
    bool Pending() const { return pending_; }
    void Diagnose(const std::filesystem::path& folder,bool late=false);
    void Observe(ID3D11DeviceContext* context,uint64_t shader,uint64_t frame=0,uintptr_t nativeTarget=0);
    const ScreenUiOptions& Options() const { return options_; }
private:
    void Record(ID3D11DeviceContext* context,ID3D11Buffer* buffer,uint64_t shader,uint64_t frame=0,uintptr_t nativeTarget=0);
    std::filesystem::path output_;
    // At most 64 shader/camera witnesses and 32 resource readbacks per session.
    std::set<std::tuple<uint64_t,uint64_t,bool>> samples_;
    unsigned sampleAttempts_{};
    ScreenUiOptions options_;
    bool invalidOptions_{},pending_{},late_{};
    Microsoft::WRL::ComPtr<ID3D11Buffer> original_,replacement_;
    std::array<Microsoft::WRL::ComPtr<ID3D11GeometryShader>,4> shaders_;
    ID3D11DeviceContext* owner_{};
    DWORD thread_{};
    std::array<D3D11_RECT,D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors_{};
    UINT scissorCount_{};
    bool changedScissors_{};
};

// Read-only coverage between a completed native scene and Present. This owns
// no images or XR frame and never calls the late-UI rendering candidate.
class ScreenUiNativeWitness {
public:
    void Diagnose(const std::filesystem::path& folder);
    bool Begin(ID3D11DeviceContext* context,uint64_t frame,uintptr_t nativeTarget);
    void Observe(ID3D11DeviceContext* context,uint64_t frame,uint64_t shader);
    void End(bool force=false);
    uint64_t Frame() const { return frame_; }
private:
    ScreenUi ui_;
    std::filesystem::path output_;
    ID3D11DeviceContext* context_{};
    DWORD thread_{};
    uintptr_t nativeTarget_{}; // diagnostic identity only; never dereferenced
    uint64_t frame_{},lastFrame_{},frames_{},published_{};
    bool changed_{};
    std::array<uint64_t,screenUiShaders.size()+screenUiObservedShaders.size()> counts_{};
};

// Normal-mode front-end UI can be drawn after the complete 3D scene. Replay
// only verified screen-camera draws onto the already completed, same-frame
// eyes. No world rendering, game simulation, or native UI state is rerun.
class LateScreenUi {
public:
    using NativeDraw=void (*)(ID3D11DeviceContext*,void*);
    void Diagnose(const std::filesystem::path& folder);
    bool Begin(ID3D11DeviceContext* context,uint64_t frame,const SceneViews& views,
        ID3D11Texture2D* native,ID3D11Texture2D* left,ID3D11Texture2D* right);
    ScreenUi::Result Draw(ID3D11DeviceContext* context,uint64_t frame,uint64_t shader,NativeDraw draw,void* args);
    void End(bool forceReceipt=false);
    void Reset();
    uint64_t Frame() const { return frame_; }
    bool OnThread() const { return context_ && thread_==GetCurrentThreadId(); }
private:
    ScreenUi ui_;
    SceneViews views_;
    std::filesystem::path output_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> native_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>,2> eyes_;
    std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>,2> targets_;
    ID3D11DeviceContext* context_{};
    DWORD thread_{};
    uint64_t frame_{},lastFrame_{},prepared_{},pairedDraws_{},failed_{},published_{};
    std::array<uint64_t,6> skipped_{}; // target, depth/stencil, UAV/SO, predicate, sample mask, camera/state
    unsigned reasons_{},publishedReasons_{};
};
}
