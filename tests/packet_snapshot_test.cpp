#include "packet_snapshot.h"
#include <cstdio>
int main() {
    std::array<std::array<uint8_t,64>,3> memory{};
    auto link=[&](unsigned i,uint32_t next) { memcpy(memory[i].data(),&next,4); };
    auto read=[&](uint32_t address,void* data,size_t size) {
        if (address<0x1000 || address>=0x10c0 || ((address-0x1000)%64)) return false;
        memcpy(data,memory[(address-0x1000)/64].data(),size); return true;
    };
    link(0,0x1040); memory[1][32]=42;
    const std::array<uint32_t,3> roots{0x1000,0x1080,0};
    const auto good=grid2vr::SnapshotPacketLists(roots,read);
    if (!good.complete || good.nodes.size()!=3 || good.nodes[1].bytes[32]!=42 || good.nodes[2].list!=1) return 1;
    if (grid2vr::SnapshotPacketLists(roots,read,1).complete) return 2;
    link(1,0x1000);
    if (grid2vr::SnapshotPacketLists(roots,read).complete) return 3;
    for (uint32_t bad:{0x1041u,0x1200u,0xfffffff0u}) {
        link(1,bad);
        if (grid2vr::SnapshotPacketLists(roots,read).complete) return 4;
    }
    if (!grid2vr::SnapshotPacketLists(std::array<uint32_t,3>{},read).nodes.empty()) return 5;
    puts("Packet snapshots reject cycles, truncation, unreadable/unaligned links and overflow.");
}
