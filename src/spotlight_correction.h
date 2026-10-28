#pragma once
#include "spotlight_projection.h"
#include "vpl_order.h"
#include <d3d11_1.h>
#include <array>
#include <cstdint>
#include <wrl/client.h>

namespace grid2vr {
// Exact spotlight draws only. GPU snapshots retain the same-scene native
// constants; a compute guard copies mismatching inputs through unchanged.
class SpotlightCorrection {
public:
    struct DrawKey {
        unsigned slot{},ordinal{};
        uint64_t kindHash{};
        std::array<UINT,5> args{};
        unsigned argCount{};
        uint64_t vsHash{},psHash{};
        bool operator==(const DrawKey&) const = default;
    };
    enum class Result { Skipped, Captured, Applied, Failed };
    struct Statistics {
        uint64_t captures{},submissions{},missingCenter{},overflow{};
        // Sparse, asynchronous GPU samples. Applied means submitted, whereas
        // these counts report the actual guard result; unsampled draws differ.
        uint64_t samples{},corrected{},pixelMismatch{},vertexMismatch{},invalidConstants{};
    };
    void Configure(NativeDispatch dispatch) { dispatch_=dispatch; }
    bool BeginScene(uint64_t scene);
    Result BeforeDraw(ID3D11DeviceContext* context,const DrawKey& key,unsigned pass,
                      const spotlightprojection::Change* change=nullptr);
    bool AfterDraw(ID3D11DeviceContext* context);
    bool ResetResources();
    bool Pending() const { return original_!=nullptr; }
    const Statistics& Stats() const { return statistics_; }
private:
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Snapshot { DrawKey key{}; Ptr<ID3D11Buffer> pixel,vertex; };
    struct Output { Ptr<ID3D11Buffer> raw,constant;Ptr<ID3D11UnorderedAccessView> view; };
    struct Sample { Ptr<ID3D11Buffer> staging;Ptr<ID3D11Query> ready;bool pending{}; };
    static constexpr unsigned MaxSnapshots=128,MaxSamples=32;
    bool Initialize(ID3D11DeviceContext* context);
    bool Buffer(Ptr<ID3D11Buffer>& buffer,UINT bytes);
    void PollSamples();
    void SampleGuard(unsigned pass);
    NativeDispatch dispatch_{};
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11DeviceContext1> context_;
    Ptr<ID3DDeviceContextState> state_;
    std::array<Ptr<ID3D11ComputeShader>,2> shaders_;
    Ptr<ID3D11Buffer> parameters_,status_,original_;
    Ptr<ID3D11UnorderedAccessView> statusView_;
    std::array<Output,2> output_;
    std::array<Snapshot,MaxSnapshots> snapshots_;
    std::array<Sample,4> samples_;
    ID3D11DeviceContext* owner_{};
    DWORD thread_{};
    uint64_t scene_{},sceneCount_{};
    unsigned count_{},sampleMask_{},samplesIssued_{},activeOutput_{};
    UINT originalFirst_{},originalCount_{};
    Statistics statistics_{};
};
}
