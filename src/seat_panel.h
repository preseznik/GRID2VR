#pragma once
#include "seat_menu.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <filesystem>
#include <memory>
#include <string>

namespace grid2vr::seat {
// Same keyboard seat-panel layout as the reference mods. GDI renders only when
// its text changes; D3D upload/draw belong to the owning render thread.
class PanelImage {
public:
    static constexpr unsigned Width=768,Height=1024;
    PanelImage();
    ~PanelImage();
    ID3D11Texture2D* Render(ID3D11Device* device,const std::wstring& name,const Menu& menu,bool sideways,const std::wstring& status={});
#ifdef GRID2VR_SEAT_PANEL_TEST
    bool SavePreview(const std::filesystem::path& path,const std::wstring& name,const Menu& menu,bool sideways);
#endif
private:
    struct Gdi;
    bool Paint(const std::wstring& name,const Menu& menu,bool sideways,const std::wstring& status={});
    std::unique_ptr<Gdi> gdi_;
    std::wstring signature_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    bool dirty_{true};
};
}
