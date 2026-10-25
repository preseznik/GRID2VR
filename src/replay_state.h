#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace grid2vr {
using HistoryState=std::array<uint8_t,0x200>;
using SceneView=std::array<uint8_t,0x90>;
struct HistoryRange { size_t offset,size; };
// Writes in AVX history update 0x2e69c0 and index setter 0x5a52e0.
// Preserve the manager identity: embedded shader-property nodes point into it.
constexpr HistoryRange historyWrites[]{
    {4,4},{0x10,0x80},{0x90,1},{0x94,4},{0xa0,4},{0xd0,0x40},
    {0x124,0xc},{0x160,0x10},{0x190,0x10},{0x1c0,0x10},{0x1ec,2}};
inline uint32_t HistoryWord(const HistoryState& state,size_t offset) {
    uint32_t value{}; memcpy(&value,state.data()+offset,4); return value;
}
inline bool HistoryOtherFieldsEqual(const HistoryState& a,const HistoryState& b) {
    for (size_t i=0;i<a.size();++i) {
        bool owned=false;
        for (auto range:historyWrites) if (i>=range.offset && i<range.offset+range.size) owned=true;
        if (!owned && a[i]!=b[i]) return false;
    }
    return true;
}
inline bool PrepareReplayHistory(const HistoryState& entry,const HistoryState& complete,
                                 const SceneView& original,HistoryState& prepared,SceneView& view) {
    // Native 0x2e6abb branches on this validity byte. With zero it seeds the
    // selected history from the current camera, then sets the byte to one.
    if (entry[0x90]>1 || complete[0x90]!=1 || original[0x8c]!=1 ||
        entry[0x1ec]!=0 || complete[0x1ec]!=0 ||
        HistoryWord(entry,0xa0)!=0 || HistoryWord(complete,0xa0)!=0 ||
        HistoryWord(complete,0x94)!=HistoryWord(entry,0x94)+1u ||
        memcmp(entry.data()+0x50,complete.data()+0x50,0x40) ||
        !HistoryOtherFieldsEqual(entry,complete)) return false;
    prepared=complete;
    prepared[0x90]=entry[0x90]; // Each eye must take the same native reset path.
    memcpy(prepared.data()+0x50,entry.data()+0x10,0x40);
    const uint32_t second=1; memcpy(prepared.data()+0xa0,&second,4);
    prepared[0x1ed]=entry[0x1ec];
    view=original; view[0x8c]=0; // Native second-view convention: no shared counter increment.
    return true;
}
inline void ApplyReplayHistorySeed(void* destination,const HistoryState& prepared) {
    auto* bytes=static_cast<uint8_t*>(destination);
    memcpy(bytes+0x50,prepared.data()+0x50,0x40);
    memcpy(bytes+0xa0,prepared.data()+0xa0,4);
    bytes[0x90]=prepared[0x90];
    bytes[0x1ed]=prepared[0x1ed];
}
inline void RestoreHistoryWrites(void* destination,const HistoryState& saved) {
    auto* bytes=static_cast<uint8_t*>(destination);
    for (auto range:historyWrites) memcpy(bytes+range.offset,saved.data()+range.offset,range.size);
}
}
