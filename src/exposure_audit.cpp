#include "exposure_audit.h"
#include "engine_trace.h"
#include "replay_trace.h"
#include <DirectXPackedVector.h>
#include <wrl/client.h>
#include <array>
#include <fstream>

namespace grid2vr::exposureaudit {
namespace {
using Microsoft::WRL::ComPtr;
bool enabled{};
std::filesystem::path folder;
uint64_t lastFrame[3]{};
unsigned rows{};
bool ReadConstants(ID3D11DeviceContext* context, unsigned slot, float* values, unsigned bytes) {
    ComPtr<ID3D11Buffer> source, staging;
    context->PSGetConstantBuffers(slot,1,&source);
    if (!source) return false;
    D3D11_BUFFER_DESC desc{};source->GetDesc(&desc);
    if (desc.ByteWidth<bytes || desc.ByteWidth>4096) return false;
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.MiscFlags=0;
    desc.StructureByteStride=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Device> device;context->GetDevice(&device);
    if (FAILED(device->CreateBuffer(&desc,nullptr,&staging))) return false;
    context->CopyResource(staging.Get(),source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return false;
    memcpy(values,mapped.pData,bytes);context->Unmap(staging.Get(),0);return true;
}
}
void Configure(const std::filesystem::path& output) {
    folder=output;enabled=std::filesystem::is_regular_file(output/L"exposure-audit.request");
}
void AfterDraw(ID3D11DeviceContext* context,uint64_t frame) {
    if (!enabled || rows>=600 || !context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    const auto scene=engine::CurrentScene();const auto pass=scene.pass;
    if (pass>2 || frame<lastFrame[pass]+60) return;
    ComPtr<ID3D11PixelShader> shader;context->PSGetShader(&shader,nullptr,nullptr);
    // Shipped UniFinal.psh.fp_lum, not a reflection/name-based substitution.
    if (replaytrace::ShaderFingerprint(shader.Get())!=0xccaee125ba648054ull) return;
    ComPtr<ID3D11RenderTargetView> target;context->OMGetRenderTargets(1,&target,nullptr);
    ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> source,staging;
    if (!target) return;target->GetResource(&resource);
    if (FAILED(resource.As(&source))) return;
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    if (desc.Width!=1 || desc.Height!=1 || desc.MipLevels!=1 || desc.ArraySize!=1 ||
        desc.SampleDesc.Count!=1 || desc.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT) return;
    std::array<float,324> globals{};std::array<float,4> encoding{};
    if (!ReadConstants(context,0,globals.data(),1296) || !ReadConstants(context,1,encoding.data(),16)) return;
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Device> device;context->GetDevice(&device);
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&staging))) return;
    context->CopyResource(staging.Get(),source.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return;
    std::array<float,4> lum{};
    for (unsigned i=0;i<4;++i) lum[i]=DirectX::PackedVector::XMConvertHalfToFloat(static_cast<const uint16_t*>(mapped.pData)[i]);
    context->Unmap(staging.Get(),0);
    std::ofstream file(folder/L"exposure-audit.csv",std::ios::app);
    unsigned cbBytes[2]{};
    for(unsigned slot=0;slot<2;++slot){ComPtr<ID3D11Buffer> b;context->PSGetConstantBuffers(slot,1,&b);D3D11_BUFFER_DESC bd{};if(b)b->GetDesc(&bd);cbBytes[slot]=bd.ByteWidth;}
    if (!rows) file<<"frame,scene,pass,cockpit,cb0_bytes,cb1_bytes,encoding_x,encoding_y,encoding_z,encoding_w,min,max,key,strength,damping,lum_x,lum_y,lum_z,lum_w\n";
    file<<frame<<','<<scene.id<<','<<pass<<','<<engine::CurrentCockpitCamera()<<','<<cbBytes[0]<<','<<cbBytes[1];
    for(auto value:encoding)file<<','<<value;
    for(auto offset:{772u,776u,780u,1012u,1016u})file<<','<<globals[offset/4];
    for(auto value:lum)file<<','<<value;
    file<<'\n';++rows;lastFrame[pass]=frame;
}
}
