#include "floor_reflection.h"
#include <d3dcompiler.h>
using Microsoft::WRL::ComPtr;
namespace grid2vr {
bool FloorReflection::Initialize(ID3D11DeviceContext* context) {
    if (!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    context->GetDevice(&device_);ComPtr<ID3D11Device1> device1;
    if (FAILED(device_.As(&device1)) || FAILED(context->QueryInterface(IID_PPV_ARGS(&context_)))) return false;
    const auto level=device_->GetFeatureLevel();
    if (FAILED(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&state_))) return false;
    // Modify an owned GPU copy only: FresnelMin and FresnelMax, nothing else.
    constexpr char code[]="RWByteAddressBuffer data:register(u0);[numthreads(1,1,1)] void main(){data.Store2(240,uint2(0,0));}";
    static_assert(fresnelOffset==240);
    ComPtr<ID3DBlob> bytes;
    if (FAILED(D3DCompile(code,sizeof(code)-1,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&bytes,nullptr)) ||
        FAILED(device_->CreateComputeShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&shader_))) return false;
    const uint32_t zero{};
    D3D11_TEXTURE2D_DESC desc{1,1,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_IMMUTABLE,D3D11_BIND_SHADER_RESOURCE};
    const D3D11_SUBRESOURCE_DATA data{&zero,4,0};ComPtr<ID3D11Texture2D> black;
    if (FAILED(device_->CreateTexture2D(&desc,&data,&black)) || FAILED(device_->CreateShaderResourceView(black.Get(),nullptr,&black_))) return false;
    thread_=GetCurrentThreadId();return true;
}
FloorReflection::Copy* FloorReflection::For(UINT bytes) {
    for (auto& copy:copies_) if (copy.bytes==bytes) return &copy;
    for (auto& copy:copies_) {
        if (copy.bytes) continue;
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=bytes;desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        Copy made{bytes};
        if (FAILED(device_->CreateBuffer(&desc,nullptr,&made.raw))) return nullptr;
        desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;desc.MiscFlags=0;
        if (FAILED(device_->CreateBuffer(&desc,nullptr,&made.filtered))) return nullptr;
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_TYPELESS;view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements=bytes/4;view.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(device_->CreateUnorderedAccessView(made.raw.Get(),&view,&made.uav))) return nullptr;
        copy=std::move(made);return &copy;
    }
    return nullptr; // Bounded: at most four distinct $Globals sizes.
}
bool FloorReflection::Apply(ID3D11DeviceContext* context,uint64_t hash,NativeDispatch dispatch) {
    const auto* shader=Find(hash);
    if (!shader || !dispatch || original_ || !context) return false;
    if (!device_ && !Initialize(context)) return false;
    if (!thread_ || thread_!=GetCurrentThreadId() || context_.Get()!=context) return false;
    ComPtr<ID3D11Buffer> source;context->PSGetConstantBuffers(0,1,&source);
    if (!source) return false;
    for (const auto& copy:copies_) if (copy.filtered==source) return false;
    D3D11_BUFFER_DESC desc{};source->GetDesc(&desc);
    if (desc.ByteWidth<shader->constantBytes || desc.ByteWidth%16 || desc.ByteWidth>D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT*16 ||
        desc.BindFlags!=D3D11_BIND_CONSTANT_BUFFER || desc.MiscFlags || desc.StructureByteStride) return false;
    auto* copy=For(desc.ByteWidth);
    if (!copy) return false;
    std::array<ComPtr<ID3D11ShaderResourceView>,2> views;
    {
        ID3D11ShaderResourceView* bound[2]{};context->PSGetShaderResources(shader->reflectionSlot,2,bound);
        views[0].Attach(bound[0]);views[1].Attach(bound[1]);
    }
    ComPtr<ID3DDeviceContextState> previous;context_->SwapDeviceContextState(state_.Get(),&previous);
    context_->CopyResource(copy->raw.Get(),source.Get());
    auto* uav=copy->uav.Get();context_->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    context_->CSSetShader(shader_.Get(),nullptr,0);dispatch(context_.Get(),1,1,1);
    uav=nullptr;context_->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    context_->CopyResource(copy->filtered.Get(),copy->raw.Get());
    context_->SwapDeviceContextState(previous.Get(),nullptr);
    if (FAILED(device_->GetDeviceRemovedReason())) return false;
    original_=source;originalViews_=views;active_=copy;slot_=shader->reflectionSlot;
    auto* constants=copy->filtered.Get();context->PSSetConstantBuffers(0,1,&constants);
    ID3D11ShaderResourceView* black[2]{black_.Get(),black_.Get()};context->PSSetShaderResources(slot_,2,black);
    ComPtr<ID3D11Buffer> actual;context->PSGetConstantBuffers(0,1,&actual);
    ID3D11ShaderResourceView* bound[2]{};context->PSGetShaderResources(slot_,2,bound);
    const bool applied=actual==copy->filtered && bound[0]==black_.Get() && bound[1]==black_.Get();
    for (auto* view:bound) if (view) view->Release();
    if (!applied) {Restore(context);return false;}
    return true;
}
bool FloorReflection::Restore(ID3D11DeviceContext* context) {
    if (!original_ || context_.Get()!=context || thread_!=GetCurrentThreadId()) return false;
    ComPtr<ID3D11Buffer> before;context->PSGetConstantBuffers(0,1,&before);
    ID3D11ShaderResourceView* current[2]{};context->PSGetShaderResources(slot_,2,current);
    bool restored=before==active_->filtered && current[0]==black_.Get() && current[1]==black_.Get();
    for (auto* view:current) if (view) view->Release();
    auto* constants=original_.Get();context->PSSetConstantBuffers(0,1,&constants);
    ID3D11ShaderResourceView* views[2]{originalViews_[0].Get(),originalViews_[1].Get()};context->PSSetShaderResources(slot_,2,views);
    ComPtr<ID3D11Buffer> after;context->PSGetConstantBuffers(0,1,&after);
    ID3D11ShaderResourceView* bound[2]{};context->PSGetShaderResources(slot_,2,bound);
    restored&=after==original_ && bound[0]==views[0] && bound[1]==views[1];
    for (auto* view:bound) if (view) view->Release();
    original_.Reset();originalViews_={};active_=nullptr;slot_=0;
    return restored;
}
}
