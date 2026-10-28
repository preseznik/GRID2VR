#include "frontend_video_resize.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdio>

using namespace grid2vr;
#define CHECK(value) do {if (!(value)) {printf("Failed line %d: %s\n",__LINE__,#value);return 1;}} while(0)
using Camera=std::array<uint32_t,0x420/4>;
using Target=std::array<uint32_t,0x90/4>;
unsigned invalidations{},resizes{},uiUpdates{},lastFlags{};
void __fastcall Invalidate(void*,void*,unsigned flags) {++invalidations;lastFlags=flags;}
void __fastcall ResizeTarget(void* object,void*,unsigned width,unsigned height) {
    auto& target=*static_cast<Target*>(object);target[0x3c/4]=width;target[0x40/4]=height;++resizes;
}
void __fastcall UpdateUi(void*,void*) {++uiUpdates;}
void Jump(uint8_t* location,void* function) {
    location[0]=0xe9;
    const auto displacement=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(function)-reinterpret_cast<uintptr_t>(location+5));
    memcpy(location+1,&displacement,4);
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};
}
int wmain(int argc,wchar_t** argv) {
    CHECK(argc==2);
    const auto folder=std::filesystem::path(argv[1]);
    const auto resize=Read(folder/L"native-fe-video-resize.bin"),reset=Read(folder/L"native-fe-video-reset.bin"),
        initial=Read(folder/L"native-fe-video-initial.bin");
    CHECK(resize.size()==0x11e && reset.size()==35 && initial.size()==49);
    auto* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,0xda0000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));CHECK(image);
    const auto base=reinterpret_cast<uintptr_t>(image);
    memcpy(image+0x784660,resize.data(),resize.size());memcpy(image+0x1b2077,reset.data(),reset.size());
    memcpy(image+0x36bef9,initial.data(),initial.size());
    for (const auto rva:{0x78466au,0x78467fu,0x78468du,0x7846a4u,0x7846acu,0x7846c0u,0x7846ceu,0x36bf0fu,0x36bf17u}) {
        uint32_t address{};memcpy(&address,image+rva,4);address=static_cast<uint32_t>(base+address-0x400000);memcpy(image+rva,&address,4);
    }
    const std::array<uint32_t,8> sizes{1024,512,1024,512,1024,512,1024,512};
    memcpy(image+0xceb218,sizes.data(),sizeof(sizes));
    CHECK(frontendvideo::InstructionsMatch(base));
    for (const auto rva:{0x784660u,0x1b2077u,0x36bef9u,0x7846f2u,0x784729u,0xceb218u,0xceb234u}) {
        image[rva]^=1;CHECK(!frontendvideo::InstructionsMatch(base));image[rva]^=1;
    }
    // Run the captured native arithmetic and field stores, with only its three
    // graphics/UI side effects replaced by observers. No game is launched.
    constexpr uint8_t getter[]{0x8b,0x41,0x04,0x85,0xc0,0x74,0x04,0x8b,0x40,0x34,0xc3,0x33,0xc0,0xc3};
    memcpy(image+0x2aa790,getter,sizeof(getter));
    const auto constant=[&](uintptr_t rva,float value) {memcpy(image+rva,&value,4);};
    constant(0xc5bcb0,4294967296.0f);constant(0xc2c2b0,1440);constant(0xc3ad94,0.5f);
    constant(0xc4948c,360);constant(0xc69aec,2);
    Jump(image+0x934260,reinterpret_cast<void*>(Invalidate));Jump(image+0x95df80,reinterpret_cast<void*>(ResizeTarget));
    Jump(image+0x13c9c0,reinterpret_cast<void*>(UpdateUi));
    std::array<uint32_t,14> renderer{};
    const auto rendererPointer=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(renderer.data()));
    memcpy(image+0xd961bc,&rendererPointer,4);
    CHECK(FlushInstructionCache(GetCurrentProcess(),image,0xda0000));
    using ResizeFn=void(__thiscall*)(void*,unsigned,unsigned,bool);
    const auto native=reinterpret_cast<ResizeFn>(image+0x784660);
    Target target{};Camera camera{};
    const auto initialize=[&](unsigned count,unsigned presentationHeight) {
        target={};camera={};renderer[0x34/4]=presentationHeight;
        camera[0]=static_cast<uint32_t>(base+0xcdd1f0);
        camera[0x94/4]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target.data()));camera[0x410/4]=count;
        target[0x30/4]=2;target[0x44/4]=target[0x4c/4]=1;target[0x50/4]=0x12;target[0x8c/4]=8;
        native(camera.data(),1024*count,512,false);
    };
    initialize(2,3264);CHECK(camera[0x408/4]==8192 && camera[0x40c/4]==2048);
    native(camera.data(),camera[0x408/4],camera[0x40c/4],true);
    CHECK(target[0x3c/4]==32768 && target[0x40/4]==8192); // Reproduces the logged bad request.
    for (unsigned count:{1u,2u}) for (unsigned presentationHeight:{180u,360u,720u,1440u,1441u,2880u,3264u,4896u,8192u}) {
        initialize(count,presentationHeight);const auto expectedCamera=camera;const auto expectedTarget=target;
        for (unsigned attempt=0;attempt<5;++attempt) {
            unsigned width=camera[0x408/4],height=camera[0x40c/4];const auto previousInvalidations=invalidations,previousUi=uiUpdates,previousResizes=resizes;
            CHECK(frontendvideo::ResetArguments(base,base+frontendvideo::resetReturnRva,camera.data(),width,height,true));
            CHECK(width==1024*count && height==512 && camera==expectedCamera && target==expectedTarget);
            native(camera.data(),width,height,true);
            CHECK(camera==expectedCamera && target==expectedTarget && invalidations==previousInvalidations+1 &&
                uiUpdates==previousUi+1 && resizes==previousResizes+1 && lastFlags==16);
        }
    }
    // Canonical dimensions also survive a change in native height factor.
    initialize(2,3264);renderer[0x34/4]=720;unsigned width=8192,height=2048;
    CHECK(frontendvideo::ResetArguments(base,base+frontendvideo::resetReturnRva,camera.data(),width,height,true));
    native(camera.data(),width,height,true);CHECK(target[0x3c/4]==2048 && target[0x40/4]==512);
    for (unsigned fault=0;fault<17;++fault) {
        initialize(2,3264);width=8192;height=2048;auto caller=base+frontendvideo::resetReturnRva;bool force=true;
        switch (fault) {
        case 0:caller=base+0x36bf2a;break; // Initial sizing is never rewritten.
        case 1:force=false;break;
        case 2:++camera[0];break;
        case 3:camera[0x404/4]|=0x100;break;
        case 4:camera[0x410/4]=0;break;
        case 5:camera[0x410/4]=3;break;
        case 6:--camera[0x408/4];break;
        case 7:--camera[0x40c/4];break;
        case 8:camera[0x94/4]=0;break;
        case 9:--target[0x3c/4];break;
        case 10:--target[0x40/4];break;
        case 11:target[0x44/4]=2;break;
        case 12:target[0x4c/4]=6;break;
        case 13:target[0x50/4]=0x13;break;
        case 14:target[0x68/4]=1;break;
        case 15:target[0x30/4]=0;break;
        case 16:target[0x8c/4]=0;break;
        }
        const auto savedCamera=camera;const auto savedTarget=target;
        CHECK(!frontendvideo::ResetArguments(base,caller,camera.data(),width,height,force));
        CHECK(width==8192 && height==2048 && camera==savedCamera && target==savedTarget);
    }
    CHECK(VirtualFree(image,0,MEM_RELEASE));
    puts("Front-end video reset: captured native failure reproduced; repeated resets, height changes, native side effects and identity guards pass.");
    return 0;
}
