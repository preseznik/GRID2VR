#pragma once
#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <vector>
#include "performance_stats.h"

namespace grid2vr {
struct GpuCounterState;
// Optional independent compositor panel. No game shader, render target or HUD
// geometry is changed. All XR/GPU calls remain on the owning render thread.
class PerformanceDisplay {
public:
    bool Initialize(XrSession session,XrSpace head,XrSpace local,ID3D11Device* device,const std::filesystem::path& output);
    const XrCompositionLayerQuad* Layer(uint64_t milliseconds,const XrPosef* anchor=nullptr);
    void Submitted(uint64_t milliseconds) {if(enabled_) rate_.Frame(milliseconds);}
    bool Shutdown();
    bool Enabled() const {return enabled_;}
    bool FollowsView() const {return followView_;}
private:
    static DWORD WINAPI SampleGpu(void* self);
    void WriteReceipt(double gpu,bool stopped=false);
    bool Upload(uint64_t milliseconds);
    XrSwapchain chain_{};
    XrCompositionLayerQuad layer_{XR_TYPE_COMPOSITION_LAYER_QUAD};
    std::vector<XrSwapchainImageD3D11KHR> images_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Query> completion_;
    std::filesystem::path output_;
    SubmittedFrameRate rate_;
    std::shared_ptr<GpuCounterState> counter_;
    std::wstring adapter_;
    DWORD thread_{};
    uint64_t updated_{};
    uint32_t index_{};
    bool followView_{true};
    bool enabled_{},ready_{},used_{},acquired_{},waited_{};
};
}
