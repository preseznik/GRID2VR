#pragma once
#include <d3d11_1.h>
#include "eye_blit.h"
#include <wrl/client.h>

namespace grid2vr {
// Adapts DiRT2VR EyeBlit's isolated D3D11.1 context-state approach. The source is
// a completed gamma-encoded game backbuffer. Draw never retains that backbuffer.
class SeatPresenter {
public:
    enum class Mode { Opaque, StraightAlpha, Composite };
    using Logger=void (*)(const char*);
    explicit SeatPresenter(Logger logger=nullptr) noexcept : logger_(logger) {}
    SeatPresenter(const SeatPresenter&)=delete;
    SeatPresenter& operator=(const SeatPresenter&)=delete;
    bool Initialize(ID3D11Device* device,NativeEyeDraw draw) noexcept;
    bool Draw(ID3D11Texture2D* source,ID3D11RenderTargetView* target,unsigned width,unsigned height,Mode mode=Mode::Opaque) noexcept;
    // Into a rectangle of a larger target, e.g. a panel over the game image on the monitor.
    bool DrawRegion(ID3D11Texture2D* source,ID3D11RenderTargetView* target,unsigned left,unsigned top,unsigned width,unsigned height,Mode mode) noexcept;
    void Reset() noexcept;
private:
    bool DrawInto(ID3D11Texture2D* source,ID3D11RenderTargetView* target,unsigned left,unsigned top,unsigned width,unsigned height,Mode mode,bool exact) noexcept;
    bool PrepareSource(ID3D11Texture2D* source,const D3D11_TEXTURE2D_DESC& desc);
    void Report(const char*) noexcept;
    Logger logger_{};
    NativeEyeDraw draw_{};
    DWORD thread_{};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> alphaPixel_,compositePixel_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> compositeBlend_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sourceCopy_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sourceView_;
    unsigned sourceWidth_{},sourceHeight_{};
    DXGI_FORMAT sourceFormat_=DXGI_FORMAT_UNKNOWN;
};
}
