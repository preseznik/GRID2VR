#include "replay_sequence.h"
#include "scene_visibility_handoff.h"
#include <cstdio>
using grid2vr::ReplaySequence;

int main() {
    ReplaySequence session;
    // Native object discovery may wait through startup. A newly constructed
    // sequence is not a configured continuous session and cannot be suspended.
    if (session.Continuous() || session.Suspend() || session.Resume() || session.Stop()) return 13;
    if (!session.ConfigureSession() || !session.Continuous() || session.Limit()!=0 ||
        session.Status()!=ReplaySequence::State::Suspended || session.Available() ||
        session.Begin(1) || session.Configure(10) || session.ConfigureSession()) return 1;
    if (!session.Resume() || session.Epoch()!=1 || session.Resume()) return 2;
    if (!session.Begin(100) || session.Suspend() || session.Stop() || !session.Observing()) return 3;
    session.Finish(true);
    if (!session.Suspend() || session.Available() || session.Begin(101) || session.Completed()!=1) return 4;
    // A restored suspension permits a frame gap, but starts a fresh history
    // epoch. The owner must reseed both eye histories instead of carrying them.
    if (!session.Resume() || session.Epoch()!=2 || session.EpochCompleted()!=0 ||
        !session.Begin(200) || session.Completed()!=1) return 5;
    session.Finish(true);
    if (session.EpochCompleted()!=1 || !session.Begin(201)) return 6;
    session.Finish(true);
    if (!session.Suspend() || !session.Resume() || session.Begin(201) ||
        session.Status()!=ReplaySequence::State::Failed || session.Resume()) return 7;
    ReplaySequence gap;
    gap.ConfigureSession();gap.Resume();gap.Begin(10);gap.Finish(true);
    if (gap.Begin(12) || gap.Available() || gap.Suspend() || gap.ConfigureSession()) return 8;
    ReplaySequence failed;
    failed.ConfigureSession();failed.Resume();failed.Begin(10);failed.Finish(false);
    if (failed.Completed() || failed.Resume() || failed.Suspend() || failed.Stop()) return 9;
    ReplaySequence poisoned;
    poisoned.ConfigureSession();poisoned.Resume();poisoned.Begin(20);poisoned.Fail();poisoned.Finish(true);
    if (poisoned.Completed() || poisoned.Available() || poisoned.Resume()) return 12;
    // Render until explicitly stopped: exercise well beyond the diagnostic cap
    // entirely offline, without using headset time as a frame-count milestone.
    ReplaySequence persistent;
    persistent.ConfigureSession();persistent.Resume();
    for (uint64_t frame=1;frame<=100000;++frame) {
        if (!persistent.Begin(frame)) return 10;
        persistent.Finish(true);
    }
    if (!persistent.Available() || persistent.Completed()!=100000 || !persistent.Stop() ||
        persistent.Available() || persistent.Begin(100001) || persistent.Resume() || persistent.Configure(10)) return 11;
    // Reproduce the captured garage exit: pair 3682 completed, then scene 3683
    // drew without a visibility callback. It must not begin a failed eye pair.
    grid2vr::SceneViews input{{grid2vr::ScenePose{},grid2vr::ScenePose{}},
        {grid2vr::diagnosticFrustum,grid2vr::diagnosticFrustum}};
    grid2vr::SceneViewFrame views;
    ReplaySequence transition;transition.ConfigureSession();transition.Resume();
    if (!views.Freeze(3682,input) || !transition.Begin(3682)) return 14;
    transition.Finish(true);
    if (!views.Finish(3682,true) ||
        !grid2vr::CanSuspendWithoutVisibility(transition,views,3683,3682,false,true)) return 15;
    if (grid2vr::CanSuspendWithoutVisibility(transition,views,3683,3683,false,true) ||
        grid2vr::CanSuspendWithoutVisibility(transition,views,3683,3684,false,true) ||
        grid2vr::CanSuspendWithoutVisibility(transition,views,3683,3682,true,true) ||
        grid2vr::CanSuspendWithoutVisibility(transition,views,3683,3682,false,false) ||
        grid2vr::CanSuspendWithoutVisibility(transition,views,3682,3681,false,true) ||
        grid2vr::CanSuspendWithoutVisibility(transition,views,0,0,false,true)) return 16;
    if (!transition.Suspend() || !views.ResetEpoch() || transition.Attempts()!=1 ||
        grid2vr::CanSuspendWithoutVisibility(transition,views,3684,3682,false,true)) return 17;
    // Loading may last many frames. The event gets a new view/history epoch.
    if (!transition.Resume() || !views.Freeze(5000,input) ||
        grid2vr::CanSuspendWithoutVisibility(transition,views,5000,3682,false,true) ||
        !transition.Begin(5000) ||
        grid2vr::CanSuspendWithoutVisibility(transition,views,5001,3682,false,true)) return 18;
    transition.Finish(true);
    if (!views.Finish(5000,true) || transition.Completed()!=2 || transition.Epoch()!=2) return 19;
    // The return to garage must survive the same boundary, while a genuine
    // replay failure and bounded diagnostics must never enter this recovery.
    if (!grid2vr::CanSuspendWithoutVisibility(transition,views,5001,5000,false,true) ||
        !transition.Suspend() || !views.ResetEpoch() || !transition.Resume() ||
        !views.Freeze(6000,input) || !transition.Begin(6000)) return 20;
    transition.Finish(false);views.Finish(6000,false);
    ReplaySequence bounded;
    if (grid2vr::CanSuspendWithoutVisibility(transition,views,6001,6000,false,true) ||
        grid2vr::CanSuspendWithoutVisibility(bounded,views,6001,6000,false,true)) return 21;
    // A flashback camera transition refused before either eye: cancel without
    // failing or counting the pair, then suspend into a fresh epoch.
    ReplaySequence flashback;flashback.ConfigureSession();flashback.Resume();
    if (flashback.Cancel() || !flashback.Begin(17026)) return 22;
    flashback.Finish(true);
    if (!flashback.Begin(17027) || !flashback.Cancel() || flashback.Cancel() || flashback.Attempts()!=1 ||
        flashback.Completed()!=1 || flashback.Status()!=ReplaySequence::State::Rendering || flashback.LastFrame()!=17027 ||
        !flashback.Suspend() || flashback.Begin(17028) || !flashback.Resume() || flashback.Epoch()!=2 ||
        flashback.EpochCompleted()!=0 || !flashback.Begin(17058)) return 23;
    flashback.Finish(true);
    if (flashback.Attempts()!=flashback.Completed() || flashback.Status()!=ReplaySequence::State::Rendering) return 24;
    ReplaySequence bounded2;bounded2.Configure(10);bounded2.Begin(1);
    ReplaySequence poisonedCancel;poisonedCancel.ConfigureSession();poisonedCancel.Resume();poisonedCancel.Begin(5);poisonedCancel.Fail();
    if (bounded2.Cancel() || poisonedCancel.Cancel() || poisonedCancel.Resume()) return 25;
    puts("Explicit session epochs, clean suspension, monotonic frames, fatal restoration failure, transient cancellation and user stop pass.");
}
