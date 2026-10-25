#pragma once
#include <d3d11.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>

namespace grid2vr::flickerwatch {
// Explicit diagnostic only: observe existing colour draws without changing bindings.
// At most 23 generic draws plus one flag per pass, three frames of queries and
// 180 accepted scenes (240 total scenes is a safety limit for rejected pairs).
bool Start(const std::filesystem::path& folder);
bool Active();
void Begin(uint64_t scene);
// Observe a CPU upload while mapped; never read GPU memory or retain its pointer.
void Upload(ID3D11Resource* resource,const void* data,UINT bytes);
void Draw(ID3D11DeviceContext* context,unsigned pass,unsigned slot,const char* kind,
          std::span<const UINT> args,uint64_t vs,uint64_t ps,std::array<uintptr_t,2> source);
void AfterDraw(ID3D11DeviceContext* context);
void Complete(ID3D11DeviceContext* context,uint64_t scene,bool accepted);
void Stop();
}
