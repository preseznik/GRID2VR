#pragma once
#include <d3d11.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <array>
#include <set>
#include <string>
#include <vector>

namespace grid2vr {
// Exact stock-atlas identities; optional replacements never change game files.
class FocusCockpit {
public:
    using TextureFn=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const D3D11_TEXTURE2D_DESC*,const D3D11_SUBRESOURCE_DATA*,ID3D11Texture2D**);
    using ViewFn=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,ID3D11Resource*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,ID3D11ShaderResourceView**);
    void Configure(const std::filesystem::path& game,const std::filesystem::path& run) noexcept;
    bool Enabled() const noexcept { return mode!=Mode::Off; }
    void HookFailed() noexcept;
    void TextureCreated(ID3D11Device*,const D3D11_TEXTURE2D_DESC*,const D3D11_SUBRESOURCE_DATA*,ID3D11Texture2D*,TextureFn,uintptr_t callerRva) noexcept;
    HRESULT CreateView(ID3D11Device*,ID3D11Resource*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,ID3D11ShaderResourceView**,ViewFn) noexcept;
    void Summary() noexcept;
#ifdef GRID2VR_FOCUS_COCKPIT_TEST
    void ConfigureTest(const std::filesystem::path& run,const std::filesystem::path& dds,const std::string& sourceHash,const std::string& sourceHash8,const std::string& packHash,bool replace);
    void AddTestAtlas(const std::filesystem::path& dds,const std::string& sourceHash,const std::string& sourceHash8,const std::string& packHash);
#endif
private:
    enum class Mode { Off,Observe,Replace };
    std::vector<unsigned char> Load(const std::filesystem::path&,const std::string& expectedHash);
    void Event(const std::string&) noexcept;
    std::atomic<Mode> mode{Mode::Off};
    std::filesystem::path folder;
    struct Atlas { std::string car,sourceHash,sourceHash8,packHash;std::filesystem::path path; };
    std::vector<Atlas> atlases;
    std::mutex packMutex;
    std::set<std::string> refusedPacks;
    std::mutex logMutex;
    std::mutex shapeMutex;
    std::set<std::array<UINT,10>> shapes;
    std::atomic<unsigned> textureCalls{},viewCalls{},atlasObservations{};
    std::atomic<unsigned> events{},candidates{},noInitialData{},matches{},replacements{},fallbacks{};
};
}
