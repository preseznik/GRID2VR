#pragma once
#include "eye_blit.h"
#include <dxgi.h>
#include <filesystem>

namespace grid2vr::seat {
void Prepare(uintptr_t base,const std::filesystem::path& output,void (*log)(const char*,...)) noexcept;
void Present(IDXGISwapChain* chain,NativeEyeDraw draw) noexcept;
void Stop() noexcept;
bool Requested();
bool Opened();
uint64_t Opening();
ID3D11Texture2D* Texture();
void XrPanelReady(bool ready);
}
