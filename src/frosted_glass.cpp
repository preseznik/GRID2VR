#include "frosted_glass.h"
#include <d3dcompiler.h>
#include <string>
using Microsoft::WRL::ComPtr;
namespace grid2vr {
namespace {
constexpr std::array<UINT,2> offsets{152,168};
}
bool FrostedGlass::Initialize(ID3D11DeviceContext* context) {
    if (!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    context->GetDevice(&device_);ComPtr<ID3D11Device1> device1;
    if (!device_ || FAILED(device_.As(&device1)) || FAILED(context->QueryInterface(IID_PPV_ARGS(&context_)))) return false;
    const auto level=device_->GetFeatureLevel();
    if (FAILED(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&state_))) return false;
    // Modify an owned GPU copy only: FrostedGlassIntensity, nothing else.
    for (size_t i=0;i<offsets.size();++i) {
        const auto code="RWByteAddressBuffer data:register(u0);[numthreads(1,1,1)] void main(){data.Store("+
            std::to_string(offsets[i])+",0u);}";
        ComPtr<ID3DBlob> bytes;
        if (FAILED(D3DCompile(code.data(),code.size(),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&bytes,nullptr)) ||
            FAILED(device_->CreateComputeShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&zero_[i]))) return false;
    }
    thread_=GetCurrentThreadId();return true;
}
FrostedGlass::Copy* FrostedGlass::For(UINT bytes,UINT offset) {
    for (auto& copy:copies_) if (copy.bytes==bytes && copy.offset==offset) return &copy;
    for (auto& copy:copies_) {
        if (copy.bytes) continue;
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=bytes;desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        Copy made{bytes,offset};
        if (FAILED(device_->CreateBuffer(&desc,nullptr,&made.raw))) return nullptr;
        desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;desc.MiscFlags=0;
        if (FAILED(device_->CreateBuffer(&desc,nullptr,&made.filtered))) return nullptr;
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_TYPELESS;view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements=bytes/4;view.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(device_->CreateUnorderedAccessView(made.raw.Get(),&view,&made.uav))) return nullptr;
        copy=std::move(made);return &copy;
    }
    return nullptr; // Bounded: at most six distinct ($Globals size, offset) pairs.
}
bool FrostedGlass::Apply(ID3D11DeviceContext* context,uint64_t hash,NativeDispatch dispatch) {
    const auto* shader=Find(hash);
    if (!shader || !dispatch || original_ || !context) return false;
    if (!device_ && !Initialize(context)) return false;
    if (!thread_ || thread_!=GetCurrentThreadId() || context_.Get()!=context) return false;
    size_t variant=0;while (variant<offsets.size() && offsets[variant]!=shader->intensityOffset) ++variant;
    if (variant==offsets.size()) return false;
    ComPtr<ID3D11Buffer> source;context->PSGetConstantBuffers(0,1,&source);
    if (!source) return false;
    for (const auto& copy:copies_) if (copy.filtered==source) return false;
    D3D11_BUFFER_DESC desc{};source->GetDesc(&desc);
    if (desc.ByteWidth<shader->constantBytes || desc.ByteWidth%16 || desc.ByteWidth>D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT*16 ||
        desc.BindFlags!=D3D11_BIND_CONSTANT_BUFFER || desc.MiscFlags || desc.StructureByteStride) return false;
    auto* copy=For(desc.ByteWidth,shader->intensityOffset);
    if (!copy) return false;
    ComPtr<ID3DDeviceContextState> previous;context_->SwapDeviceContextState(state_.Get(),&previous);
    context_->CopyResource(copy->raw.Get(),source.Get());
    auto* uav=copy->uav.Get();context_->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    context_->CSSetShader(zero_[variant].Get(),nullptr,0);dispatch(context_.Get(),1,1,1);
    uav=nullptr;context_->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    context_->CopyResource(copy->filtered.Get(),copy->raw.Get());
    context_->SwapDeviceContextState(previous.Get(),nullptr);
    if (FAILED(device_->GetDeviceRemovedReason())) return false;
    original_=source;active_=copy;
    auto* constants=copy->filtered.Get();context->PSSetConstantBuffers(0,1,&constants);
    ComPtr<ID3D11Buffer> actual;context->PSGetConstantBuffers(0,1,&actual);
    if (actual!=copy->filtered) {Restore(context);return false;}
    return true;
}
bool FrostedGlass::Restore(ID3D11DeviceContext* context) {
    if (!original_ || context_.Get()!=context || thread_!=GetCurrentThreadId()) return false;
    ComPtr<ID3D11Buffer> before;context->PSGetConstantBuffers(0,1,&before);
    bool restored=before==active_->filtered;
    auto* constants=original_.Get();context->PSSetConstantBuffers(0,1,&constants);
    ComPtr<ID3D11Buffer> after;context->PSGetConstantBuffers(0,1,&after);
    restored&=after==original_;
    original_.Reset();active_=nullptr;
    return restored;
}
}
