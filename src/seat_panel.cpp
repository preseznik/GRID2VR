#include "seat_panel.h"
#include <algorithm>
#include <objidl.h>
namespace Gdiplus {using std::min;using std::max;}
#include <gdiplus.h>
#include <cwchar>
#include <vector>

namespace grid2vr::seat {
struct PanelImage::Gdi {
    ULONG_PTR token{};
    std::unique_ptr<Gdiplus::Bitmap> bitmap;
    ~Gdi(){bitmap.reset();if(token)Gdiplus::GdiplusShutdown(token);}
};
PanelImage::PanelImage()=default;
PanelImage::~PanelImage()=default;
bool PanelImage::Paint(const std::wstring& name,const Menu& menu,bool sideways,const std::wstring& statusText) {
    if(!gdi_) {
        auto next=std::make_unique<Gdi>();Gdiplus::GdiplusStartupInput startup;
        if(Gdiplus::GdiplusStartup(&next->token,&startup,nullptr)!=Gdiplus::Ok)return false;
        next->bitmap=std::make_unique<Gdiplus::Bitmap>(Width,Height,PixelFormat32bppPARGB);gdi_=std::move(next);
    }
    const auto seat=menu.Current();wchar_t values[256]{};
    if(menu.PositionsEnabled())swprintf_s(values,L"Height      %+d cm\nDepth       %+d cm\nSideways   %+d cm",seat.height,seat.forward,seat.right);
    else swprintf_s(values,L"Height      Off\nDepth       Off\nSideways   Off");
    std::wstring valueText=values;
    if(menu.Desktop())valueText+=L"\nView angle "+std::wstring(menu.Fov()>=0?L"+":L"")+std::to_wstring(menu.Fov())+L"\u00b0";
    const std::wstring hint=L"Shift: sideways   Enter: save   Esc: cancel"+std::wstring(menu.Desktop()?L"\nPgUp/PgDn: view angle":L"");
    const std::wstring status=!statusText.empty()?statusText:menu.PositionsEnabled()?L"Arrows: move the seat":L"Desktop seat positions are off";
    const auto signature=name+L'\n'+valueText+L'\n'+hint+L'\n'+status+(sideways?L"\nside":L"\ndepth");
    if(signature==signature_)return true;
    auto& bitmap=*gdi_->bitmap;Gdiplus::Graphics g(&bitmap);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.Clear(Gdiplus::Color(238,25,27,29));
    const Gdiplus::Color redAccent(255,255,119,115); // GRID2VR dark-theme accent (#ff7773).
    Gdiplus::SolidBrush white(Gdiplus::Color(255,245,245,245)),accent(redAccent);
    Gdiplus::Font title(L"Segoe UI",43,Gdiplus::FontStyleBold,Gdiplus::UnitPixel),normal(L"Segoe UI",29,Gdiplus::FontStyleRegular,Gdiplus::UnitPixel),bold(L"Segoe UI",32,Gdiplus::FontStyleBold,Gdiplus::UnitPixel);
    const auto label=[&](const wchar_t* text,float x,float y,float width,float height,Gdiplus::Font& font,Gdiplus::Brush& brush){
        g.DrawString(text,-1,&font,Gdiplus::RectF(x,y,width,height),nullptr,&brush);
    };
    label(L"SEAT POSITION",40,30,690,65,title,white);label(name.c_str(),40,98,690,80,normal,white);
    label(L"Tab: save & close",40,174,690,45,normal,accent);
    Gdiplus::Pen seatPen(Gdiplus::Color(255,245,245,245),24);
    const Gdiplus::PointF seatPoints[]={{330,315},{347,426},{364,480},{445,480},{480,520}};g.DrawLines(&seatPen,seatPoints,5);
    const auto arrow=[&](float x,float y,float dx,float dy){
        Gdiplus::Pen pen(redAccent,9);g.DrawLine(&pen,x-dx*23,y-dy*23,x+dx*23,y+dy*23);
        const Gdiplus::PointF triangle[]={{x+dx*40,y+dy*40},{x+dx*12-dy*20,y+dy*12+dx*20},{x+dx*12+dy*20,y+dy*12-dx*20}};g.FillPolygon(&accent,triangle,3);
    };
    arrow(384,254,0,-1);arrow(384,555,0,1);arrow(126,406,-1,0);arrow(642,406,1,0);
    label(L"UP",356,286,90,45,bold,white);label(L"DOWN",335,597,130,45,bold,white);
    label(sideways?L"LEFT":L"BACK",55,451,180,45,bold,white);label(sideways?L"RIGHT":L"FORWARD",550,451,205,45,bold,white);
    label(valueText.c_str(),175,menu.Desktop()?640.f:670.f,550,menu.Desktop()?200.f:145.f,bold,white);
    label(status.c_str(),40,838,690,45,normal,white);label(hint.c_str(),40,886,690,120,normal,white);
    signature_=signature;dirty_=true;return true;
}
ID3D11Texture2D* PanelImage::Render(ID3D11Device* device,const std::wstring& name,const Menu& menu,bool sideways,const std::wstring& status) {
    if(!device || !Paint(name,menu,sideways,status))return nullptr;
    if(device_.Get()!=device){texture_.Reset();device_=device;}
    if(!texture_) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=Width;desc.Height=Height;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        if(FAILED(device->CreateTexture2D(&desc,nullptr,&texture_)))return nullptr;
        dirty_=true;
    }
    if(dirty_) {
        Gdiplus::BitmapData data{};Gdiplus::Rect rect(0,0,Width,Height);
        if(gdi_->bitmap->LockBits(&rect,Gdiplus::ImageLockModeRead,PixelFormat32bppPARGB,&data)!=Gdiplus::Ok)return nullptr;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
        context->UpdateSubresource(texture_.Get(),0,nullptr,data.Scan0,UINT(data.Stride),0);gdi_->bitmap->UnlockBits(&data);dirty_=false;
    }
    return texture_.Get();
}
#ifdef GRID2VR_SEAT_PANEL_TEST
bool PanelImage::SavePreview(const std::filesystem::path& path,const std::wstring& name,const Menu& menu,bool sideways) {
    if(!Paint(name,menu,sideways))return false;
    UINT count{},bytes{};if(Gdiplus::GetImageEncodersSize(&count,&bytes)!=Gdiplus::Ok || !bytes)return false;
    std::vector<uint8_t> storage(bytes);auto* encoders=reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if(Gdiplus::GetImageEncoders(count,bytes,encoders)!=Gdiplus::Ok)return false;
    for(UINT i=0;i<count;++i)if(!std::wcscmp(encoders[i].MimeType,L"image/png"))return gdi_->bitmap->Save(path.c_str(),&encoders[i].Clsid,nullptr)==Gdiplus::Ok;
    return false;
}
#endif
}
