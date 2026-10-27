#include "replay_state.h"
#include "replay_camera.h"
#include "replay_sequence.h"
#include "scene_views.h"
#include <cstdio>
#include <fstream>
#include <filesystem>
int main(int argc,char** argv) {
    grid2vr::ClipTransitionTrigger trigger;
    trigger.Observe(1,0x1000,0x3dcccccdu,0x466a6000u);
    trigger.Observe(2,0x1000,0x3e4ccccdu,0x42c80000u);
    if (trigger.ready) return 20;
    trigger.Observe(3,0x1000,0x3dcccccdu,0x466a6000u);
    trigger.Observe(3,0x2000,0x3e4ccccdu,0x42c80000u);
    if (trigger.ready) return 21;
    trigger.Observe(4,0x1000,0x3dcccccdu,0x466a6000u);
    trigger.Observe(4,0x1000,0x3e4ccccdu,0x42c80000u);
    if (!trigger.ready) return 22;
    trigger.Observe(5,0x2000,0,0);
    if (!trigger.ready) return 23;
    trigger={};
    trigger.Observe(6,0x1000,0x3e4ccccdu,0x466a6000u);
    trigger.Observe(6,0x1000,0x3e4ccccdu,0x42c80000u);
    if (!trigger.ready) return 30;
    grid2vr::CameraContext camera{},finished{};
    const uint32_t nearMain=0x3dcccccdu,farMain=0x466a6000u,nearSub=0x3e4ccccdu,farSub=0x42c80000u;
    memcpy(camera.data()+0xe8,&nearMain,4); memcpy(camera.data()+0xec,&farMain,4);
    finished=camera;
    if (!grid2vr::ReplayCameraSupported(camera,finished,nearMain,nearMain,farMain)) return 10;
    if (!grid2vr::ReplayCameraSupported(camera,finished,nearMain,nearMain,farMain,false)) return 31;
    memcpy(finished.data()+0xe8,&nearSub,4); memcpy(finished.data()+0xec,&farSub,4);
    for (auto offset:grid2vr::clipWords) if (offset>0xec) finished[offset]=1;
    if (!grid2vr::ReplayCameraSupported(camera,finished,nearMain,nearSub,farMain)) return 11;
    if (grid2vr::ReplayCameraSupported(camera,finished,nearMain,nearSub,farMain,false)) return 32;
    for (size_t i=0;i<camera.size();++i) {
        bool owned=false;
        for (auto offset:grid2vr::clipWords) if (i>=offset && i<offset+4) owned=true;
        if (owned) continue;
        auto bad=finished; bad[i]^=1;
        if (grid2vr::ReplayCameraSupported(camera,bad,nearMain,nearSub,farMain)) return 12;
        grid2vr::RestoreClipWords(bad.data(),camera);
        if (bad[i]==camera[i]) return 13; // Selective restore never hides an unrelated write.
    }
    auto seeded=finished; grid2vr::RestoreClipWords(seeded.data(),camera);
    if (seeded!=camera) return 14;
    grid2vr::RestoreClipWords(seeded.data(),finished);
    if (seeded!=finished) return 15;
    if (grid2vr::ReplayCameraSupported(camera,finished,nearSub,nearSub,farMain) ||
        grid2vr::ReplayCameraSupported(camera,finished,nearMain,nearMain,farMain) ||
        grid2vr::ReplayCameraSupported(camera,finished,nearMain,nearSub,farSub)) return 16;
    const uint32_t nan=0x7fc00000;
    memcpy(finished.data()+0x138,&nan,4);
    if (grid2vr::ReplayCameraSupported(camera,finished,nearMain,nearSub,farMain)) return 17;
    // Exact native far-only transition: scene 316 of the tracked XR retry.
    if(argc!=3) return 24;
    std::ifstream beforeFile(argv[1],std::ios::binary),afterFile(argv[2],std::ios::binary);
    grid2vr::CameraContext nativeBefore{},nativeAfter{};
    if(!beforeFile.read(reinterpret_cast<char*>(nativeBefore.data()),512) || beforeFile.peek()!=EOF ||
        !afterFile.read(reinterpret_cast<char*>(nativeAfter.data()),512) || afterFile.peek()!=EOF ||
        !grid2vr::ReplayCameraSupported(nativeBefore,nativeAfter,nearSub,nearSub,farMain) ||
        grid2vr::CameraClipEqual(nativeBefore,nativeAfter)) return 25;
    if (grid2vr::ReplayCameraSupported(nativeBefore,nativeAfter,nearSub,nearSub,farMain,false)) return 33;
    auto nativeSeed=nativeAfter;grid2vr::RestoreClipWords(nativeSeed.data(),nativeBefore);
    if(nativeSeed!=nativeBefore) return 26;
    grid2vr::RestoreClipWords(nativeSeed.data(),nativeAfter);
    if(nativeSeed!=nativeAfter || !grid2vr::CameraClipEqual(nativeSeed,nativeAfter)) return 27;
    auto invalid=nativeAfter;invalid[0x150]^=1;
    if(grid2vr::ReplayCameraSupported(nativeBefore,invalid,nearSub,nearSub,farMain)) return 28;
    invalid=nativeAfter;memcpy(invalid.data()+0xec,&farMain,4);
    if(grid2vr::ReplayCameraSupported(nativeBefore,invalid,nearSub,nearSub,farMain)) return 29;
    // Exact native menu inputs from the retained user runs. They only
    // authorize cancellation before replay, not relaxing the headset guard.
    // Sample 2: diagnostic-20261024-132135-387, frame 3789, native far 4900.
    // Sample 3: diagnostic-20261024-145553-087, frame 7677, replay navigation;
    // near .2 rounded up one ULP, world far 2500, fixed .2/100 destination.
    // Sample 5: 20261027-193011-656, frame 4523, chase replay navigation;
    // near .11, world far 15000, the same fixed .2/100 destination.
    const auto fixtures=std::filesystem::path(argv[1]).parent_path();
    for (unsigned sample=0;sample<6;++sample) {
        grid2vr::CameraContext entry{},done{};
        const auto prefix=sample==5?std::string("native-replay-menu-4523"):
            sample==3?std::string("native-replay-menu-7677"):"native-pause-"+std::to_string(sample==4?0:sample);
        std::ifstream before(fixtures/(prefix+"-entry.bin"),std::ios::binary);
        std::ifstream after(fixtures/(prefix+"-complete.bin"),std::ios::binary);
        if(!before.read(reinterpret_cast<char*>(entry.data()),entry.size()) || !after.read(reinterpret_cast<char*>(done.data()),done.size())) return 34;
        const uint32_t near=sample==5?0x3de147aeu:sample==4?0x3ca3d70au:sample==3?0x3e4cccceu:sample?nearSub:nearMain,
            far=sample==5?farMain:sample==3?0x451c4000u:sample==2?0x45992000u:0x469c4000u;
        // Synthetic cockpit clip variant of the native pause fixture. This
        // tests classification/byte guards, not a new live pause witness.
        if(sample==4) memcpy(entry.data()+0xe8,&near,4);
        if(!grid2vr::MenuClipTransition(entry,done,near,nearSub,far) ||
            grid2vr::ReplayCameraSupported(entry,done,near,nearSub,far,false) ||
            grid2vr::MenuClipTransition(entry,entry,near,near,far)) return 35;
        if(!grid2vr::MenuCamera(entry,done,near,nearSub,far) || !grid2vr::MenuCamera(done,done,nearSub,nearSub,farSub) ||
            !grid2vr::MenuCamera(done,done,nearSub,nearSub,far) ||
            grid2vr::MenuCamera(entry,entry,near,near,far) ||
            !grid2vr::ReplayCameraSupported(entry,entry,near,near,far,false)) return 38;
        for(size_t i=0;i<entry.size();++i) {
            bool clip=false;for(auto offset:grid2vr::clipWords)if(i>=offset && i<offset+4)clip=true;
            if(clip)continue;
            auto changed=done;changed[i]^=1;
            if(grid2vr::MenuClipTransition(entry,changed,near,nearSub,far)) return 36;
        }
        auto invalid=done;memcpy(invalid.data()+0x138,&nan,4);
        if(grid2vr::MenuClipTransition(entry,invalid,near,nearSub,far) ||
            grid2vr::MenuCamera(invalid,invalid,nearSub,nearSub,far)) return 37;
        // Malformed/short source planes cannot impersonate a world-to-menu
        // transition. A route-independent classifier must still reject them.
        for(uint32_t badFar:{0u,0xbf800000u,0x42c60000u,farSub,0x7f800000u,nan}) {
            auto badEntry=entry;memcpy(badEntry.data()+0xec,&badFar,4);
            if(grid2vr::MenuClipTransition(badEntry,done,near,nearSub,badFar) ||
                (badFar!=farSub && grid2vr::MenuCamera(done,done,nearSub,nearSub,badFar))) return 39;
        }
        if(grid2vr::MenuCamera(entry,done,near,nearSub,farSub) ||
            grid2vr::MenuCamera(done,done,nearMain,nearMain,far)) return 40;
        for(uint32_t badNear:{0u,0xbf800000u,0x3ca3d709u,0x3ca3d70bu,0x3de147adu,0x3de147afu,0x3e4cccccu,0x3e4ccccfu,0x7f800000u,nan}) {
            auto badEntry=entry;memcpy(badEntry.data()+0xe8,&badNear,4);
            if(grid2vr::MenuClipTransition(badEntry,done,badNear,nearSub,far)) return 41;
        }
        if((sample==3 || sample==5) && grid2vr::ReplayCameraSupported(entry,done,near,nearSub,far,true)) return 42;
        if(sample==5) {
            // Captured transition -> stable replay camera -> transition back
            // to driving. Cancel before Begin so no failed pair poisons XR.
            grid2vr::ReplaySequence session;
            grid2vr::SceneViewFrame frozen;
            grid2vr::SceneViews eyes{{grid2vr::ScenePose{},grid2vr::ScenePose{}},
                {grid2vr::diagnosticFrustum,grid2vr::diagnosticFrustum}};
            if(!session.ConfigureSession() || !session.Resume() ||
                !frozen.Freeze(4522,eyes) || !session.Begin(4522)) return 43;
            session.Finish(true);
            if(!frozen.Finish(4522,true)) return 44;
            for(uint64_t frame:{4523u,5001u}) {
                if(!frozen.Freeze(frame,eyes) ||
                    !grid2vr::MenuCamera(entry,done,near,nearSub,far) ||
                    !frozen.CancelBeforeReplay(frame) || !session.Suspend() ||
                    !frozen.ResetEpoch() || frozen.Get(frame)) return 45;
                const auto next=frame==4523?5000u:6000u;
                if(grid2vr::MenuCamera(entry,entry,near,near,far) ||
                    !grid2vr::ReplayCameraSupported(entry,entry,near,near,far,false) ||
                    !session.Resume() || !frozen.Freeze(next,eyes) || !session.Begin(next)) return 46;
                session.Finish(true);
                if(!frozen.Finish(next,true)) return 47;
            }
            if(session.Attempts()!=3 || session.Completed()!=3 || session.Epoch()!=3 ||
                session.EpochCompleted()!=1 || !session.Available()) return 48;
            // Genuine failures still cannot resume; this is not a retry path.
            if(!session.Begin(6001)) return 49;
            session.Finish(false);
            if(session.Resume() || session.Suspend()) return 50;
        }
    }
    grid2vr::HistoryState entry{},complete{},prepared{};
    grid2vr::SceneView original{},view{};
    // Pointer sentinels must survive both preparation and selective restoration.
    entry.fill(0x55); entry[0x90]=1; entry[0x1ec]=0;
    const uint32_t zero=0,one=1;
    memcpy(entry.data()+0xa0,&zero,4); memcpy(entry.data()+0x94,&zero,4);
    complete=entry; memcpy(complete.data()+0x94,&one,4);
    memset(complete.data()+0x10,0x33,0x40); original[0x8c]=1;
    if (!grid2vr::PrepareReplayHistory(entry,complete,original,prepared,view) ||
        grid2vr::HistoryWord(prepared,0xa0)!=1 || grid2vr::HistoryWord(prepared,0x94)!=1 ||
        memcmp(prepared.data()+0x50,entry.data()+0x10,0x40) || view[0x8c]!=0 || original[0x8c]!=1 ||
        !grid2vr::HistoryOtherFieldsEqual(complete,prepared)) return 1;
    grid2vr::RestoreHistoryWrites(prepared.data(),complete);
    if (prepared!=complete) return 2;
    for (size_t offset:{size_t(0),size_t(0xb8),size_t(0x130),size_t(0x148),size_t(0x1a8)}) {
        auto bad=complete; bad[offset]^=1;
        if (grid2vr::PrepareReplayHistory(entry,bad,original,prepared,view)) return 3;
        grid2vr::RestoreHistoryWrites(bad.data(),complete);
        if (bad[offset]==complete[offset]) return 4; // Never overwrite unknown state.
    }
    for (size_t offset:{size_t(0x90),size_t(0x94),size_t(0xa0),size_t(0x50),size_t(0x1ec)}) {
        auto bad=complete; bad[offset]^=1;
        if (grid2vr::PrepareReplayHistory(entry,bad,original,prepared,view)) return 5;
    }
    original[0x8c]=0;
    if (grid2vr::PrepareReplayHistory(entry,complete,original,prepared,view)) return 6;
    puts("Replay history seed, second-view flag, counter/identity guards and selective restoration passed.");
}
