#pragma once
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <d3d11.h>

namespace grid2vr::instancecapacity {
// Isolated recorded-view diagnostics; no effect without the request marker.
bool Install(uintptr_t base,const std::filesystem::path& folder);
enum class BindReadiness { Waiting, Ready, Refused };
// Read-only, quiet inspection before any scene hooks attach. Bind repeats the
// same strict qualification after installation to detect a changed owner.
BindReadiness ReadyToBind(uintptr_t main);
bool Bind(uintptr_t main);
bool Enabled();
void Disable();
void BufferCreated(const D3D11_BUFFER_DESC* requested,HRESULT result,ID3D11Buffer* buffer,uintptr_t caller);
void Demand(uint64_t frame,int lane,int count);
// Read-only: how the game maps its instance buffers in the native pass (0)
// and each replayed pass (1 rear, 2 right).
bool Mapped(ID3D11Resource* resource,D3D11_MAP kind,uint64_t frame,unsigned pass);
void Status(std::ostream& output);
}
