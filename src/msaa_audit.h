#pragma once
#include <d3d11.h>
#include <cstdint>
#include <filesystem>

namespace grid2vr::msaaaudit {
// Read-only, bounded observation; absent request is a successful no-op.
bool Install(uintptr_t gameBase,const std::filesystem::path& folder) noexcept;
bool Enabled() noexcept;
void TextureCreated(const D3D11_TEXTURE2D_DESC* requested,HRESULT result,
    ID3D11Texture2D* texture,uintptr_t caller) noexcept;
}
