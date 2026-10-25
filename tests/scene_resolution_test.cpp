#include "scene_resolution.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>

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
    CHECK(argc==3);
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
    for(const auto text:{"", "3840", "3840 2161", "0 0", "640 359", "3841 2160", "1920 1080 extra", "1920 1080\n0", " 1920 1080", "1920  1080", "-1920 1080", "+1920 1080"}) {
        Size parsed{1,2};CHECK(!ParseSize(text,parsed) && (parsed==Size{1,2}));
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
    auto* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x87f000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(image);
    memcpy(image+0x87e490,native.data(),native.size());const auto base=reinterpret_cast<uintptr_t>(image);
    for(const auto offset:{0x87e701u,0x87e6f4u,0x87e874u,0x87e608u}) {
        auto r=Fixture();const auto before=r;image[offset]^=1;
        CHECK(!ApplyNative(base,base+0x87e887,&r.swap,{3840,2160}));CHECK(!memcmp(&r,&before,sizeof(r)));
        image[offset]^=1;
    }
    auto r=Fixture();const auto before=r;
    CHECK(!ApplyNative(base,base+0x87e888,&r.swap,{3840,2160}) && !memcmp(&r,&before,sizeof(r)));
    CHECK(!ApplyNative(base,base+0x87e887,&r.swap,{3840,2161}) && !memcmp(&r,&before,sizeof(r)));
    CHECK(ApplyNative(base,base+0x87e887,&r.swap,{3840,2160}));CHECK(r.width==3840 && r.height==2160);
    auto* readOnly=static_cast<Renderer*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(readOnly);
    *readOnly=Fixture();DWORD old{};CHECK(VirtualProtect(readOnly,4096,PAGE_READONLY,&old));
    CHECK(!ApplyNative(base,base+0x87e887,&readOnly->swap,{3840,2160}));CHECK(readOnly->width==1176);
    CHECK(VirtualFree(readOnly,0,MEM_RELEASE));
    const auto output=std::filesystem::path(argv[2])/std::to_wstring(GetCurrentProcessId());std::filesystem::create_directories(output);
    std::ofstream(output/L"scene-size.request") << "3840 2160\n";
    r=Fixture();Apply(base,output,&r.swap,base+0x87e887);CHECK(r.width==3840 && r.height==2160);
    ObserveScene(3840,2160);
    const auto receipt=Read(output/L"scene-resolution.json");
    CHECK(receipt.find("\"applied\":true")!=std::string::npos && receipt.find("\"sceneMatchesRequest\":true")!=std::string::npos);
    CHECK(receipt.find("\"engineBefore\":{\"width\":1176,\"height\":664}")!=std::string::npos);
    std::thread other([]{ObserveScene(1280,720);});other.join();CHECK(Read(output/L"scene-resolution.json")==receipt);
    CHECK(VirtualFree(image,0,MEM_RELEASE));
    puts("Scene size: all launcher percentages, guarded GRID native layout, field preservation, read-only rejection and actual-scene receipt pass.");
    return 0;
}
