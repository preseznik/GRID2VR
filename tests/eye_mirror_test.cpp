#include "eye_mirror.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
using Microsoft::WRL::ComPtr;
#define CHECK(value) do { if (!(value)) { printf("Eye mirror check failed at line %d: %s\n",__LINE__,#value);return 1; } } while (false)

namespace {
constexpr UINT width=8,height=4;
using Pixels=std::array<uint32_t,width*height>;
D3D11_TEXTURE2D_DESC Description(DXGI_FORMAT format=DXGI_FORMAT_R10G10B10A2_UNORM) {
    D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;
    desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=format;
    desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    return desc;
}
bool PixelsMatch(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* texture,const Pixels& pixels) {
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&staging))) return false;
    ComPtr<ID3D11Predicate> predicate;BOOL condition{};context->GetPredication(&predicate,&condition);
    context->SetPredication(nullptr,FALSE);context->CopyResource(staging.Get(),texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto result=context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped);
    bool matches=SUCCEEDED(result);
    if (matches) {
        for (unsigned y=0;y<height;++y)
            matches&=!memcmp(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch,pixels.data()+y*width,width*sizeof(uint32_t));
        context->Unmap(staging.Get(),0);
    }
    context->SetPredication(predicate.Get(),condition);return matches;
}
bool CounterMatches(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11UnorderedAccessView* view,UINT expected) {
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=4;
    ComPtr<ID3D11Buffer> count,staging;
    if (FAILED(device->CreateBuffer(&desc,nullptr,&count))) return false;
    desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateBuffer(&desc,nullptr,&staging))) return false;
    ComPtr<ID3D11Predicate> predicate;BOOL condition{};context->GetPredication(&predicate,&condition);
    context->SetPredication(nullptr,FALSE);
    context->CopyStructureCount(count.Get(),0,view);context->CopyResource(staging.Get(),count.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto result=context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped);
    bool matches=SUCCEEDED(result);
    if (matches) { matches=*static_cast<const UINT*>(mapped.pData)==expected;context->Unmap(staging.Get(),0); }
    context->SetPredication(predicate.Get(),condition);return matches;
}
}
int main() {
    const auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    CHECK(module);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    CHECK(create && SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
    grid2vr::EyeMirror mirror;
    CHECK(!mirror.Initialize(nullptr) && !mirror.Copy(context.Get(),nullptr,nullptr));
    CHECK(mirror.Initialize(device.Get()) && !mirror.Initialize(device.Get()));
    Pixels pixels{};
    for (unsigned i=0;i<pixels.size();++i) pixels[i]=0x41239877u*(i+1); // Includes alpha and non-primary display-encoded colours.
    auto desc=Description();D3D11_SUBRESOURCE_DATA data{pixels.data(),width*sizeof(uint32_t),0};
    ComPtr<ID3D11Texture2D> source,destination;ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11ShaderResourceView> srv;
    CHECK(SUCCEEDED(device->CreateTexture2D(&desc,&data,&source)) && SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&destination)));
    CHECK(SUCCEEDED(device->CreateRenderTargetView(destination.Get(),nullptr,&rtv)) && SUCCEEDED(device->CreateShaderResourceView(source.Get(),nullptr,&srv)));
    auto depthDesc=Description(DXGI_FORMAT_D24_UNORM_S8_UINT);depthDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;ComPtr<ID3D11DepthStencilView> dsv;
    CHECK(SUCCEEDED(device->CreateTexture2D(&depthDesc,nullptr,&depth)) && SUCCEEDED(device->CreateDepthStencilView(depth.Get(),nullptr,&dsv)));
    D3D11_BUFFER_DESC bufferDesc{};bufferDesc.ByteWidth=64;bufferDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constant,geometry;
    CHECK(SUCCEEDED(device->CreateBuffer(&bufferDesc,nullptr,&constant)));
    bufferDesc.BindFlags=D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER;
    CHECK(SUCCEEDED(device->CreateBuffer(&bufferDesc,nullptr,&geometry)));
    bufferDesc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;bufferDesc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bufferDesc.StructureByteStride=4;
    std::array<ComPtr<ID3D11Buffer>,2> unorderedBuffers;
    std::array<ComPtr<ID3D11UnorderedAccessView>,2> uavs;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};uavDesc.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements=16;uavDesc.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_COUNTER;
    for (unsigned i=0;i<2;++i)
        CHECK(SUCCEEDED(device->CreateBuffer(&bufferDesc,nullptr,&unorderedBuffers[i])) &&
            SUCCEEDED(device->CreateUnorderedAccessView(unorderedBuffers[i].Get(),&uavDesc,&uavs[i])));
    D3D11_SAMPLER_DESC samplerDesc{};samplerDesc.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU=samplerDesc.AddressV=samplerDesc.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;samplerDesc.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;CHECK(SUCCEEDED(device->CreateSamplerState(&samplerDesc,&sampler)));
    D3D11_RASTERIZER_DESC rasterDesc{};rasterDesc.FillMode=D3D11_FILL_WIREFRAME;rasterDesc.CullMode=D3D11_CULL_FRONT;rasterDesc.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;CHECK(SUCCEEDED(device->CreateRasterizerState(&rasterDesc,&raster)));
    D3D11_BLEND_DESC blendDesc{};blendDesc.AlphaToCoverageEnable=TRUE;blendDesc.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blend;CHECK(SUCCEEDED(device->CreateBlendState(&blendDesc,&blend)));
    D3D11_DEPTH_STENCIL_DESC depthStateDesc{};depthStateDesc.DepthEnable=TRUE;depthStateDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;depthStateDesc.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
    ComPtr<ID3D11DepthStencilState> depthState;CHECK(SUCCEEDED(device->CreateDepthStencilState(&depthStateDesc,&depthState)));
    D3D11_QUERY_DESC queryDesc{D3D11_QUERY_OCCLUSION_PREDICATE,0};ComPtr<ID3D11Predicate> predicate;
    CHECK(SUCCEEDED(device->CreatePredicate(&queryDesc,&predicate)));
    context->Begin(predicate.Get());context->End(predicate.Get());context->Flush();
    BOOL queryValue{};HRESULT queryStatus=S_FALSE;
    for (unsigned i=0;i<100 && queryStatus==S_FALSE;++i) {
        queryStatus=context->GetData(predicate.Get(),&queryValue,sizeof(queryValue),0);
        if (queryStatus==S_FALSE) Sleep(1);
    }
    CHECK(queryStatus==S_OK && !queryValue);
    auto* color=rtv.Get();auto* output=uavs[0].Get();UINT counter=2;
    context->OMSetRenderTargetsAndUnorderedAccessViews(1,&color,dsv.Get(),1,1,&output,&counter);
    output=uavs[1].Get();counter=3;context->CSSetUnorderedAccessViews(4,1,&output,&counter);
    auto* cb=constant.Get();
    context->VSSetConstantBuffers(3,1,&cb);context->HSSetConstantBuffers(3,1,&cb);context->DSSetConstantBuffers(3,1,&cb);
    context->GSSetConstantBuffers(3,1,&cb);context->PSSetConstantBuffers(3,1,&cb);context->CSSetConstantBuffers(3,1,&cb);
    auto* input=srv.Get();context->PSSetShaderResources(7,1,&input);context->CSSetShaderResources(7,1,&input);
    auto* sample=sampler.Get();context->PSSetSamplers(5,1,&sample);context->CSSetSamplers(5,1,&sample);
    auto* vertex=geometry.Get();UINT stride=16,offset=4;context->IASetVertexBuffers(2,1,&vertex,&stride,&offset);
    context->IASetIndexBuffer(geometry.Get(),DXGI_FORMAT_R16_UINT,2);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    const D3D11_VIEWPORT viewport{1,2,4,2,.2f,.8f};const D3D11_RECT scissor{1,2,5,4};
    context->RSSetState(raster.Get());context->RSSetViewports(1,&viewport);context->RSSetScissorRects(1,&scissor);
    const float blendFactor[]{.1f,.2f,.3f,.4f};context->OMSetBlendState(blend.Get(),blendFactor,0x12345678);
    context->OMSetDepthStencilState(depthState.Get(),31);context->SetPredication(predicate.Get(),TRUE);
    auto preserved=[&] {
        ComPtr<ID3D11RenderTargetView> actualRtv;ComPtr<ID3D11DepthStencilView> actualDsv;ComPtr<ID3D11UnorderedAccessView> actualUav;
        context->OMGetRenderTargetsAndUnorderedAccessViews(1,&actualRtv,&actualDsv,1,1,&actualUav);
        if (actualRtv!=rtv || actualDsv!=dsv || actualUav!=uavs[0]) return false;
        actualUav.Reset();context->CSGetUnorderedAccessViews(4,1,&actualUav);if (actualUav!=uavs[1]) return false;
        ComPtr<ID3D11Buffer> actualCb;
        context->VSGetConstantBuffers(3,1,&actualCb);if (actualCb!=constant) return false;
        actualCb.Reset();context->HSGetConstantBuffers(3,1,&actualCb);if (actualCb!=constant) return false;
        actualCb.Reset();context->DSGetConstantBuffers(3,1,&actualCb);if (actualCb!=constant) return false;
        actualCb.Reset();context->GSGetConstantBuffers(3,1,&actualCb);if (actualCb!=constant) return false;
        actualCb.Reset();context->PSGetConstantBuffers(3,1,&actualCb);if (actualCb!=constant) return false;
        actualCb.Reset();context->CSGetConstantBuffers(3,1,&actualCb);if (actualCb!=constant) return false;
        ComPtr<ID3D11ShaderResourceView> actualSrv;context->PSGetShaderResources(7,1,&actualSrv);if (actualSrv!=srv) return false;
        actualSrv.Reset();context->CSGetShaderResources(7,1,&actualSrv);if (actualSrv!=srv) return false;
        ComPtr<ID3D11SamplerState> actualSampler;context->PSGetSamplers(5,1,&actualSampler);if (actualSampler!=sampler) return false;
        actualSampler.Reset();context->CSGetSamplers(5,1,&actualSampler);if (actualSampler!=sampler) return false;
        ComPtr<ID3D11Buffer> actualGeometry;UINT actualStride{},actualOffset{};
        context->IAGetVertexBuffers(2,1,&actualGeometry,&actualStride,&actualOffset);
        if (actualGeometry!=geometry || actualStride!=stride || actualOffset!=offset) return false;
        actualGeometry.Reset();DXGI_FORMAT indexFormat{};context->IAGetIndexBuffer(&actualGeometry,&indexFormat,&actualOffset);
        if (actualGeometry!=geometry || indexFormat!=DXGI_FORMAT_R16_UINT || actualOffset!=2) return false;
        D3D11_PRIMITIVE_TOPOLOGY topology{};context->IAGetPrimitiveTopology(&topology);if (topology!=D3D11_PRIMITIVE_TOPOLOGY_LINELIST) return false;
        ComPtr<ID3D11RasterizerState> actualRaster;context->RSGetState(&actualRaster);if (actualRaster!=raster) return false;
        D3D11_VIEWPORT actualViewport{};UINT count=1;context->RSGetViewports(&count,&actualViewport);
        if (count!=1 || memcmp(&actualViewport,&viewport,sizeof(viewport))) return false;
        D3D11_RECT actualScissor{};count=1;context->RSGetScissorRects(&count,&actualScissor);
        if (count!=1 || memcmp(&actualScissor,&scissor,sizeof(scissor))) return false;
        ComPtr<ID3D11BlendState> actualBlend;float actualFactor[4]{};UINT mask{};
        context->OMGetBlendState(&actualBlend,actualFactor,&mask);
        if (actualBlend!=blend || memcmp(actualFactor,blendFactor,sizeof(blendFactor)) || mask!=0x12345678) return false;
        ComPtr<ID3D11DepthStencilState> actualDepthState;UINT stencil{};context->OMGetDepthStencilState(&actualDepthState,&stencil);
        if (actualDepthState!=depthState || stencil!=31) return false;
        ComPtr<ID3D11Predicate> actualPredicate;BOOL condition{};context->GetPredication(&actualPredicate,&condition);
        return actualPredicate==predicate && condition==TRUE;
    };
    CHECK(preserved());
    for (unsigned repeat=0;repeat<3;++repeat) {
        CHECK(mirror.Copy(context.Get(),source.Get(),destination.Get()) && preserved());
        CHECK(PixelsMatch(device.Get(),context.Get(),destination.Get(),pixels));
        CHECK(CounterMatches(device.Get(),context.Get(),uavs[0].Get(),2) && CounterMatches(device.Get(),context.Get(),uavs[1].Get(),3) && preserved());
    }
    CHECK(!mirror.Copy(nullptr,source.Get(),destination.Get()) && !mirror.Copy(context.Get(),nullptr,destination.Get()) &&
        !mirror.Copy(context.Get(),source.Get(),nullptr) && !mirror.Copy(context.Get(),source.Get(),source.Get()) && preserved());
    ComPtr<ID3D11DeviceContext> deferred;CHECK(SUCCEEDED(device->CreateDeferredContext(0,&deferred)));
    CHECK(!mirror.Copy(deferred.Get(),source.Get(),destination.Get()) && preserved());
    bool foreignThread=true;std::thread other([&] {foreignThread=mirror.Copy(context.Get(),source.Get(),destination.Get());});other.join();
    CHECK(!foreignThread && preserved());
    ComPtr<ID3D11Device> foreignDevice;ComPtr<ID3D11DeviceContext> foreignContext;ComPtr<ID3D11Texture2D> foreignTexture;
    CHECK(SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&foreignDevice,nullptr,&foreignContext)) &&
        SUCCEEDED(foreignDevice->CreateTexture2D(&desc,nullptr,&foreignTexture)));
    CHECK(!mirror.Copy(foreignContext.Get(),source.Get(),destination.Get()) && !mirror.Copy(context.Get(),foreignTexture.Get(),destination.Get()) &&
        !mirror.Copy(context.Get(),source.Get(),foreignTexture.Get()) && preserved());
    for (unsigned refusal=0;refusal<8;++refusal) {
        auto invalid=Description();
        if (refusal==0) invalid.Width*=2;
        if (refusal==1) invalid.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        if (refusal==2) invalid.MipLevels=2;
        if (refusal==3) invalid.ArraySize=2;
        if (refusal==4) invalid.SampleDesc.Count=4;
        if (refusal==5) { invalid.Usage=D3D11_USAGE_STAGING;invalid.CPUAccessFlags=D3D11_CPU_ACCESS_READ;invalid.BindFlags=0; }
        if (refusal==6) invalid.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        if (refusal==7) invalid.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        ComPtr<ID3D11Texture2D> bad;CHECK(SUCCEEDED(device->CreateTexture2D(&invalid,nullptr,&bad)));
        CHECK(!mirror.Copy(context.Get(),source.Get(),bad.Get()) && preserved());
        if (refusal!=6) CHECK(!mirror.Copy(context.Get(),bad.Get(),destination.Get()) && preserved());
        CHECK(PixelsMatch(device.Get(),context.Get(),destination.Get(),pixels) && preserved());
    }
    auto unsupported=Description(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    ComPtr<ID3D11Texture2D> srgbSource,srgbDestination,immutable;
    CHECK(SUCCEEDED(device->CreateTexture2D(&unsupported,nullptr,&srgbSource)) &&
        SUCCEEDED(device->CreateTexture2D(&unsupported,nullptr,&srgbDestination)));
    CHECK(!mirror.Copy(context.Get(),srgbSource.Get(),srgbDestination.Get()) && preserved());
    unsupported=Description();unsupported.Usage=D3D11_USAGE_IMMUTABLE;unsupported.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    CHECK(SUCCEEDED(device->CreateTexture2D(&unsupported,&data,&immutable)));
    CHECK(!mirror.Copy(context.Get(),immutable.Get(),destination.Get()) && !mirror.Copy(context.Get(),source.Get(),immutable.Get()) && preserved());
    // Recreated swapchain-sized targets must receive the latest pixels, with no retained image cache.
    context->ClearState();
    for (const auto format : {DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM}) {
        desc=Description(format);
        for (unsigned repeat=0;repeat<4;++repeat) {
            for (auto& pixel:pixels) pixel^=0x13579bdfu+repeat;
            ComPtr<ID3D11Texture2D> nextSource,nextDestination;
            CHECK(SUCCEEDED(device->CreateTexture2D(&desc,&data,&nextSource)) && SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&nextDestination)));
            CHECK(mirror.Copy(context.Get(),nextSource.Get(),nextDestination.Get()) && PixelsMatch(device.Get(),context.Get(),nextDestination.Get(),pixels));
        }
    }
    CHECK(SUCCEEDED(device->GetDeviceRemovedReason()));
    puts("Eye mirror: exact RGB10/RGBA/BGRA pixels, recreated targets, predication isolation, OM/CS UAV counters and pipeline restoration, invalid texture/context/thread refusal passed.");
}
