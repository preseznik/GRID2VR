#include <windows.h>
#include <d3d11.h>
#include <filesystem>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 1;
    const auto output = std::filesystem::temp_directory_path()/(L"grid2vr-proxy-test-"+std::to_wstring(GetCurrentProcessId()));
    if (std::filesystem::exists(output)) return 2;
    SetEnvironmentVariableW(L"GRID2VR_DIAGNOSTICS", wcscmp(argv[2],L"inactive")==0 ? L"0" : L"1");
    SetEnvironmentVariableW(L"GRID2VR_OUTPUT", output.c_str());
    const auto module = LoadLibraryW(argv[1]);
    if (!module) return 3;
    const auto create = reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    if (!create) return 4;
    ID3D11Device* device{}; ID3D11DeviceContext* context{};
    const auto hr = create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);
    if (context) context->Release();
    if (device) device->Release();
    const bool clean = !std::filesystem::exists(output);
    FreeLibrary(module);
    if (FAILED(hr) || !clean) return 5;
    puts("Device creation forwarded; no game hooks or diagnostics for inactive/unsupported host.");
    return 0;
}
