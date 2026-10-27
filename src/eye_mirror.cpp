#include "eye_mirror.h"
#include "resolution_limits.h"
#include <cstdint>
#include <utility>
using Microsoft::WRL::ComPtr;
namespace grid2vr {
bool EyeMirror::Initialize(ID3D11Device* device) {
    if (!device || device_ || FAILED(device->GetDeviceRemovedReason())) return false;
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3DDeviceContextState> state;
    device->GetImmediateContext(&immediate);
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) || FAILED(immediate.As(&context))) return false;
    const auto level=device->GetFeatureLevel();
    if (FAILED(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&state))) return false;
    device_=device;context_=std::move(context);state_=std::move(state);thread_=GetCurrentThreadId();
    return true;
}
bool EyeMirror::Copy(ID3D11DeviceContext* context,ID3D11Texture2D* source,ID3D11Texture2D* destination) {
    if (!device_ || thread_!=GetCurrentThreadId() || !context || !source || !destination || source==destination ||
        context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    ComPtr<ID3D11Device> contextDevice,sourceDevice,destinationDevice;
    context->GetDevice(&contextDevice);source->GetDevice(&sourceDevice);destination->GetDevice(&destinationDevice);
    if (contextDevice!=device_ || sourceDevice!=device_ || destinationDevice!=device_ || FAILED(device_->GetDeviceRemovedReason())) return false;
    D3D11_TEXTURE2D_DESC input{},output{};source->GetDesc(&input);destination->GetDesc(&output);
    const auto supported=[](const D3D11_TEXTURE2D_DESC& desc) {
        return resolution::ValidScene(desc.Width,desc.Height) &&
            desc.MipLevels==1 && desc.ArraySize==1 && desc.SampleDesc.Count==1 && desc.SampleDesc.Quality==0 &&
            desc.Usage==D3D11_USAGE_DEFAULT && !desc.CPUAccessFlags &&
            (desc.Format==DXGI_FORMAT_R10G10B10A2_UNORM || desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM);
    };
    if (!supported(input) || !supported(output) || input.Width!=output.Width || input.Height!=output.Height || input.Format!=output.Format ||
        !(output.BindFlags&D3D11_BIND_RENDER_TARGET)) return false;
    // A private empty state unbinds both resources and disables predication.
    // Swapping back also preserves OM/CS UAV bindings and their hidden counters.
    ComPtr<ID3DDeviceContextState> previous;
    context_->SwapDeviceContextState(state_.Get(),&previous);
    context_->CopyResource(destination,source);
    context_->SwapDeviceContextState(previous.Get(),nullptr);
    return SUCCEEDED(device_->GetDeviceRemovedReason());
}
}
