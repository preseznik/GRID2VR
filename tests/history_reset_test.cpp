#include "replay_state.h"
#include <fstream>
#include <cstdio>
using namespace grid2vr;
int main(int argc,char** argv) {
    if (argc!=2) return 1;
    std::ifstream input(argv[1],std::ios::binary);
    HistoryState entry{},complete{},prepared{};
    SceneView original{},view{};
    if (!input.read(reinterpret_cast<char*>(entry.data()),entry.size()) ||
        !input.read(reinterpret_cast<char*>(complete.data()),complete.size()) ||
        !input.read(reinterpret_cast<char*>(original.data()),original.size()) || input.peek()!=EOF) return 2;
    // Exact failure at frame 4207: only the native validity flag broke the old
    // guard. All identity, counter, secondary-history and view checks still hold.
    if (entry[0x90]!=0 || complete[0x90]!=1 || HistoryWord(entry,0x94)!=4203 ||
        HistoryWord(complete,0x94)!=4204 || !PrepareReplayHistory(entry,complete,original,prepared,view) ||
        prepared[0x90]!=0 || view[0x8c]!=0) return 3;
    auto manager=complete;
    for (unsigned eye=0;eye<2;++eye) {
        ApplyReplayHistorySeed(manager.data(),prepared);
        if (manager[0x90]!=0 || HistoryWord(manager,0xa0)!=1 || HistoryWord(manager,0x94)!=4204 ||
            !HistoryOtherFieldsEqual(manager,complete)) return 4;
        // Model native writes, not native execution: each eye produces its own
        // current matrix, marks it valid, then the original completed state returns.
        memset(manager.data()+0x50,eye?0x22:0x11,0x40); manager[0x90]=1;
        RestoreHistoryWrites(manager.data(),complete);
        if (manager!=complete) return 5;
    }
    for (size_t offset:{size_t(0),size_t(0x50),size_t(0x90),size_t(0x94),size_t(0xa0),size_t(0xb8),size_t(0x1ec)}) {
        auto bad=complete;bad[offset]^=1;
        const auto before=prepared;const auto beforeView=view;
        if (PrepareReplayHistory(entry,bad,original,prepared,view) || prepared!=before || view!=beforeView) return 6;
    }
    for (unsigned flag:{2u,255u}) {
        auto bad=entry;bad[0x90]=static_cast<uint8_t>(flag);
        if (PrepareReplayHistory(bad,complete,original,prepared,view)) return 7;
    }
    auto next=complete;const uint32_t counter=4205;memcpy(next.data()+0x94,&counter,4);
    if (!PrepareReplayHistory(complete,next,original,prepared,view) || prepared[0x90]!=1) return 8;
    ApplyReplayHistorySeed(manager.data(),prepared);
    if (manager[0x90]!=1) return 9; // Ordinary frames retain the existing history path.
    std::puts("Captured native history reset: both eyes reseed, completed state restores, invalid transitions still rejected.");
    return 0;
}
