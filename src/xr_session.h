#pragma once
#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>
#include <array>
#include <filesystem>
#include <fstream>
#include "xr_eye_images.h"
#include "bounded_log.h"
#include "performance_display.h"

namespace grid2vr {
// All calls, including explicit Shutdown, belong to the initializing render
// thread. Never destroy OpenXR objects from DllMain or a global destructor.
class XrSessionProbe {
public:
    struct LocatedViews {
        uint64_t frame{};
        XrTime displayTime{};
        bool tracked{},shouldRender{};
        XrPosef head{};
        std::array<XrView,2> eyes{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};
    };
    bool Initialize(ID3D11Device* device,const std::filesystem::path& runtime,const std::filesystem::path& output);
    bool InitializeImages(NativeEyeDraw draw,unsigned width,unsigned height);
    bool Poll();
    bool BeginFrame(uint64_t frame);
    const LocatedViews* Views() const { return frameOpen_?&located_:nullptr; }
    bool FinishFrame(uint64_t frame,bool rendererAccepted,ID3D11Texture2D* left=nullptr,ID3D11Texture2D* right=nullptr);
    bool Sample(uint64_t frame);
    bool PresentScreen(uint64_t frame,ID3D11Texture2D* backbuffer);
    // Explicit recenter only. Normal head movement and mode changes retain the
    // same level LOCAL-space screen instead of following the headset.
    void ResetScreenAnchor() { if (OnThread()) screenAnchorReady_=false; }
    uint64_t LastFrame() const { return frame_; }
    void Shutdown();
    bool Closed() const { return !instance_ && !session_ && !local_ && !head_ && !frameOpen_; }
    bool Failed() const { return failed_; }
    bool Running() const { return running_; }
    bool Focused() const { return running_ && state_==XR_SESSION_STATE_FOCUSED && !failed_; }
    bool ReadyForViews(bool requireFocus=false) const { return running_ && Visible() && sampledVisible_ && !failed_ && !frameOpen_ && located_.tracked && located_.shouldRender && (!requireFocus || (Focused() && sampledFocused_)); }
    unsigned Samples() const { return samples_; }
    XrSessionProbe()=default;
    XrSessionProbe(const XrSessionProbe&)=delete;
    XrSessionProbe& operator=(const XrSessionProbe&)=delete;
private:
    void Log(const char* format,...);
    bool Check(XrResult result,const char* operation);
    bool EndFrame(const XrCompositionLayerBaseHeader* layer=nullptr);
    bool OnThread() const { return thread_==GetCurrentThreadId(); }
    bool Visible() const { return state_==XR_SESSION_STATE_VISIBLE || state_==XR_SESSION_STATE_FOCUSED; }
    BoundedLog log_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    XrInstance instance_{};
    XrSession session_{};
    XrSpace local_{},head_{};
    DWORD thread_{};
    XrTime displayTime_{};
    uint64_t frame_{};
    unsigned samples_{};
    bool running_{},frameOpen_{},failed_{};
    XrSessionState state_{XR_SESSION_STATE_UNKNOWN};
    bool sampledVisible_{};
    bool sampledFocused_{};
    LocatedViews located_;
    XrEyeImages images_;
    PerformanceDisplay performance_;
    bool imagesAttempted_{},imagesReady_{};
    unsigned imagesSubmitted_{};
    unsigned screensSubmitted_{};
    bool diagnosticLogging_{true};
    XrPosef screenAnchor_{};
    bool screenAnchorReady_{};
};
}
