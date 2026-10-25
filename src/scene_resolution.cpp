#include "scene_resolution.h"
#include "guarded_hooks.h"
#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <string>

namespace grid2vr::sceneresolution {
namespace {
constexpr uintptr_t factoryReturn=0x87e887;
// Descriptor creation, dimension copies, and the actual factory call.
constexpr std::array<uint8_t,14> dimensions{0x8b,0x46,0x30,0x89,0x03,0x8b,0x4e,0x34,0x89,0x4b,0x04,0x8b,0x56,0x18};
constexpr std::array<uint8_t,8> descriptor{0x8d,0x9e,0xa4,0,0,0,0x57,0x53};
constexpr std::array<uint8_t,19> factory{0x8b,0x10,0x8b,0x52,0x28,0x8d,0x8e,0x9c,0,0,0,0x51,0x8b,0x0f,0x53,0x51,0x50,0xff,0xd2};
constexpr std::array<uint8_t,18> windowDimensions{0x8b,0x7e,0x30,0x89,0xbe,0x14,0x01,0,0,0x8b,0x7e,0x34,0x89,0xbe,0x18,0x01,0,0};
std::filesystem::path receipt;
Size requested{},before{},backbuffer{},scene{};
bool configured{},applied{};
DWORD owner{};
const char* reason="not_requested";

void Save() noexcept {
    try {
        if(receipt.empty())return;
        std::ofstream out(receipt,std::ios::trunc);
        out << "{\"schema\":1,\"requested\":{\"width\":" << requested.width << ",\"height\":" << requested.height
            << "},\"engineBefore\":{\"width\":" << before.width << ",\"height\":" << before.height
            << "},\"applied\":" << (applied?"true":"false") << ",\"reason\":\"" << reason
            << "\",\"backbuffer\":{\"width\":" << backbuffer.width << ",\"height\":" << backbuffer.height
            << "},\"scene\":{\"width\":" << scene.width << ",\"height\":" << scene.height
            << "},\"sceneObserved\":" << (scene.width && scene.height?"true":"false")
            << ",\"sceneMatchesRequest\":" << (scene==requested && scene.width?"true":"false") << "}\n";
    } catch(...) { /* A report cannot change native renderer lifetime. */ }
}
bool Writable(const void* address,size_t bytes) {
    MEMORY_BASIC_INFORMATION memory{};
    const auto start=reinterpret_cast<uintptr_t>(address);
    return start && VirtualQuery(address,&memory,sizeof(memory)) && memory.State==MEM_COMMIT &&
        !(memory.Protect&(PAGE_GUARD|PAGE_NOACCESS)) &&
        (memory.Protect&(PAGE_READWRITE|PAGE_EXECUTE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_WRITECOPY)) &&
        start>=reinterpret_cast<uintptr_t>(memory.BaseAddress) &&
        bytes<=memory.RegionSize-(start-reinterpret_cast<uintptr_t>(memory.BaseAddress));
}
}
bool ValidSize(Size size) {
    // Match every launcher percentage, including odd sizes produced by integer
    // scaling. Reject unrelated dimensions instead of accepting an arbitrary cap.
    for(unsigned scale=50;scale<=300;++scale)
        if(size.width==1280*scale/100 && size.height==720*scale/100)return true;
    return false;
}
bool ParseSize(std::string_view text,Size& size) {
    if(text.empty() || text.size()>32)return false;
    const auto space=text.find(' ');
    if(space==std::string_view::npos)return false;
    Size candidate;
    const auto width=std::from_chars(text.data(),text.data()+space,candidate.width);
    if(width.ec!=std::errc{} || width.ptr!=text.data()+space)return false;
    auto end=text.size();
    while(end>space+1 && (text[end-1]=='\r' || text[end-1]=='\n'))--end;
    const auto height=std::from_chars(text.data()+space+1,text.data()+end,candidate.height);
    if(height.ec!=std::errc{} || height.ptr!=text.data()+end || !ValidSize(candidate))return false;
    size=candidate;return true;
}
bool SetFields(Renderer& renderer,Size size) {
    if(!ValidSize(size) || renderer.fullscreen || !renderer.swap.Windowed || !renderer.window ||
        renderer.window!=renderer.swap.OutputWindow || !renderer.device || renderer.chain ||
        renderer.width!=renderer.swap.BufferDesc.Width || renderer.height!=renderer.swap.BufferDesc.Height ||
        renderer.windowWidth!=renderer.width || renderer.windowHeight!=renderer.height ||
        !renderer.width || !renderer.height || renderer.swap.SampleDesc.Count!=1 ||
        renderer.swap.BufferDesc.Format!=DXGI_FORMAT_R10G10B10A2_UNORM)return false;
    renderer.width=renderer.windowWidth=renderer.swap.BufferDesc.Width=size.width;
    renderer.height=renderer.windowHeight=renderer.swap.BufferDesc.Height=size.height;
    return true;
}
bool ApplyNative(uintptr_t gameBase,uintptr_t caller,DXGI_SWAP_CHAIN_DESC* desc,Size size) {
    if(!gameBase || caller!=gameBase+factoryReturn || !desc || !ValidSize(size) ||
        !MatchesInstructions(reinterpret_cast<void*>(gameBase+0x87e701),dimensions) ||
        !MatchesInstructions(reinterpret_cast<void*>(gameBase+0x87e6f4),descriptor) ||
        !MatchesInstructions(reinterpret_cast<void*>(gameBase+0x87e874),factory) ||
        !MatchesInstructions(reinterpret_cast<void*>(gameBase+0x87e608),windowDimensions))return false;
    const auto address=reinterpret_cast<uintptr_t>(desc);
    if(address<offsetof(Renderer,swap) || address%alignof(Renderer))return false;
    auto* renderer=reinterpret_cast<Renderer*>(address - offsetof(Renderer,swap));
    return Writable(renderer,sizeof(*renderer)) && SetFields(*renderer,size);
}
void Apply(uintptr_t gameBase,const std::filesystem::path& output,DXGI_SWAP_CHAIN_DESC* desc,uintptr_t caller) noexcept {
    try {
        // A different factory caller must not consume the request or infer a
        // renderer base from its unrelated descriptor.
        if(caller!=gameBase+factoryReturn || configured)return;
        configured=true;owner=GetCurrentThreadId();
        const auto request=output/L"scene-size.request";
        if(!std::filesystem::is_regular_file(request))return;
        receipt=output/L"scene-resolution.json";
        if(std::filesystem::file_size(request)>32) {reason="invalid_request";Save();return;}
        std::ifstream input(request,std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(input),{}};
        if(!input || !ParseSize(text,requested)) {reason="invalid_request";Save();return;}
        DXGI_SWAP_CHAIN_DESC original{};
        if(ReadMemory(desc,&original,sizeof(original)))before={original.BufferDesc.Width,original.BufferDesc.Height};
        applied=ApplyNative(gameBase,caller,desc,requested);
        reason=applied?"applied":"renderer_guard_rejected";Save();
    } catch(...) {reason="request_unavailable";Save();}
}
void ObserveSwapchain(IDXGISwapChain* chain) noexcept {
    if(receipt.empty() || !chain || GetCurrentThreadId()!=owner)return;
    DXGI_SWAP_CHAIN_DESC desc{};
    if(SUCCEEDED(chain->GetDesc(&desc))) {
        const Size actual{desc.BufferDesc.Width,desc.BufferDesc.Height};
        if(actual!=backbuffer) {backbuffer=actual;Save();}
    }
}
void ObserveScene(unsigned width,unsigned height) noexcept {
    if(receipt.empty() || GetCurrentThreadId()!=owner || !width || !height)return;
    const Size actual{width,height};
    if(actual!=scene) {scene=actual;Save();}
}
}
