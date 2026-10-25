#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

namespace grid2vr {
// Only the next link at +0 is established for these flattened packet lists.
// The following bytes are a bounded observation window, not a typed object.
struct PacketNode { uint32_t list,address; std::array<uint8_t,64> bytes; };
struct PacketSnapshot { std::vector<PacketNode> nodes; bool complete=true; };
template<class Reader> PacketSnapshot SnapshotPacketLists(const std::array<uint32_t,3>& roots,Reader read,size_t limit=4096) {
    PacketSnapshot result;
    for (uint32_t list=0;list<roots.size();++list) {
        std::set<uint32_t> visited;
        auto address=roots[list];
        while (address) {
            if (address>UINT32_MAX-64 || (address&3) || result.nodes.size()>=limit || !visited.insert(address).second) {
                result.complete=false; return result;
            }
            PacketNode node{list,address,{}};
            if (!read(address,node.bytes.data(),node.bytes.size())) { result.complete=false; return result; }
            result.nodes.push_back(node);
            memcpy(&address,node.bytes.data(),4);
        }
    }
    return result;
}
}
