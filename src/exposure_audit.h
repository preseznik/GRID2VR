#pragma once
#include <d3d11.h>
#include <cstdint>
#include <filesystem>

namespace grid2vr::exposureaudit {
void Configure(const std::filesystem::path& output);
void AfterDraw(ID3D11DeviceContext* context, uint64_t frame);
}
