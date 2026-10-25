#include "msaa_audit.h"
#include "guarded_hooks.h"
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace grid2vr::msaaaudit {
namespace {
uintptr_t base{};
bool enabled{};
std::filesystem::path path;
std::mutex mutex;
std::atomic<unsigned> textureCalls{};

void Append(const std::string& text) noexcept {
    try {std::lock_guard lock(mutex);std::ofstream out(path,std::ios::app);out<<text<<'\n';}
    catch(...) { /* Diagnostic failure cannot change the renderer. */ }
}
void Description(std::ostream& out,const D3D11_TEXTURE2D_DESC& desc) {
    out<<"{\"width\":"<<desc.Width<<",\"height\":"<<desc.Height<<",\"format\":"<<unsigned(desc.Format)
        <<",\"samples\":"<<desc.SampleDesc.Count<<",\"quality\":"<<desc.SampleDesc.Quality
        <<",\"bind\":"<<desc.BindFlags<<",\"mips\":"<<desc.MipLevels<<",\"array\":"<<desc.ArraySize<<'}';
}
}
bool Install(uintptr_t gameBase,const std::filesystem::path& folder) noexcept {
    try {
        if(enabled)return true;
        if(!std::filesystem::exists(folder/L"msaa-audit.request"))return true;
        path=folder/L"msaa-audit.jsonl";
        {std::ofstream out(path,std::ios::trunc);if(!out)return false;}
        base=gameBase;
        enabled=true;
        Append("{\"event\":\"install\",\"installed\":true,\"nativeParserHook\":false,\"textureLimit\":128}");return true;
    } catch(...) {return false;}
}
bool Enabled() noexcept {return enabled;}
void TextureCreated(const D3D11_TEXTURE2D_DESC* requested,HRESULT result,ID3D11Texture2D* texture,uintptr_t caller) noexcept {
    if(!enabled || !requested)return;
    D3D11_TEXTURE2D_DESC desc{};
    if(!ReadMemory(requested,&desc,sizeof(desc)) ||
       !(desc.SampleDesc.Count>1 || (desc.Width>=640 && desc.Height>=360 && (desc.BindFlags&(D3D11_BIND_RENDER_TARGET|D3D11_BIND_DEPTH_STENCIL)))))return;
    const auto ordinal=textureCalls.fetch_add(1);
    if(ordinal>=128)return;
    try {
        std::ostringstream out;
        out<<"{\"event\":\"texture\",\"ordinal\":"<<ordinal<<",\"callerRva\":"<<(caller-base)
            <<",\"thread\":"<<GetCurrentThreadId()<<",\"result\":"<<static_cast<uint32_t>(result)<<",\"requested\":";
        Description(out,desc);
        out<<",\"actual\":";
        if(SUCCEEDED(result) && texture) {D3D11_TEXTURE2D_DESC actual{};texture->GetDesc(&actual);Description(out,actual);}
        else out<<"null";
        out<<'}';Append(out.str());
    } catch(...) {}
}
}
