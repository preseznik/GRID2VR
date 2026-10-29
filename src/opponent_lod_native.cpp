#include "opponent_lod_native.h"
#include "game_executable.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace grid2vr::opponentlod::native {
namespace {
constexpr uint32_t callbackRelocs[]{0x17,0x3d,0x9a,0xa2,0xaa,0xb2,0xba,0xc2,0xe4,0xeb,0x129,0x14d,0x191,0x1a3,0x257,0x318,0x326,0x334,0x358};
constexpr Call callbackCalls[]{
    {0x2e,0x5d2500},{0x168,0x45e4d0},{0x1b2,0x45e4d0},{0x1cc,0x7348d0},
    {0x1e4,0x27c140},{0x215,0x804780},{0x228,0x45e4d0},{0x266,0x408a40},
    {0x273,0x56dd30},{0x283,0x56dd30},{0x29b,0x6de9b0},{0x2ca,0x804780},
    {0x2d8,0x804780},{0x30b,0x804780},{0x32a,0x3c0340},{0x340,0x3572f0}};
constexpr uint32_t builderRelocs[]{0x39,0x81,0x109,0x110,0x12c,0x13a,0x172,0x1a6,0x1ae,0x1b6,0x1be,0x1ce,0x1f0,0x1f8,0x242,0x24a,0x252,0x262,0x293,0x2b6,0x2be,0x2ca,0x2d2,0x354,0x53a,0x544,0x54a,0x552,0x55a,0x57b,0x742};
constexpr Call builderCalls[]{
    {0x26,0x18aee0},{0x49,0x59d200},{0x92,0x932ea0},{0xe9,0x45e4d0},
    {0x130,0x6eafd0},{0x14a,0x559020},{0x183,0x6de970},{0x194,0x559020},
    {0x5f3,0x6b6620},{0x774,0x4eb620}};
constexpr Call wrapperCalls[]{{0x11,0x169050}};
constexpr uint32_t selectorRelocs[]{0xe1,0x167,0x229,0x231,0x259,0x273,0x27b,0x28c,0x2a6,0x2cf,0x2ee,0x321,0x3f8,0x4ce,0x4db,0x4ec,0x4f9,0x520,0x530,0x538,0x542,0x54e,0x556,0x55e,0x56a,0x572,0x57a,0x584,0x591,0x5b5,0x5d1,0x5f0,0x659,0x668};
constexpr Call selectorCalls[]{
    {0x17a,0x87d860},{0x424,0x47c700},{0x465,0x69a060},{0x4d2,0xac6898},
    {0x4f0,0xac68c0},{0x51a,0x81ee40},{0x8b8,0x17e710},{0x92f,0x6a260}};
constexpr uint32_t updaterRelocs[]{0x29,0x7e,0x98,0xb1,0xd7,0xfe,0x118,0x2cc,0x2dd,0x2f6,0x30c,0x311,0x316,0x561,0x5bc,0x5c1,0x5cc,0x5dd,0x5ff,0x609,0x614,0x62e,0x646,0x675,0x6b2,0x6bf,0x6d5,0x6e8,0x6f0,0x79a,0x7dd,0x7f0,0x7ff,0x82b,0x836,0x896,0x8a8,0x8be,0x8dd,0x8f9,0x91c,0x924,0x92c,0x934};
constexpr Call updaterCalls[]{
    {0x36,0x1f4070},{0x4b,0x6eb2a0},{0x84,0x500300},{0x8b,0x444f00},
    {0x92,0x5a9bd0},{0x9c,0x56f6d0},{0xbe,0x7c940},{0xf1,0x59d200},
    {0x157,0x6b6620},{0x179,0x48d940},{0x29c,0x87d860},{0x2a5,0x2f7f10},
    {0x303,0x6b6620},{0x32a,0x48d940},{0x450,0x87d860},{0x45c,0x2f7f10},
    {0x48b,0x2f7f10},{0x4ba,0x2f7f10},{0x4ed,0x2f7f10},{0x52b,0x2f7f10},
    {0x56e,0x1ee660},{0x576,0x5d2500},{0x58d,0x3f6bd0},{0x594,0x3c0340},
    {0x59c,0x3f6bd0},{0x5a7,0x3572f0},{0x5af,0x5dade0},{0x5b6,0x15abb0},
    {0x5c5,0x69de20},{0x5d6,0x647e90},{0x5e1,0x79d260},{0x603,0x4d950},
    {0x60d,0x5bf260},{0x663,0x7e73a0},{0x692,0x7e73a0},{0x6b6,0x359af0},
    {0x762,0x7348d0},{0x782,0x8d0380},{0x7a2,0x52a820},{0x7e9,0x28bca0},
    {0x7f4,0x6eafd0},{0x804,0x3fe000},{0x819,0x4a920},{0x825,0x4a920},
    {0x82f,0x22cf70},{0x89a,0x7ef310},{0x8b0,0xd250},{0x8c2,0xaacb0},
    {0x8d1,0x112b10},{0x8e1,0x2a810},{0x8f3,0x2971d0},{0x8fd,0x18cd10},
    {0x911,0x7c3160},{0x94d,0x42520},{0x958,0x278e00}};
constexpr std::array<BodyGuard,BodyCount> bodies{{
    {"callback",0x2e3930,0x367,"9e17bb31fa7d015cd1760818e2d82970c98be9937eea4d236cb65b2250c291fc",callbackRelocs,callbackCalls},
    {"builder",0x3572f0,0x784,"f3881df7e08293a7c7b140d76c2330534c2c7c994269988e7f50e9fe1080bd3e",builderRelocs,builderCalls},
    {"wrapper",0x4eb620,0x1a,"a9914f9c1881968ee3167ee6b5c345039610121def01d0082a2cda6e973a17bf",{},wrapperCalls},
    {"selector",0x169050,0x972,"7ea7851121f2c681010d885326d8e953603ef8bd18246cf0ab54532fe3a4cfc2",selectorRelocs,selectorCalls},
    {"updater",0x76e580,0x968,"70529ce0979087ef012db43ce1d1fdb2b1a621fea33eb95e716022f84039a4a5",updaterRelocs,updaterCalls}}};
constexpr std::size_t MaxBodyBytes=0x972;
constexpr uint32_t ImageExtent=0x1000000; // Conservative address-overflow/overlap bound, not a PE identity guard.
uint32_t Load32(const uint8_t* at) noexcept {uint32_t value{};std::memcpy(&value,at,4);return value;}
void Store32(uint8_t* at,uint32_t value) noexcept {std::memcpy(at,&value,4);}
bool Range(uint32_t at,std::size_t size) noexcept {
    return at>=0x10000 && size && size<=std::numeric_limits<uint32_t>::max()-at;
}
bool Overlap(uintptr_t a,std::size_t an,uintptr_t b,std::size_t bn) noexcept {
    return a<=b?b-a<an:a-b<bn;
}
StageResult Refuse(Failure failure) noexcept {StageResult result;result.failure=failure;return result;}

StageResult Stage(std::span<const BodyGuard,BodyCount> guards,const BodyInputs& inputs,
    uint32_t imageBase,uint32_t copyBase,uint32_t helper,std::span<uint8_t> destination) {
    if(!Range(imageBase,ImageExtent) || (imageBase&0xffff) ||
       !Range(copyBase,StagingBytes) || (copyBase&15) || !Range(helper,1) ||
       Overlap(imageBase,ImageExtent,copyBase,StagingBytes) ||
       Overlap(copyBase,StagingBytes,helper,1))return Refuse(Failure::InvalidAddress);
    if(destination.size()<StagingBytes)return Refuse(Failure::Capacity);
    StageResult result;std::size_t cursor{};
    for(std::size_t i=0;i<BodyCount;++i) {
        const auto& guard=guards[i];
        // Shape and original RVAs are never configurable, including in fixtures.
        const auto& expected=bodies[i];
        if(guard.rva!=expected.rva || guard.size!=expected.size || guard.sha256.size()!=64 ||
           guard.highlow.size()!=expected.highlow.size() || guard.calls.size()!=expected.calls.size() ||
           !std::equal(guard.highlow.begin(),guard.highlow.end(),expected.highlow.begin()) ||
           !std::equal(guard.calls.begin(),guard.calls.end(),expected.calls.begin(),
               [](Call a,Call b){return a.offset==b.offset && a.targetRva==b.targetRva;}))
            return Refuse(Failure::InvalidManifest);
        if(inputs[i].size()!=guard.size)return Refuse(Failure::InputSize);
        if(Overlap(reinterpret_cast<uintptr_t>(destination.data()),destination.size(),
            reinterpret_cast<uintptr_t>(inputs[i].data()),inputs[i].size()))return Refuse(Failure::AliasedInput);
        cursor=(cursor+15)&~std::size_t(15);result.copies[i]=copyBase+static_cast<uint32_t>(cursor);cursor+=guard.size;
    }
    cursor=(cursor+15)&~std::size_t(15);result.adapter=copyBase+static_cast<uint32_t>(cursor);cursor+=AdapterBytes;
    if(cursor!=StagingBytes)return Refuse(Failure::InvalidManifest);
    std::array<uint8_t,StagingBytes> staged;staged.fill(0xcc);
    for(std::size_t i=0;i<BodyCount;++i) {
        const auto& guard=guards[i];const auto input=inputs[i];
        std::array<bool,MaxBodyBytes> operands{};
        std::array<uint8_t,MaxBodyBytes> normalized{};
        std::copy(input.begin(),input.end(),normalized.begin());
        const auto claim=[&](uint32_t at,uint32_t size) {
            if(at>guard.size || size>guard.size-at)return false;
            for(uint32_t j=at;j<at+size;++j)if(operands[j])return false;
            for(uint32_t j=at;j<at+size;++j)operands[j]=true;
            return true;
        };
        for(const auto offset:guard.highlow) {
            if(!claim(offset,4))return Refuse(Failure::InvalidManifest);
            Store32(normalized.data()+offset,Load32(input.data()+offset)-(imageBase-PreferredImageBase));
        }
        for(const auto call:guard.calls) {
            if(!claim(call.offset,5) || call.targetRva>=ImageExtent)return Refuse(Failure::InvalidManifest);
            if(input[call.offset]!=0xe8 ||
               imageBase+guard.rva+call.offset+5+Load32(input.data()+call.offset+1)!=imageBase+call.targetRva)
                return Refuse(Failure::CallOperand);
        }
        const auto digest=gameexe::Digest(normalized.data(),guard.size);
        if(digest.empty())return Refuse(Failure::HashUnavailable);
        if(digest!=guard.sha256)return Refuse(Failure::BodyHash);
        auto output=staged.data()+(result.copies[i]-copyBase);
        std::copy(input.begin(),input.end(),output);
        for(const auto call:guard.calls) {
            uint32_t target=imageBase+call.targetRva;
            // Redirect only the five audited semantic edges, never every call
            // whose destination happens to equal another copied entry point.
            if(i==0 && call.offset==0x340)target=result.copies[1];
            if(i==1 && call.offset==0x774)target=result.copies[2];
            if(i==2 && call.offset==0x11)target=result.copies[3];
            if(i==3 && call.offset==0x92f)target=result.adapter;
            if(i==4 && call.offset==0x5a7)target=result.copies[1];
            // x86 rel32 arithmetic is modulo 2^32, including layouts across the
            // signed-2GB boundary. Do not do signed C++ subtraction here.
            Store32(output+call.offset+1,target-(result.copies[i]+call.offset+5));
        }
    }
    const auto failure=BuildAdapter(result.adapter,helper,imageBase+ApplyRva,
        std::span(staged).subspan(result.adapter-copyBase,AdapterBytes));
    if(failure!=Failure::None)return Refuse(failure);
    std::copy(staged.begin(),staged.end(),destination.begin());
    result.used=cursor;result.failure=Failure::None;return result;
}
}

