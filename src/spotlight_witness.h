#pragma once
#include <d3d11.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>

namespace grid2vr::spotlightwitness {
inline constexpr uint64_t SpotlightVs=0xdedcbb651ce31ec3ull;
inline constexpr uint64_t StencilVs=0x2d8af52ce5564fb2ull;
inline constexpr std::array<uint64_t,6> SpotlightPs{
    0x1328844cfb79ab34ull,0xce3893511fabf53dull,0x0bbb6a6010ddd7c4ull,
    0x1f779a4bfb080f9eull,0x13e126614b7cc867ull,0xd9c9b5521a549073ull};
constexpr UINT PixelBufferSize(uint64_t shader) {
    for (size_t i=0;i<SpotlightPs.size();++i) if (SpotlightPs[i]==shader) return i<3?432:464;
    return 0;
}
constexpr bool Selected(uint64_t vs,uint64_t ps) {
    return vs==StencilVs || (vs==SpotlightVs && PixelBufferSize(ps));
}
constexpr bool TracksSize(UINT bytes) {
    return bytes==128 || bytes==192 || bytes==320 || bytes==368 || bytes==432 || bytes==464;
}
// One complete same-scene center/left/right set per known variant/stage.
// Incomplete menu sets roll forward; they cannot consume later eye evidence.
struct Selection {
    struct Group { uint64_t vs{},ps{},scene{};unsigned slot{},mask{};bool missingWritten{},finished{};unsigned attempts{}; };
    static constexpr unsigned StagesPerVariant=4,MaxGroups=(unsigned(SpotlightPs.size())+1)*StagesPerVariant;
    static constexpr unsigned MaxAttempts=32;
    std::array<Group,MaxGroups> groups{};
    Group* Take(uint64_t scene,unsigned pass,unsigned slot,uint64_t vs,uint64_t ps) {
        if (!scene || pass>2 || !Selected(vs,ps)) return nullptr;
        unsigned variant=unsigned(SpotlightPs.size());
        if (vs==StencilVs) ps=0;
        else for(unsigned i=0;i<SpotlightPs.size();++i)if(ps==SpotlightPs[i])variant=i;
        Group* empty=nullptr;
        for (unsigned i=variant*StagesPerVariant;i<(variant+1)*StagesPerVariant;++i) {
            auto& group=groups[i];
            if (!group.scene) {if(!empty)empty=&group;continue;}
            if (group.vs!=vs || group.ps!=ps || group.slot!=slot) continue;
            if (group.finished) return nullptr;
            if (group.scene!=scene) {
                if(pass!=0)return nullptr;
                group.scene=scene;group.mask=0;
            }
            if (group.mask&(1u<<pass)) return nullptr;
            group.mask|=1u<<pass;return &group;
        }
        if (!empty || pass!=0) return nullptr;
        *empty={vs,ps,scene,slot,1};return empty;
    }
    static bool Finish(Group& group,bool uploadsPresent) {
        if(group.mask!=7 || group.finished)return false;
        if(uploadsPresent){group.finished=true;return true;}
        if(++group.attempts>=MaxAttempts)group.finished=true;
        if(group.missingWritten)return false;
        group.missingWritten=true;return true;
    }
};
void Configure(const std::filesystem::path& folder,bool enabled);
bool Enabled();
// Copies CPU upload bytes only; no GPU reads, writes or binding changes.
void Upload(ID3D11Resource* resource,const void* data,UINT bytes);
void Draw(ID3D11DeviceContext* context,uint64_t scene,unsigned pass,unsigned slot,
          unsigned ordinal,const char* kind,std::span<const UINT> args,uint64_t vs,uint64_t ps);
}
