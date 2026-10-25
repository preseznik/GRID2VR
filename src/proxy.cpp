#include <windows.h>
#include <intrin.h>
#include <filesystem>
#include "diagnostics.h"

static FARPROC RealProc(const char* name) {
    static HMODULE system = [] {
        wchar_t path[MAX_PATH]{};
        GetSystemDirectoryW(path, MAX_PATH);
        return LoadLibraryExW((std::filesystem::path(path)/L"d3d11.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }();
    const auto proc = system ? GetProcAddress(system, name) : nullptr;
    if (!proc) __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    return proc;
}

extern "C" HRESULT WINAPI ProxyCreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type,
    HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT count, UINT sdk,
    ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context) {
    const auto original = reinterpret_cast<decltype(&D3D11CreateDevice)>(RealProc("D3D11CreateDevice"));
    const auto hr = original(adapter, type, software, flags, levels, count, sdk, device, level, context);
    if (SUCCEEDED(hr) && device && *device && grid2vr::Active())
        grid2vr::Attach(*device, context ? *context : nullptr, nullptr);
    return hr;
}

extern "C" HRESULT WINAPI ProxyCreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type,
    HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT count, UINT sdk,
    const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapchain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context) {
    const auto original = reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(RealProc("D3D11CreateDeviceAndSwapChain"));
    const auto hr = original(adapter, type, software, flags, levels, count, sdk, desc, swapchain, device, level, context);
    if (SUCCEEDED(hr) && device && *device && grid2vr::Active())
        grid2vr::Attach(*device, context ? *context : nullptr, swapchain ? *swapchain : nullptr);
    return hr;
}

#include "forwarders.inc"

