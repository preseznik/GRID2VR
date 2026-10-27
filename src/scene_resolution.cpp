#include "scene_resolution.h"
#include "resolution_limits.h"
#include "guarded_hooks.h"
#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <string>

namespace grid2vr::sceneresolution {
namespace {
constexpr uintptr_t factoryReturn=0x87e887;
constexpr uintptr_t recreateReturn=0x87d7ae,resizeReturn=0x87f99a;
// Descriptor creation, dimension copies, and the actual factory call.
constexpr std::array<uint8_t,14> dimensions{0x8b,0x46,0x30,0x89,0x03,0x8b,0x4e,0x34,0x89,0x4b,0x04,0x8b,0x56,0x18};
constexpr std::array<uint8_t,8> descriptor{0x8d,0x9e,0xa4,0,0,0,0x57,0x53};
constexpr std::array<uint8_t,19> factory{0x8b,0x10,0x8b,0x52,0x28,0x8d,0x8e,0x9c,0,0,0,0x51,0x8b,0x0f,0x53,0x51,0x50,0xff,0xd2};
constexpr std::array<uint8_t,18> windowDimensions{0x8b,0x7e,0x30,0x89,0xbe,0x14,0x01,0,0,0x8b,0x7e,0x34,0x89,0xbe,0x18,0x01,0,0};
constexpr std::array<uint8_t,23> recreateFactory{0x8b,0x08,0x57,0x8d,0x96,0xa4,0,0,0,0x52,0x8b,0x96,0x98,0,0,0,0x52,0x50,0x8b,0x41,0x28,0xff,0xd0};
constexpr std::array<uint8_t,27> resizeCall{0x8b,0x96,0x18,0x01,0,0,0x52,0x8b,0x96,0x14,0x01,0,0,0x52,0x8b,0x96,0xcc,0,0,0,0x52,0x50,0x8b,0x41,0x34,0xff,0xd0};
constexpr std::array<uint8_t,18> resizeDimensions{0x8b,0x46,0x30,0x8b,0x4e,0x34,0x89,0x86,0x14,0x01,0,0,0x89,0x8e,0x18,0x01,0,0};
std::filesystem::path receipt;
Size requested{},before{},backbuffer{},scene{};
bool configured{},applied{};
DWORD owner{};
const char* reason="not_requested";
Renderer* boundRenderer{};
IUnknown* boundDevice{};
HWND boundWindow{};
IDXGISwapChain* boundChain{}; // Identity only: retaining it would obstruct native recreation.
uintptr_t boundBase{};
uint64_t resizeCount{},recreationCount{};
Size lastIncoming{},lastEffective{};
HRESULT lastResult=S_OK;
bool operationComplete{};

void Save() noexcept {
    try {
        if(receipt.empty())return;
        std::ofstream out(receipt,std::ios::trunc);
        out << "{\"schema\":1,\"processId\":" << GetCurrentProcessId() << ",\"window\":" << reinterpret_cast<uintptr_t>(boundWindow)
            << ",\"requested\":{\"width\":" << requested.width << ",\"height\":" << requested.height
            << "},\"engineBefore\":{\"width\":" << before.width << ",\"height\":" << before.height
            << "},\"applied\":" << (applied?"true":"false") << ",\"reason\":\"" << reason
            << "\",\"backbuffer\":{\"width\":" << backbuffer.width << ",\"height\":" << backbuffer.height
            << "},\"scene\":{\"width\":" << scene.width << ",\"height\":" << scene.height
            << "},\"sceneObserved\":" << (scene.width && scene.height?"true":"false")
            << ",\"sceneMatchesRequest\":" << (scene==requested && scene.width?"true":"false")
            << ",\"resizeCount\":" << resizeCount << ",\"recreationCount\":" << recreationCount
            << ",\"lastIncoming\":{\"width\":" << lastIncoming.width << ",\"height\":" << lastIncoming.height
            << "},\"lastEffective\":{\"width\":" << lastEffective.width << ",\"height\":" << lastEffective.height
            << "},\"operationComplete\":" << (operationComplete?"true":"false") << ",\"lastResult\":" << lastResult << "}\n";
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
bool FieldsAgree(const Renderer& r,bool requireNonzero=true) {
    return !r.fullscreen && r.swap.Windowed && r.window && r.window==r.swap.OutputWindow && r.device &&
        (!requireNonzero || (r.width && r.height)) && r.width==r.swap.BufferDesc.Width && r.height==r.swap.BufferDesc.Height &&
        r.windowWidth==r.width && r.windowHeight==r.height && r.swap.SampleDesc.Count==1 &&
        r.swap.BufferDesc.Format==DXGI_FORMAT_R10G10B10A2_UNORM;
}
void SetSize(Renderer& r,Size size) {
    r.width=r.windowWidth=r.swap.BufferDesc.Width=size.width;
    r.height=r.windowHeight=r.swap.BufferDesc.Height=size.height;
}
Change Snapshot(Renderer* r,Size incoming,bool recreation=false) {
    return {r,{r->width,r->height},{r->windowWidth,r->windowHeight},
        {r->swap.BufferDesc.Width,r->swap.BufferDesc.Height},incoming,requested,r->chain,recreation};
}
void Restore(const Change& change) {
    auto* r=change.renderer;
    if(!r || !Writable(r,sizeof(*r)))return;
    // Do not overwrite a subsequent game change, even on an operation failure.
    if(r->width==change.effective.width && r->height==change.effective.height &&
        r->windowWidth==change.effective.width && r->windowHeight==change.effective.height &&
        r->swap.BufferDesc.Width==change.effective.width && r->swap.BufferDesc.Height==change.effective.height) {
        r->width=change.engine.width;r->height=change.engine.height;
        r->windowWidth=change.window.width;r->windowHeight=change.window.height;
        r->swap.BufferDesc.Width=change.descriptor.width;r->swap.BufferDesc.Height=change.descriptor.height;
    }
}
void BeginOperation(Size incoming) {
    lastIncoming=incoming;lastEffective=incoming;operationComplete=false;
    applied=false;scene={};backbuffer={};
}
void Reject(const char* why) {reason=why;Save();}
}
bool ValidSize(Size size) {
    // Headsets have different aspect ratios; instruction and ownership guards
    // still apply to every native write. Bound dimensions to scene-copy limits.
    return size.width>=320 && size.height>=180 && resolution::ValidScene(size.width,size.height);
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
    if(!ValidSize(size) || renderer.chain || !FieldsAgree(renderer))return false;
    SetSize(renderer,size);
    return true;
}
bool ApplyNative(uintptr_t gameBase,uintptr_t caller,DXGI_SWAP_CHAIN_DESC* desc,Size size) {
    if(!gameBase || !desc || !ValidSize(size))return false;
    if(caller==gameBase+factoryReturn) {
        if(!MatchesInstructions(reinterpret_cast<void*>(gameBase+0x87e701),dimensions) ||
            !MatchesInstructions(reinterpret_cast<void*>(gameBase+0x87e6f4),descriptor) ||
            !MatchesInstructions(reinterpret_cast<void*>(gameBase+0x87e874),factory) ||
            !MatchesInstructions(reinterpret_cast<void*>(gameBase+0x87e608),windowDimensions))return false;
    } else if(caller!=gameBase+recreateReturn ||
        !MatchesInstructions(reinterpret_cast<void*>(gameBase+0x87d797),recreateFactory) ||
        !MatchesInstructions(reinterpret_cast<void*>(gameBase+0x87f8a7),resizeDimensions))return false;
    const auto address=reinterpret_cast<uintptr_t>(desc);
    if(address<offsetof(Renderer,swap) || address%alignof(Renderer))return false;
    auto* renderer=reinterpret_cast<Renderer*>(address - offsetof(Renderer,swap));
    return Writable(renderer,sizeof(*renderer)) && SetFields(*renderer,size);
}
bool ApplyResizeNative(uintptr_t gameBase,uintptr_t caller,Renderer& renderer,IDXGISwapChain* chain,Size size) {
    if(!gameBase || caller!=gameBase+resizeReturn || !ValidSize(size) || !chain ||
        !MatchesInstructions(reinterpret_cast<void*>(gameBase+0x87f97f),resizeCall) ||
        !MatchesInstructions(reinterpret_cast<void*>(gameBase+0x87f8a7),resizeDimensions) ||
        !Writable(&renderer,sizeof(renderer)) || renderer.chain!=chain || !FieldsAgree(renderer,false))return false;
    SetSize(renderer,size);return true;
}
Change PrepareCreate(uintptr_t gameBase,const std::filesystem::path& output,IUnknown* device,
    DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** result,uintptr_t caller) noexcept {
    try {
        // A different factory caller must not consume the request or infer a
        // renderer base from its unrelated descriptor.
        const bool recreation=caller==gameBase+recreateReturn;
        if(caller!=gameBase+factoryReturn && !recreation)return {};
        if(!configured) {
            if(recreation)return {};
            configured=true;owner=GetCurrentThreadId();boundBase=gameBase;
            const auto request=output/L"scene-size.request";
            if(!std::filesystem::is_regular_file(request))return {};
            receipt=output/L"scene-resolution.json";
            if(std::filesystem::file_size(request)>32) {Reject("invalid_request");return {};}
            std::ifstream input(request,std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(input),{}};
            if(!input || !ParseSize(text,requested)) {Reject("invalid_request");return {};}
        } else if(!recreation)return {};
        if(receipt.empty() || !ValidSize(requested) || owner!=GetCurrentThreadId() || gameBase!=boundBase)return {};
        DXGI_SWAP_CHAIN_DESC original{};
        if(!ReadMemory(desc,&original,sizeof(original))) {Reject("renderer_guard_rejected");return {};}
        const auto address=reinterpret_cast<uintptr_t>(desc);
        if(address<offsetof(Renderer,swap) || address%alignof(Renderer))return {};
        auto* r=reinterpret_cast<Renderer*>(address - offsetof(Renderer,swap));
        // An unrelated recreation cannot replace the verified binding.
        if(recreation && r!=boundRenderer)return {};
        BeginOperation({original.BufferDesc.Width,original.BufferDesc.Height});
        if(recreation) {++recreationCount;boundChain=nullptr;} else before=lastIncoming;
        Renderer snapshot{};
        if(!ReadMemory(r,&snapshot,sizeof(snapshot)) || result!=&r->chain || device!=snapshot.device ||
            (recreation && (snapshot.device!=boundDevice || snapshot.window!=boundWindow))) {
            Reject("renderer_guard_rejected");return {};
        }
        const auto change=Snapshot(r,lastIncoming,recreation);
        if(!ApplyNative(gameBase,caller,desc,requested)) {Reject("renderer_guard_rejected");return {};}
        boundRenderer=r;boundDevice=device;boundWindow=r->window;
        lastEffective=requested;reason="creation_pending";Save();return change;
    } catch(...) {Reject("request_unavailable");return {};}
}
void CompleteCreate(const Change& change,HRESULT result,IDXGISwapChain* chain) noexcept {
    if(!change || owner!=GetCurrentThreadId())return;
    lastResult=result;operationComplete=true;
    if(FAILED(result) || !chain || change.renderer->chain!=chain) {
        Restore(change);boundChain=nullptr;Reject("creation_failed");return;
    }
    boundChain=chain;ObserveSwapchain(chain);
    applied=backbuffer==requested;reason=applied?"applied":"backbuffer_mismatch";Save();
}
Change PrepareResize(uintptr_t gameBase,uintptr_t caller,IDXGISwapChain* chain,UINT& width,UINT& height,DXGI_FORMAT format) noexcept {
    if(!boundChain || chain!=boundChain || owner!=GetCurrentThreadId() || gameBase!=boundBase)return {};
    ++resizeCount;BeginOperation({width,height});
    Change rejected{};rejected.chain=chain;
    Renderer r{};
    if(!ReadMemory(boundRenderer,&r,sizeof(r)) || r.device!=boundDevice || r.window!=boundWindow || r.chain!=chain ||
        (format!=DXGI_FORMAT_UNKNOWN && format!=r.swap.BufferDesc.Format) ||
        (width && width!=r.windowWidth) || (height && height!=r.windowHeight)) {
        Reject("resize_guard_rejected");return rejected;
    }
    const auto change=Snapshot(boundRenderer,{width,height});
    if(!ApplyResizeNative(gameBase,caller,*boundRenderer,chain,requested)) {Reject("resize_guard_rejected");return rejected;}
    width=requested.width;height=requested.height;lastEffective=requested;reason="resize_pending";Save();return change;
}
void CompleteResize(const Change& change,HRESULT result) noexcept {
    if(!change.chain || change.chain!=boundChain || owner!=GetCurrentThreadId())return;
    lastResult=result;operationComplete=true;
    if(!change) {ObserveSwapchain(change.chain);Save();return;}
    if(FAILED(result)) {Restore(change);ObserveSwapchain(change.chain);Reject("resize_failed");return;}
    ObserveSwapchain(change.chain);applied=backbuffer==requested;
    reason=applied?"applied":"backbuffer_mismatch";Save();
}
void ObserveSwapchain(IDXGISwapChain* chain) noexcept {
    if(receipt.empty() || !chain || chain!=boundChain || GetCurrentThreadId()!=owner)return;
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
