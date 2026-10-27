#include "scene_resolution.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>
#include <wrl/client.h>

using namespace grid2vr::sceneresolution;
#define CHECK(value) do { if(!(value)) {printf("Failed line %d: %s\n",__LINE__,#value);return 1;} } while(0)
Renderer Fixture() {
    Renderer r{};
    memset(&r,0x5a,sizeof(r));
    r.fullscreen=0;r.window=r.swap.OutputWindow=reinterpret_cast<HWND>(1);
    r.device=reinterpret_cast<IUnknown*>(2);r.chain=nullptr;r.swap.Windowed=TRUE;
    r.width=r.windowWidth=r.swap.BufferDesc.Width=1176;
    r.height=r.windowHeight=r.swap.BufferDesc.Height=664;
    r.swap.SampleDesc.Count=1;r.swap.BufferDesc.Format=DXGI_FORMAT_R10G10B10A2_UNORM;
    return r;
}
std::string Read(const std::filesystem::path& file) {
    std::ifstream in(file,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};
}
int wmain(int argc,wchar_t** argv) {
    CHECK(argc==5);
    for(unsigned scale=50;scale<=300;++scale) {
        const Size size{1280*scale/100,720*scale/100};Size parsed{};
        const auto text=std::to_string(size.width)+" "+std::to_string(size.height)+"\r\n";
        CHECK(ParseSize(text,parsed) && parsed==size);
        auto r=Fixture();const auto before=r;CHECK(SetFields(r,size));
        CHECK(r.width==size.width && r.height==size.height && r.windowWidth==size.width && r.windowHeight==size.height);
        CHECK(r.swap.BufferDesc.Width==size.width && r.swap.BufferDesc.Height==size.height);
        for(size_t i=0;i<sizeof(r);++i) {
            const bool dimensions=(i>=0x30 && i<0x38) || (i>=0xa4 && i<0xac) || (i>=0x114 && i<0x11c);
            CHECK(dimensions || reinterpret_cast<const uint8_t*>(&r)[i]==reinterpret_cast<const uint8_t*>(&before)[i]);
        }
    }
    for(const auto text:{"", "3840", "8193 2160", "0 0", "640 179", "8192 8193", "1920 1080 extra", "1920 1080\n0", " 1920 1080", "1920  1080", "-1920 1080", "+1920 1080"}) {
        Size parsed{1,2};CHECK(!ParseSize(text,parsed) && (parsed==Size{1,2}));
    }
    for(const Size size: {Size{2400,2600},Size{3288,3562},Size{4096,4096},Size{320,180},Size{4608,4896},Size{6144,6528},Size{8192,8192}}) {
        auto r=Fixture();CHECK(SetFields(r,size));
        CHECK(r.width==size.width && r.height==size.height && r.swap.BufferDesc.Width==size.width && r.swap.BufferDesc.Height==size.height);
    }
    for(unsigned fault=0;fault<10;++fault) {
        auto r=Fixture();
        switch(fault) {
        case 0:r.fullscreen=1;break;
        case 1:r.swap.Windowed=FALSE;break;
        case 2:r.swap.OutputWindow=nullptr;break;
        case 3:r.width=1280;break;
        case 4:r.height=720;break;
        case 5:r.windowWidth=1280;break;
        case 6:r.windowHeight=720;break;
        case 7:r.device=nullptr;break;
        case 8:r.chain=reinterpret_cast<IDXGISwapChain*>(3);break;
        case 9:r.swap.SampleDesc.Count=4;break;
        }
        const auto before=r;CHECK(!SetFields(r,{3840,2160}));CHECK(!memcmp(&r,&before,sizeof(r)));
    }
    const auto native=Read(argv[1]);CHECK(native.size()==0x4fd);
    auto* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x880000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(image);
    memcpy(image+0x87e490,native.data(),native.size());const auto base=reinterpret_cast<uintptr_t>(image);
    const auto recreate=Read(argv[3]),resize=Read(argv[4]);CHECK(recreate.size()==0x124 && resize.size()==0x430);
    memcpy(image+0x87d700,recreate.data(),recreate.size());memcpy(image+0x87f6a0,resize.data(),resize.size());
    for(unsigned scale=50;scale<=300;++scale) {
        const Size size{1280*scale/100,720*scale/100};
        auto fixture=Fixture();CHECK(ApplyNative(base,base+0x87e887,&fixture.swap,size));
        fixture=Fixture();CHECK(ApplyNative(base,base+0x87d7ae,&fixture.swap,size));
        fixture=Fixture();fixture.chain=reinterpret_cast<IDXGISwapChain*>(3);const auto previous=fixture;
        CHECK(ApplyResizeNative(base,base+0x87f99a,fixture,fixture.chain,size));
        CHECK(fixture.width==size.width && fixture.height==size.height && fixture.swap.BufferDesc.Width==size.width && fixture.windowHeight==size.height);
        for(size_t i=0;i<sizeof(fixture);++i) {
            const bool dimensions=(i>=0x30 && i<0x38) || (i>=0xa4 && i<0xac) || (i>=0x114 && i<0x11c);
            CHECK(dimensions || reinterpret_cast<const uint8_t*>(&fixture)[i]==reinterpret_cast<const uint8_t*>(&previous)[i]);
        }
    }
    for(const auto offset:{0x87d797u,0x87f8a7u,0x87f97fu}) {
        auto fixture=Fixture();const auto previous=fixture;image[offset]^=1;
        if(offset!=0x87f97f)CHECK(!ApplyNative(base,base+0x87d7ae,&fixture.swap,{3840,2160}) && !memcmp(&fixture,&previous,sizeof(fixture)));
        fixture.chain=reinterpret_cast<IDXGISwapChain*>(3);
        if(offset!=0x87d797)CHECK(!ApplyResizeNative(base,base+0x87f99a,fixture,fixture.chain,{3840,2160}) && fixture.width==1176);
        image[offset]^=1;
    }
    for(const auto offset:{0x87e701u,0x87e6f4u,0x87e874u,0x87e608u}) {
        auto r=Fixture();const auto before=r;image[offset]^=1;
        CHECK(!ApplyNative(base,base+0x87e887,&r.swap,{3840,2160}));CHECK(!memcmp(&r,&before,sizeof(r)));
        image[offset]^=1;
    }
    auto r=Fixture();const auto before=r;
    CHECK(!ApplyNative(base,base+0x87e888,&r.swap,{3840,2160}) && !memcmp(&r,&before,sizeof(r)));
    CHECK(!ApplyNative(base,base+0x87e887,&r.swap,{8192,8193}) && !memcmp(&r,&before,sizeof(r)));
    CHECK(ApplyNative(base,base+0x87e887,&r.swap,{3840,2160}));CHECK(r.width==3840 && r.height==2160);
    auto* readOnly=static_cast<Renderer*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(readOnly);
    *readOnly=Fixture();DWORD old{};CHECK(VirtualProtect(readOnly,4096,PAGE_READONLY,&old));
    CHECK(!ApplyNative(base,base+0x87e887,&readOnly->swap,{3840,2160}));CHECK(readOnly->width==1176);
    CHECK(VirtualFree(readOnly,0,MEM_RELEASE));
    const auto output=std::filesystem::path(argv[2])/std::to_wstring(GetCurrentProcessId());std::filesystem::create_directories(output);
    std::ofstream(output/L"scene-size.request") << "3840 2160\n";
    using Microsoft::WRL::ComPtr;
    const auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);CHECK(module);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));CHECK(create);
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    CHECK(SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory> factory;
    CHECK(SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))));
    const auto window=CreateWindowExW(0,L"STATIC",L"GRID2VR hidden resolution fixture",WS_OVERLAPPEDWINDOW,0,0,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window && !IsWindowVisible(window));
    r={};r.window=r.swap.OutputWindow=window;r.device=device.Get();r.context=context.Get();
    r.swap.Windowed=TRUE;r.swap.BufferCount=2;r.swap.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    r.swap.SampleDesc.Count=1;r.swap.BufferDesc.Format=DXGI_FORMAT_R10G10B10A2_UNORM;r.swap.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    const auto setNativeSize=[&](Size size) {r.width=r.windowWidth=r.swap.BufferDesc.Width=size.width;r.height=r.windowHeight=r.swap.BufferDesc.Height=size.height;};
    setNativeSize({1176,664});
    auto foreign=r;
    CHECK(!PrepareCreate(base,output,device.Get(),&foreign.swap,&foreign.chain,base+0x87e888));
    const auto initial=PrepareCreate(base,output,device.Get(),&r.swap,&r.chain,base+0x87e887);CHECK(initial && r.width==3840);
    CHECK(!Read(output/L"scene-resolution.json").empty());
    auto hr=factory->CreateSwapChain(device.Get(),&r.swap,&r.chain);CHECK(SUCCEEDED(hr));CompleteCreate(initial,hr,r.chain);
    ComPtr<IDXGISwapChain> chain;chain.Attach(r.chain);
    ObserveScene(3840,2160);
    const auto receipt=Read(output/L"scene-resolution.json");
    CHECK(receipt.find("\"applied\":true")!=std::string::npos && receipt.find("\"sceneMatchesRequest\":true")!=std::string::npos);
    CHECK(receipt.find("\"engineBefore\":{\"width\":1176,\"height\":664}")!=std::string::npos);
    std::thread other([]{ObserveScene(1280,720);});other.join();CHECK(Read(output/L"scene-resolution.json")==receipt);
    UINT width=2560,height=1417;
    bool otherChanged{};
    std::thread wrongThread([&]{otherChanged=bool(PrepareResize(base,base+0x87f99a,chain.Get(),width,height,DXGI_FORMAT_UNKNOWN));});
    wrongThread.join();CHECK(!otherChanged && width==2560 && height==1417 && Read(output/L"scene-resolution.json")==receipt);
    for(const auto input:{Size{2560,1417},Size{800,600},Size{3840,2160},Size{0,0},Size{0,1417},Size{2560,0},Size{2560,1417}}) {
        CHECK(SetWindowPos(window,nullptr,0,0,input.width?input.width:320,input.height?input.height:240,SWP_NOACTIVATE|SWP_NOZORDER));
        CHECK(!IsWindowVisible(window));
        setNativeSize(input);
        width=input.width;height=input.height;
        const auto change=PrepareResize(base,base+0x87f99a,chain.Get(),width,height,DXGI_FORMAT_UNKNOWN);
        CHECK(change && width==3840 && height==2160 && r.width==3840 && r.windowHeight==2160);
        CHECK(Read(output/L"scene-resolution.json").find("\"sceneObserved\":false")!=std::string::npos);
        hr=chain->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,0);CHECK(SUCCEEDED(hr));CompleteResize(change,hr);
        DXGI_SWAP_CHAIN_DESC actual{};CHECK(SUCCEEDED(chain->GetDesc(&actual)) && actual.BufferDesc.Width==3840 && actual.BufferDesc.Height==2160);
        CHECK(Read(output/L"scene-resolution.json").find("\"applied\":true")!=std::string::npos);
        ObserveScene(actual.BufferDesc.Width,actual.BufferDesc.Height);
    }
    const auto resized=Read(output/L"scene-resolution.json");
    CHECK(resized.find("\"resizeCount\":7")!=std::string::npos && resized.find("\"operationComplete\":true,\"lastResult\":0")!=std::string::npos);
    CHECK(resized.find("\"lastIncoming\":{\"width\":2560,\"height\":1417}")!=std::string::npos &&
        resized.find("\"lastEffective\":{\"width\":3840,\"height\":2160}")!=std::string::npos);
    // A real failed DXGI resize restores only our six words and clears stale scene evidence.
    ComPtr<ID3D11Texture2D> retained;CHECK(SUCCEEDED(chain->GetBuffer(0,IID_PPV_ARGS(&retained))));
    setNativeSize({2560,1417});const auto failedBefore=r;width=2560;height=1417;
    auto change=PrepareResize(base,base+0x87f99a,chain.Get(),width,height,DXGI_FORMAT_UNKNOWN);CHECK(change);
    hr=chain->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,0);CHECK(FAILED(hr));CompleteResize(change,hr);
    CHECK(!memcmp(&r,&failedBefore,sizeof(r)) && Read(output/L"scene-resolution.json").find("resize_failed")!=std::string::npos);
    retained.Reset();
    // Unsupported callers, foreign identities and altered code do not change fields or arguments.
    for(unsigned fault=0;fault<7;++fault) {
        setNativeSize({2560,1417});width=2560;height=1417;auto caller=base+0x87f99a;auto* target=chain.Get();
        if(fault==0)++caller;
        if(fault==1)target=reinterpret_cast<IDXGISwapChain*>(3);
        if(fault==2)r.device=reinterpret_cast<IUnknown*>(3);
        if(fault==3)r.window=nullptr;
        if(fault==4)r.chain=nullptr;
        if(fault==5)image[0x87f97f]^=1;
        const auto snapshot=r;
        CHECK(!PrepareResize(base,caller,target,width,height,fault==6?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_UNKNOWN));
        CHECK(width==2560 && height==1417 && !memcmp(&r,&snapshot,sizeof(r)));
        if(fault==5)image[0x87f97f]^=1;
        r.device=device.Get();r.window=window;r.chain=chain.Get();
    }
    // Native recreation releases its old chain before calling the factory with the same descriptor.
    chain.Reset();r.chain=nullptr;setNativeSize({2560,1417});
    const auto rejectedBefore=r;auto* wrongDevice=reinterpret_cast<IUnknown*>(3);
    CHECK(!PrepareCreate(base,output,wrongDevice,&r.swap,&r.chain,base+0x87d7ae) && !memcmp(&r,&rejectedBefore,sizeof(r)));
    change=PrepareCreate(base,output,device.Get(),&r.swap,&r.chain,base+0x87d7ae);CHECK(change && change.recreation && r.width==3840);
    CompleteCreate(change,E_FAIL,nullptr);CHECK(r.width==2560 && r.height==1417);
    change=PrepareCreate(base,output,device.Get(),&r.swap,&r.chain,base+0x87d7ae);CHECK(change);
    hr=factory->CreateSwapChain(device.Get(),&r.swap,&r.chain);CHECK(SUCCEEDED(hr));CompleteCreate(change,hr,r.chain);chain.Attach(r.chain);
    CHECK(Read(output/L"scene-resolution.json").find("\"sceneObserved\":false")!=std::string::npos);
    CHECK(Read(output/L"scene-resolution.json").find("\"recreationCount\":3")!=std::string::npos);
    setNativeSize({800,600});width=800;height=600;
    change=PrepareResize(base,base+0x87f99a,chain.Get(),width,height,DXGI_FORMAT_UNKNOWN);CHECK(change);
    hr=chain->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,0);CHECK(SUCCEEDED(hr));CompleteResize(change,hr);
    CHECK(Read(output/L"scene-resolution.json").find("\"applied\":true")!=std::string::npos);
    chain.Reset();r.chain=nullptr;CHECK(DestroyWindow(window));
    CHECK(VirtualFree(image,0,MEM_RELEASE));
    puts("Scene size: all scales, native init/resize/recreation guards, hidden-window WARP resizing, failure rollback and fresh measurement receipts pass.");
    return 0;
}
