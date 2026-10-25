#pragma once
#include <d3d11.h>
#include <dxgi.h>

namespace grid2vr {
bool Active();
void Attach(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapchain);
}

