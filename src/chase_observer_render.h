#pragma once
#include "chase_observer_owner.h"
#include "scene_call.h"
#include <array>

namespace grid2vr::chaseobserver {
struct RenderObservation {
    uintptr_t main{},mainTable{},renderer{},rendererTable{},target{},context{};
    uint32_t type{};
    uint8_t active{};
    OwnerObservation owner{};
    SceneCall call{};
    std::array<uint32_t,36> exportedView{},globalView{},mainView{},renderView{};
    std::array<uint32_t,128> contextWords{};
    bool exportedReadable{},globalReadable{},mainReadable{},renderReadable{},contextReadable{};
};

// Observe main slot 5 after the native camera builder, before forwarding its
// behavior. The caller owns executable/instruction guards, source freshness,
// thread/serial correlation and native forwarding. This reads raw data only;
// success is not a displayed-camera or camera-write acceptance decision.
template<class Read>
const char* CaptureRender(uintptr_t base,uintptr_t main,uintptr_t camera,
                          const OwnerObservation& sourceOwner,std::span<const uintptr_t> ownerStack,
                          uintptr_t context,uintptr_t view,uintptr_t packet,
                          std::span<const uintptr_t> sceneStack,Read&& read,RenderObservation& out) {
    out={};out.main=main;
    if(!OwnerNativePointer(base) || base>UINT32_MAX-0xdff9b0-0x90 || !OwnerNativePointer(main) ||
       !OwnerNativePointer(context) || !OwnerNativePointer(view) || !OwnerNativePointer(packet))
        return "render_address_invalid";
    if(!DecodeSceneCall(sceneStack,base,5,context,view,packet,out.call))return "render_scene_stack";
    // 0x549e20 passes +5f0 then +680 to 0x7a7bd0; its complete-scene
    // call passes +680 as the rendered view, with +5f0 in args[1].
    if(view!=main+0x680 || out.call.args[1]!=main+0x5f0)return "render_view_arguments";
    const auto identity=[&](RenderObservation& value) {
        const auto pointer=[&](uintptr_t address,uintptr_t& destination) {
            uint32_t native{};
            if(!read(address,&native,sizeof(native)))return false;
            destination=native;return OwnerNativePointer(destination);
        };
        uint32_t target{};
        if(!pointer(main,value.mainTable) || value.mainTable!=base+0xcd9da8 ||
           !read(main+0x14,&value.type,sizeof(value.type)) || value.type!=0x400 ||
           !read(main+0x18,&value.active,1) || value.active!=1 ||
           !read(main+0x94,&target,sizeof(target)))return false;
        value.target=target;
        return target && pointer(main+0x28,value.renderer) && pointer(value.renderer,value.rendererTable) &&
            value.rendererTable==base+0xd4a5a0 && pointer(main+0x5e4,value.context) && value.context==context;
    };
    if(!identity(out))return "render_main_identity";
    if(const auto refusal=DecodeOwner(ownerStack,base,camera,read,out.owner))return refusal;
    if(out.owner!=sourceOwner)return "render_source_changed";
    out.exportedReadable=read(out.owner.output,out.exportedView.data(),sizeof(out.exportedView));
    out.globalReadable=read(base+0xdff9b0,out.globalView.data(),sizeof(out.globalView));
    out.mainReadable=read(main+0x5f0,out.mainView.data(),sizeof(out.mainView));
    out.renderReadable=read(main+0x680,out.renderView.data(),sizeof(out.renderView));
    out.contextReadable=read(context,out.contextWords.data(),sizeof(out.contextWords));
    if(!out.exportedReadable || !out.globalReadable || !out.mainReadable || !out.renderReadable)
        return "render_view_unreadable";
    if(!out.contextReadable)return "render_context_unreadable";
    RenderObservation again{};
    if(!identity(again) || again.mainTable!=out.mainTable || again.renderer!=out.renderer ||
       again.rendererTable!=out.rendererTable || again.target!=out.target || again.context!=out.context ||
       again.type!=out.type || again.active!=out.active ||
       DecodeOwner(ownerStack,base,camera,read,again.owner) || again.owner!=out.owner)
        return "render_identity_changed";
    // Stable boundary identities do not make the multi-field copies atomic.
    // Retain the exact words for offline correlation instead of comparing here.
    return nullptr;
}
}
