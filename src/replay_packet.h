#pragma once
#include <array>
#include <cstdint>
#include <cstring>
namespace grid2vr {
using ScenePacket=std::array<uint8_t,0x128>;
inline bool PacketHasLazyLists(const ScenePacket& packet) {
    for (size_t offset:{0x84u,0x88u,0xe4u}) {
        uint32_t head{};memcpy(&head,packet.data()+offset,4);
        if (head) return true;
    }
    return false;
}
// Native main callback 0x7848e0 calls lazy builder 0x63db10 only when
// [base+0xe2fa7c]==4, main+0xe4 is non-null and builder+0x68 is enabled.
struct LazyListGate {
    uint32_t mode{},builder{};
    uint8_t enabled{};
    bool Open() const { return mode==4 && builder && enabled; }
    bool operator==(const LazyListGate&) const = default;
};
template<class Reader> const char* ReadLazyListGate(uintptr_t base,uintptr_t main,Reader read,LazyListGate& gate) {
    gate={};
    if (base>UINT32_MAX-0xe2fa7c || main>UINT32_MAX-0xe8) return "empty_packet_address";
    std::array<uint8_t,31> expected{0x83,0x3d,0,0,0,0,0x04,0x75,0x16,0x8b,0x8e,0xe4,0,0,0,
        0x85,0xc9,0x74,0x0c,0x80,0x79,0x68,0,0x74,0x06,0x57,0xe8,0xe9,0x8f,0xeb,0xff};
    const auto selector=static_cast<uint32_t>(base+0xe2fa7c);
    memcpy(expected.data()+2,&selector,4);
    std::array<uint8_t,31> actual{};
    if (!read(base+0x784b08,actual.data(),actual.size()) || actual!=expected) return "empty_packet_builder_instructions";
    if (!read(selector,&gate.mode,sizeof(gate.mode))) return "empty_packet_builder_read";
    if (gate.mode!=4) return nullptr;
    if (!read(main+0xe4,&gate.builder,sizeof(gate.builder))) return "empty_packet_builder_read";
    if (!gate.builder) return nullptr;
    if (gate.builder>UINT32_MAX-0x69 || !read(gate.builder+0x68,&gate.enabled,sizeof(gate.enabled))) return "empty_packet_builder_read";
    return nullptr;
}

struct LazyListState {
    std::array<uint8_t,0x6c> object{};
    std::array<uint32_t,3> roots{};
    uint8_t auxiliaryPending{}; // (builder+0x70)+0x2390, guarded native enqueue no-op.
    bool operator==(const LazyListState&) const = default;
};
template<class Reader> bool ReadLazyListState(uintptr_t builder,Reader read,LazyListState& state) {
    state={};
    if (!builder || builder>UINT32_MAX-0x2401 || !read(builder,state.object.data(),state.object.size()) ||
        !read(builder+0x2400,&state.auxiliaryPending,1)) return false;
    for (unsigned i=0;i<3;++i) {
        uint32_t tree{};memcpy(&tree,state.object.data()+0x34+i*4,4);
        if (!tree || tree>UINT32_MAX-8 || !read(tree+4,&state.roots[i],4)) return false;
    }
    return true;
}
struct LazyListIdentity {
    uint64_t scene{},frame{};
    uint32_t thread{};
    uintptr_t packet{},builder{};
    bool operator==(const LazyListIdentity&) const = default;
};
// Witness the completed native scene, after its original generator/flatten
// calls have run. Both eyes reuse this exact empty result. No game code is
// patched, and no invented internal call count is used as evidence.
struct EmptyListWitness {
    LazyListIdentity identity{};
    LazyListState state{};
    LazyListGate gate{};
    ScenePacket packet{};
    // Read, nonempty packet, nonempty trees, identity, enabled state, boundary,
    // auxiliary job not already pending (its enabled-byte consumers must no-op).
    unsigned rejectionMask{};
    bool healthy{};
    void Observe(const LazyListIdentity& call,bool completed,bool readable,const ScenePacket& result,
                 const LazyListGate& currentGate,const LazyListState& after) {
        *this={};identity=call;gate=currentGate;state=after;packet=result;
        if(!readable) rejectionMask|=1;
        if(readable && PacketHasLazyLists(result)) rejectionMask|=2;
        if(readable && gate.Open() && after.roots!=std::array<uint32_t,3>{}) rejectionMask|=4;
        if(!call.scene || !call.frame || !call.thread || !call.packet || call.builder!=gate.builder) rejectionMask|=8;
        if(readable && gate.Open() && after.object[0x68]!=gate.enabled) rejectionMask|=16;
        if(!completed) rejectionMask|=32;
        if(readable && gate.Open() && !after.auxiliaryPending) rejectionMask|=64;
        healthy=rejectionMask==0;
    }
    bool Matches(const LazyListIdentity& call,const ScenePacket& currentPacket,const LazyListGate& currentGate,
                 const LazyListState& current) const {
        return healthy && call==identity && currentPacket==packet && currentGate==gate && (!gate.Open() || current==state);
    }
};
class EmptyListReplay {
    bool active{},healthy{},released{},restored{},ownsByte{};
    EmptyListWitness witness{};
public:
    template<class Exchange> bool Begin(const LazyListIdentity& call,const ScenePacket& packet,const LazyListGate& gate,
               const LazyListState& current,const EmptyListWitness& completed,Exchange exchange) {
        if (active) { healthy=false;return false; }
        if (!completed.Matches(call,packet,gate,current)) return false;
        witness=completed;released=false;restored=false;ownsByte=false;
        if(gate.Open()) {
            if(!exchange(call.builder+0x68,gate.enabled,uint8_t(0))) return false;
            ownsByte=true;
        }
        healthy=true;active=true;return true;
    }
    bool Active() const { return active; }
    // Always release before reading the post-replay state, including on an
    // abnormal native return. The exchange callback validates live ownership.
    // A failed release is never retried against a possibly replaced object.
    template<class Exchange> bool Release(Exchange exchange) {
        if(!active) return true;
        if(released) return restored;
        released=true;
        restored=!ownsByte || exchange(witness.identity.builder+0x68,uint8_t(0),witness.gate.enabled);
        ownsByte=false;
        return restored;
    }
    bool Finish(const LazyListIdentity& call,bool readable,const ScenePacket& packet,const LazyListGate& gate,const LazyListState& current) {
        const bool valid=active && healthy && released && restored && readable && witness.Matches(call,packet,gate,current);
        active=false;return valid;
    }
    void Cancel() { active=false;healthy=false; }
};
}
