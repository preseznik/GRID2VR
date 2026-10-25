#pragma once
#include <d3d11.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>

namespace grid2vr::instancecapacity {
struct BufferEvidence {
    uintptr_t buffer{};
    uint32_t requested{},actual{},bind{},usage{},cpuAccess{};
    HRESULT result{};
    unsigned observations{1};
};

// Metadata follows the D3D object's lifetime, not an append-only capture prefix.
// The tag never owns a reference to the GPU buffer and lookup never calls through
// a pointer obtained from game memory. A missing/ambiguous owner cannot qualify.
class BufferRegistry {
    class Lifetime;
    struct Entry { BufferEvidence evidence; Lifetime* lifetime{}; };
    struct State {
        std::mutex mutex;
        std::unordered_map<uintptr_t,Entry> entries;
        size_t peak{};
        uint64_t failures{};
    };
    class Lifetime final : public IUnknown {
        std::atomic<ULONG> references_{1};
        std::shared_ptr<State> state_;
        uintptr_t buffer_;
    public:
        Lifetime(std::shared_ptr<State> state,uintptr_t buffer):state_(std::move(state)),buffer_(buffer){}
        ~Lifetime() {
            std::lock_guard lock(state_->mutex);
            const auto entry=state_->entries.find(buffer_);
            if(entry!=state_->entries.end() && entry->second.lifetime==this)state_->entries.erase(entry);
        }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** output) override {
            if(!output)return E_POINTER;
            *output=nullptr;
            if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
            *output=static_cast<IUnknown*>(this);AddRef();return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
        ULONG STDMETHODCALLTYPE Release() override {
            const auto remaining=--references_;if(!remaining)delete this;return remaining;
        }
    };
    std::shared_ptr<State> state_=std::make_shared<State>();
    static constexpr GUID lifetimeId{0x617b2ee4,0xd39e,0x4cc1,{0x98,0xb5,0x81,0x64,0x40,0x73,0x05,0xc6}};
public:
    // Independent memory bound. Reaching it never invalidates tracked owners;
    // an untracked required lane still fails the ordinary ownership checks.
    static constexpr size_t MaxLiveBuffers=8192;
    struct Statistics {size_t live{},peak{};uint64_t failures{};};
    void Reset(){state_=std::make_shared<State>();}
    bool Observe(ID3D11Buffer* buffer,const BufferEvidence& evidence) {
        if(!buffer || evidence.buffer!=reinterpret_cast<uintptr_t>(buffer))return false;
        auto* lifetime=new(std::nothrow) Lifetime(state_,evidence.buffer);
        bool registered=false;
        {
            std::lock_guard lock(state_->mutex);
            const auto previous=state_->entries.find(evidence.buffer);
            if(previous!=state_->entries.end())previous->second.evidence.observations=2;
            else if(lifetime && state_->entries.size()<MaxLiveBuffers) {
                try {
                    state_->entries.emplace(evidence.buffer,Entry{evidence,lifetime});
                    state_->peak=std::max(state_->peak,state_->entries.size());registered=true;
                } catch(const std::bad_alloc&) {}
            }
            if(!registered)++state_->failures;
        }
        // D3D releases the tag when the buffer dies. Do not hold our mutex while
        // setting/releasing private data: replacing an old tag can call Release.
        const auto attached=registered?buffer->SetPrivateDataInterface(lifetimeId,lifetime):E_FAIL;
        if(registered && FAILED(attached)) {
            std::lock_guard lock(state_->mutex);++state_->failures;
            state_->entries.erase(evidence.buffer);
        }
        if(lifetime)lifetime->Release();
        return SUCCEEDED(attached);
    }
    bool Find(uintptr_t buffer,BufferEvidence& output) const {
        std::lock_guard lock(state_->mutex);
        const auto entry=state_->entries.find(buffer);
        if(entry==state_->entries.end())return false;
        output=entry->second.evidence;return true;
    }
    Statistics Stats() const {
        std::lock_guard lock(state_->mutex);
        return {state_->entries.size(),state_->peak,state_->failures};
    }
};
}
