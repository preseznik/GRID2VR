#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "camera_shader.h"
#include <cstdio>

int main() {
    using Microsoft::WRL::ComPtr;
    // Padding belongs to the declared layout but is not read by the shader.
    const char* source=R"(
        cbuffer CameraParamsConstantBuffer : register(b3) {
            float4x4 projection : packoffset(c0);
            float leftEye : packoffset(c19);
            float3 padding[3] : packoffset(c20);
        };
        float4 main(float4 position : POSITION) : SV_POSITION {
            return mul(projection,position)+leftEye;
        }
    )";
    ComPtr<ID3DBlob> code,error;
    auto hr=D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&code,&error);
    if (FAILED(hr)) { if (error) puts(static_cast<const char*>(error->GetBufferPointer())); return 1; }
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(&reflection)))) return 2;
    grid2vr::CameraBinding camera{};
    if (!grid2vr::FindCameraBinding(reflection.Get(),camera) || camera.slot!=3 || camera.declaredBytes!=368 || camera.usedBytes!=308) return 3;
    const char* unrelated="cbuffer Other : register(b3) { float4 values[23]; }; float4 main(float4 p : POSITION) : SV_POSITION { return p+values[22]; }";
    code.Reset(); reflection.Reset();
    if (FAILED(D3DCompile(unrelated,strlen(unrelated),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&code,&error))) return 4;
    if (FAILED(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(&reflection)))) return 5;
    if (grid2vr::FindCameraBinding(reflection.Get(),camera)) return 6;
    puts("Named camera binding and unused trailing fields verified; same-size unrelated buffer rejected.");
}
