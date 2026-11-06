#pragma once
#include <d3d11.h>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace grid2vr::sponsordepth {
// GRID 2 hangs cloth sponsor banners (sponsor_flag, texture cloth.tga) on scaffolding
// 0.35 mm in front of a same-facing scaffolding surface. With the native 0.2 m near
// plane a 24-bit depth step exceeds that gap from about 35 m, so banner and scaffolding
// z-fight per pixel, and each VR eye resolves it differently (ProJict 6a887534).
// The opt-in fix biases only draws that sample a stock sponsor cloth texture toward
// the viewer; geometry, shaders, depth range and every other draw stay native.
constexpr UINT clothWidth=1024,clothHeight=256;
constexpr UINT mip0Pitch=(clothWidth/4)*8,mip0Rows=clothHeight/4;
constexpr size_t mip0Bytes=size_t(mip0Pitch)*mip0Rows;
constexpr UINT viewSlots=8;

inline bool Bc1(DXGI_FORMAT f) {
    return f==DXGI_FORMAT_BC1_TYPELESS || f==DXGI_FORMAT_BC1_UNORM || f==DXGI_FORMAT_BC1_UNORM_SRGB;
}
// Shape of every stock tracks/sponsors cloth.tga as GRID 2 uploads it.
inline bool Candidate(const D3D11_TEXTURE2D_DESC& d) {
    return d.Width==clothWidth && d.Height==clothHeight && d.MipLevels>=1 && d.ArraySize==1 && Bc1(d.Format) &&
        d.SampleDesc.Count==1 && (d.BindFlags&D3D11_BIND_SHADER_RESOURCE) && !d.CPUAccessFlags;
}
// Toward the viewer: a standard depth test (LESS/LESS_EQUAL) subtracts, a reversed one
// (GREATER/GREATER_EQUAL) adds. Disabled or other tests are never biased.
inline int BiasSign(const D3D11_DEPTH_STENCIL_DESC& d) {
    if(!d.DepthEnable)return 0;
    switch(d.DepthFunc) {
    case D3D11_COMPARISON_LESS: case D3D11_COMPARISON_LESS_EQUAL: return -1;
    case D3D11_COMPARISON_GREATER: case D3D11_COMPARISON_GREATER_EQUAL: return 1;
    default: return 0;
    }
}
inline D3D11_DEPTH_STENCIL_DESC DefaultDepthStencil() {
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable=TRUE;d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;d.DepthFunc=D3D11_COMPARISON_LESS;
    d.StencilReadMask=d.StencilWriteMask=D3D11_DEFAULT_STENCIL_READ_MASK;
    d.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};
    d.BackFace=d.FrontFace;return d;
}
inline D3D11_RASTERIZER_DESC DefaultRasterizer() {
    D3D11_RASTERIZER_DESC d{};
    d.FillMode=D3D11_FILL_SOLID;d.CullMode=D3D11_CULL_BACK;d.DepthClipEnable=TRUE;return d;
}
// Bias in depth-buffer units plus a slope term for banners seen at grazing angles.
struct Settings { INT units=16;float slope=2.f; };
inline D3D11_RASTERIZER_DESC Biased(D3D11_RASTERIZER_DESC d,int sign,const Settings& s) {
    d.DepthBias+=sign*s.units;d.SlopeScaledDepthBias+=static_cast<float>(sign)*s.slope;return d;
}
// Request text: empty for the defaults, or "units=N" / "slope=F" lines (units 1..256, slope 0..16).
inline bool ParseSettings(std::string_view text,Settings& out) {
    Settings s;
    while(!text.empty()) {
        const auto end=text.find('\n');
        auto line=text.substr(0,end);text=end==text.npos?std::string_view{}:text.substr(end+1);
        while(!line.empty() && (line.back()=='\r' || line.back()==' '))line.remove_suffix(1);
        if(line.empty())continue;
        const auto eq=line.find('=');if(eq==line.npos)return false;
        const auto key=line.substr(0,eq),value=line.substr(eq+1);
        if(key=="units") {
            int v{};const auto r=std::from_chars(value.data(),value.data()+value.size(),v);
            if(r.ec!=std::errc{} || r.ptr!=value.data()+value.size() || v<1 || v>256)return false;s.units=v;
        } else if(key=="slope") {
            float v{};const auto r=std::from_chars(value.data(),value.data()+value.size(),v);
            if(r.ec!=std::errc{} || r.ptr!=value.data()+value.size() || !std::isfinite(v) || v<0.f || v>16.f)return false;s.slope=v;
        } else return false;
    }
    out=s;return true;
}
}
