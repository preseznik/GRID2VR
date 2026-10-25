#pragma once
#include <d3d11shader.h>
#include <cstring>

namespace grid2vr {
struct CameraBinding {
    UINT slot{}, declaredBytes{}, usedBytes{};
};

// Match the reflected name and binding, never the allocation size alone.
inline bool FindCameraBinding(ID3D11ShaderReflection* reflection, CameraBinding& result) {
    constexpr auto name="CameraParamsConstantBuffer";
    D3D11_SHADER_INPUT_BIND_DESC binding{};
    if (FAILED(reflection->GetResourceBindingDescByName(name,&binding)) || binding.Type!=D3D_SIT_CBUFFER)
        return false;
    auto* buffer=reflection->GetConstantBufferByName(name);
    D3D11_SHADER_BUFFER_DESC desc{};
    if (FAILED(buffer->GetDesc(&desc))) return false;
    result={binding.BindPoint,desc.Size,0};
    for (UINT i=0;i<desc.Variables;++i) {
        D3D11_SHADER_VARIABLE_DESC variable{};
        if (SUCCEEDED(buffer->GetVariableByIndex(i)->GetDesc(&variable)) && (variable.uFlags&D3D_SVF_USED)) {
            const auto end=variable.StartOffset+variable.Size;
            if (end>result.usedBytes) result.usedBytes=end;
        }
    }
    return result.slot<D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
}
}
