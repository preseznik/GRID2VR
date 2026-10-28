#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>

namespace grid2vr::projectedlights {
inline constexpr uintptr_t ProducerRva=0x3ddd90,LightTableRva=0xd0d068;
using Camera=std::array<uint8_t,512>;
struct alignas(16) Call {
    uintptr_t self{},light{},context{},material{};unsigned index{};
    bool supported{};
    alignas(16) Camera camera{};
    // Native input pointers and manager/transform data must stay fixed while
    // replay changes only the camera and the producer's output properties.
    std::array<uintptr_t,11> pointers{};
    std::array<uint32_t,8> inputs{};
    std::array<uint8_t,64> manager{},transform{};
};
class PreparedCalls {
    static constexpr unsigned Capacity=512;
    std::array<Call,Capacity> calls_{};
    std::array<unsigned,Capacity> selected_{};
    uint64_t frame_{};uintptr_t context_{};
    unsigned count_{},selectedCount_{},applies_{};
    bool overflow_{},sealed_{},dirty_{};
public:
    bool Begin(uint64_t frame) {
        if(dirty_)return false;
        frame_=frame;count_=selectedCount_=applies_=0;context_=0;overflow_=sealed_=false;return true;
    }
    void Remember(uint64_t frame,const Call& call) {
        if(frame!=frame_ || sealed_ || count_==Capacity){overflow_=true;return;}
        calls_[count_++]=call;
    }
    bool Snapshot(uint64_t frame,uintptr_t context,const Camera& camera) {
        if(frame!=frame_ || !context || overflow_ || sealed_ || dirty_)return false;
        selectedCount_=0;
        for(unsigned i=0;i<count_;++i) {
            const auto& call=calls_[i];if(call.context!=context)continue;
            if(!call.supported || memcmp(call.camera.data()+0x70,camera.data()+0x70,64) ||
                memcmp(call.camera.data()+0x150,camera.data()+0x150,64))return false;
            selected_[selectedCount_++]=i;
        }
        context_=context;sealed_=true;return true;
    }
    template<class Validate,class Invoke>
    bool Apply(uint64_t frame,uintptr_t context,Validate validate,Invoke invoke) {
        if(frame!=frame_ || context!=context_ || !sealed_ || overflow_ || applies_>=2)return false;
        for(unsigned i=0;i<selectedCount_;++i)if(!validate(calls_[selected_[i]]))return false;
        // Set before calling native code so every refusal after this boundary
        // still requires restoration, including after only the first eye.
        dirty_=selectedCount_!=0;++applies_;
        for(unsigned i=0;i<selectedCount_;++i)invoke(calls_[selected_[i]],context);
        return true;
    }
    template<class Validate,class Invoke>
    bool Restore(uint64_t frame,uintptr_t context,Validate validate,Invoke invoke) {
        if(frame!=frame_ || context!=context_ || !sealed_)return false;
        if(!dirty_)return true;
        for(unsigned i=0;i<selectedCount_;++i)if(!validate(calls_[selected_[i]]))return false;
        for(unsigned i=0;i<selectedCount_;++i) {
            const auto& call=calls_[selected_[i]];
            invoke(call,reinterpret_cast<uintptr_t>(call.camera.data()));
        }
        dirty_=false;return true;
    }
    bool Clear(){if(dirty_)return false;return Begin(frame_);}
    bool Dirty()const{return dirty_;}
    unsigned Count()const{return selectedCount_;}
};
// Install is called only after the owning engine has verified the executable
// fingerprint and initialized MinHook. BeginFrame precedes native preparation.
bool Install(uintptr_t base,const std::filesystem::path& folder);
bool BeginFrame(uint64_t frame);
bool Snapshot(uint64_t frame,uintptr_t context,const Camera& original);
bool Apply(uint64_t frame,uintptr_t context);
// Uses each saved producer camera, not the completed near-object camera.
bool Restore(uint64_t frame,uintptr_t originalContext);
bool Clear();
}