std::span<const BodyGuard,BodyCount> Guards() noexcept {return bodies;}
Failure BuildAdapter(uint32_t address,uint32_t helper,uint32_t apply,std::span<uint8_t> destination) noexcept {
    if(!Range(address,AdapterBytes) || !Range(helper,1) || !Range(apply,1) ||
       Overlap(address,AdapterBytes,helper,1) || Overlap(address,AdapterBytes,apply,1))return Failure::InvalidAddress;
    if(destination.size()<AdapterBytes)return Failure::Capacity;
    std::array<uint8_t,AdapterBytes> bytes{0x51,0xff,0x73,0x14,0xff,0x73,0x10,0xff,0x73,0x0c,0xff,0x73,0x08,
        0xe8,0,0,0,0,0x59,0xe9,0,0,0,0};
    Store32(bytes.data()+0x0e,helper-(address+0x12));
    Store32(bytes.data()+0x14,apply-(address+0x18));
    std::copy(bytes.begin(),bytes.end(),destination.begin());return Failure::None;
}
StageResult StageCopies(const BodyInputs& inputs,uint32_t imageBase,uint32_t copyBase,
    uint32_t helper,std::span<uint8_t> destination) noexcept {
    try {return Stage(bodies,inputs,imageBase,copyBase,helper,destination);}
    catch(...) {return Refuse(Failure::HashUnavailable);}
}
Failure Install() noexcept {return Failure::NotQualified;}
#ifdef GRID2VR_OPPONENT_LOD_NATIVE_TEST
namespace testing {
StageResult StageCopies(std::span<const BodyGuard,BodyCount> guards,const BodyInputs& inputs,
    uint32_t imageBase,uint32_t copyBase,uint32_t helper,std::span<uint8_t> destination) noexcept {
    try {return Stage(guards,inputs,imageBase,copyBase,helper,destination);}
    catch(...) {return Refuse(Failure::HashUnavailable);}
}
}
#endif
}
