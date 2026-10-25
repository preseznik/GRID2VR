#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace grid2vr::sceneresolution {
struct Size {
    unsigned width{},height{};
    bool operator==(const Size&) const = default;
};
// GRID 2 AVX 1.0.85.8679: RVA 87e490 builds its factory descriptor at
// renderer+a4 from +30/+34. The windowed dimension copies are +114/+118.
// These offsets are independently derived from GRID, not the DiRT 2 layout.
struct Renderer {
    uint8_t prefix[0xc];
    uint8_t fullscreen;
    uint8_t beforeSize[0x30-0xd];
    unsigned width,height;
    uint8_t beforeWindow[0x74-0x38];
    HWND window;
    uint8_t beforeDevice[0x98-0x78];
    IUnknown* device;
    IDXGISwapChain* chain;
    ID3D11DeviceContext* context;
    DXGI_SWAP_CHAIN_DESC swap;
    uint8_t beforeWindowSize[0x114-0xe0];
    unsigned windowWidth,windowHeight;
};
static_assert(offsetof(Renderer,width)==0x30);
static_assert(offsetof(Renderer,window)==0x74);
static_assert(offsetof(Renderer,swap)==0xa4);
static_assert(offsetof(Renderer,windowWidth)==0x114);
static_assert(sizeof(Renderer)==0x11c);

bool ValidSize(Size size);
bool ParseSize(std::string_view text,Size& size);
bool SetFields(Renderer& renderer,Size size);
bool ApplyNative(uintptr_t gameBase,uintptr_t caller,DXGI_SWAP_CHAIN_DESC* desc,Size size);

// Call only after Active() validates the executable SHA. The explicit request
// is scene-size.request ("width height"); absence leaves all game fields alone.
void Apply(uintptr_t gameBase,const std::filesystem::path& output,DXGI_SWAP_CHAIN_DESC* desc,uintptr_t caller) noexcept;
void ObserveSwapchain(IDXGISwapChain* chain) noexcept;
// The slot-100 final colour target establishes the actual per-eye scene size.
void ObserveScene(unsigned width,unsigned height) noexcept;
}
