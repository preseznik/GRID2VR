#pragma once
#include <d3d11.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>

namespace grid2vr::flicker {
// Optional, single eight-scene burst. No rendering decisions depend on this.
void Configure(const std::filesystem::path& folder);
void Poll();
void Begin(uint64_t scene);
bool Capturing(uint64_t scene=0);
bool Observing(uint64_t scene=0);
void Upload(ID3D11Resource* resource,const void* data,UINT bytes);
void RememberLayout(ID3D11InputLayout* layout,const D3D11_INPUT_ELEMENT_DESC* elements,UINT count);
void Draw(ID3D11DeviceContext* context,unsigned pass,unsigned slot,const char* kind,
          std::span<const UINT> args,uint64_t vs,uint64_t ps,std::array<uintptr_t,2> source,
          std::array<uint32_t,12> nativeList={});
void AfterDraw(ID3D11DeviceContext* context);
void FinishPass(ID3D11DeviceContext* context,uint64_t scene,unsigned pass,ID3D11Texture2D* finalSource);
void DepthClear(ID3D11DepthStencilView* view,unsigned pass,UINT flags,FLOAT depth,UINT8 stencil);
void NativeQuery(ID3D11Asynchronous* query,unsigned pass,const char* operation,uintptr_t caller,
                 HRESULT result=S_FALSE,const void* data=nullptr,UINT bytes=0);
void Complete(ID3D11DeviceContext* context,uint64_t frame,uint64_t scene,bool accepted,
              const std::array<ID3D11Texture2D*,3>& images);
}
