#include "eye_projection.h"
#include "guarded_hooks.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>

using namespace grid2vr;
namespace {
uintptr_t gameBase{},rendererObject{},ownedContext{},mainObject{};
void** rendererShadow{};
DWORD renderThread{};
bool inReplay=true,asymmetricEyeFov=true,eyeProjectionHealthy=true,clipHealthy=true;
unsigned eyeProjectionUploads{};
std::atomic<uint64_t> frame{100};
struct { bool valid=true;uint64_t id=1,frame=100;struct {uintptr_t args[4]{};} call; } pendingScene;
struct { uint64_t frame=100;bool active{},healthy=true;unsigned uploads{}; } nativeEye;
EyeFrustum eye{-1.2f,.9f,-1.f,.8f};
const EyeFrustum& CurrentFrustum() {return eye;}
// The native left-eye pass must take frusta[0] even though replayPass names the right eye.
std::array<EyeFrustum,2> CurrentFrusta() {return {eye,EyeFrustum{-.5f,.5f,-.5f,.5f}};}
template<class T>T Read(uintptr_t address) {T value{};ReadMemory(reinterpret_cast<void*>(address),&value,sizeof(value));return value;}
// Other native clip/ownership guards remain unchanged. This test isolates the
// production uploader's caller classification, projection and refusal behavior.
bool ClipInstructionsMatch(uintptr_t context) {return clipHealthy && context==ownedContext;}
bool WritableHeap(uintptr_t address,size_t bytes) {return address==ownedContext && bytes==512;}
template<class... T>void Log(const char*,T...) {}
#include "eye_projection_upload.inc"
}
#define CHECK(test) do {if (!(test)) {std::fprintf(stderr,"line %d: %s\n",__LINE__,#test);return 1;}} while(0)
int main(int argc,char** argv) {
    CHECK(argc>1);
    auto* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x500000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    CHECK(image);gameBase=reinterpret_cast<uintptr_t>(image);
    constexpr uint8_t alternate[]{0x55,0x8b,0xec,0x8b,0x55,0x08,0x83,0xec,0x08,0x53,0x56,0x8b,0xf1,0x8b,0x4e,0x28,
        0x8b,0x01,0x8b,0x80,0x44,0x01,0,0,0x57,0x52,0xff,0xd0};
    constexpr uint8_t rebind[]{0x8b,0x4e,0x28,0x8b,0x11,0x8b,0x7b,0x08,0x8b,0x82,0x44,0x01,0,0,0x57,0xff,0xd0};
    memcpy(image+0x4e4680,alternate,sizeof(alternate));memcpy(image+0x32c2db,rebind,sizeof(rebind));
    void* table[88]{};rendererShadow=table;auto* renderer=table;
    rendererObject=reinterpret_cast<uintptr_t>(&renderer);
    for(int file=1;file<argc;++file) {
        CameraContext native{};std::ifstream input(argv[file],std::ios::binary);
        CHECK(input.read(reinterpret_cast<char*>(native.data()),native.size()) && input.peek()==EOF);
        for(const auto fov:{EyeFrustum{-1.2f,.9f,-1.f,.8f},EyeFrustum{-.9f,1.2f,-1.f,.8f}}) {
            eye=fov;auto camera=native;ownedContext=reinterpret_cast<uintptr_t>(camera.data());pendingScene.call.args[3]=ownedContext;
            eyeProjectionHealthy=true;eyeProjectionUploads=0;
            ApplyReplayProjection(&renderer,ownedContext,0x25cccd);
            CHECK(eyeProjectionHealthy && eyeProjectionUploads==1);
            const auto projected=camera;
            // Captured custom-event sequence: builder, alternate upload, final
            // rebind. Both uploads must verify without changing a single byte.
            for(auto caller:{0x4e469cu,0x32c2ecu,0x4e469cu}) {
                ApplyReplayProjection(&renderer,ownedContext,caller);
                CHECK(eyeProjectionHealthy && eyeProjectionUploads==1 && camera==projected);
            }
            for(unsigned fault=0;fault<9;++fault) {
                camera=projected;eyeProjectionHealthy=true;eyeProjectionUploads=1;
                uintptr_t caller=0x4e469c;void* self=&renderer;
                if(fault==0)++caller;
                if(fault==1)eyeProjectionUploads=0;
                if(fault==2)camera=native;
                if(fault==3)self=nullptr;
                if(fault==4)renderer=nullptr;
                if(fault==5)--pendingScene.frame;
                if(fault==6)clipHealthy=false;
                if(fault==7) {float nan=NAN;memcpy(camera.data()+0x190,&nan,4);}
                if(fault==8)eye=EyeFrustum{-.9f,1.3f,-1.f,.8f};
                const auto untouched=camera;const auto uploads=eyeProjectionUploads;
                ApplyReplayProjection(self,ownedContext,caller);
                CHECK(!eyeProjectionHealthy && camera==untouched && eyeProjectionUploads==uploads);
                renderer=table;pendingScene.frame=100;clipHealthy=true;eye=fov;
            }
            camera=projected;
            for(size_t byte=0;byte<sizeof(alternate);++byte) {
                image[0x4e4680+byte]^=1;eyeProjectionHealthy=true;eyeProjectionUploads=1;
                ApplyReplayProjection(&renderer,ownedContext,0x4e469c);
                CHECK(!eyeProjectionHealthy && camera==projected && eyeProjectionUploads==1);
                image[0x4e4680+byte]^=1;
            }
            // The native left-eye pass: outside a replay, only the main context
            // (main+0x5e4) on the render thread, with its own upload count.
            std::array<uint8_t,0x600> main{};memcpy(main.data()+0x5e4,&ownedContext,sizeof(uint32_t));
            mainObject=reinterpret_cast<uintptr_t>(main.data());renderThread=GetCurrentThreadId();
            inReplay=false;eyeProjectionHealthy=true;eyeProjectionUploads=7;
            camera=native;nativeEye={100,true,true,0};
            ApplyReplayProjection(&renderer,ownedContext,0x25cccd);
            CHECK(nativeEye.healthy && nativeEye.uploads==1 && eyeProjectionUploads==7 && camera==projected);
            ApplyReplayProjection(&renderer,ownedContext,0x32c2ec);
            CHECK(nativeEye.healthy && nativeEye.uploads==1 && camera==projected);
            for(unsigned refusal=0;refusal<4;++refusal) {
                camera=native;nativeEye={100,true,true,0};
                if(refusal==0)nativeEye.active=false;
                if(refusal==1)nativeEye.frame=99;
                if(refusal==2)renderThread=GetCurrentThreadId()+1;
                if(refusal==3)memset(main.data()+0x5e4,0,4);
                ApplyReplayProjection(&renderer,ownedContext,0x25cccd);
                CHECK(camera==native && nativeEye.uploads==0 && eyeProjectionUploads==7);
                renderThread=GetCurrentThreadId();memcpy(main.data()+0x5e4,&ownedContext,sizeof(uint32_t));
            }
            inReplay=true;nativeEye={};mainObject=0;
        }
    }
    CHECK(VirtualFree(image,0,MEM_RELEASE));
    puts("Production eye projection uploads: custom-event rebind, both eyes, byte preservation and caller/state refusals pass.");
}
