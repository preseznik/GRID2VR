#pragma once
#include "replay_sequence.h"
#include "scene_views.h"

namespace grid2vr {
// A native scene can finish after world visibility has stopped during loading.
// Suspend only before acquiring views or beginning either eye. A callback that
// ran and failed, a pending restoration, or a failed pair must remain fatal.
inline bool CanSuspendWithoutVisibility(const ReplaySequence& sequence,const SceneViewFrame& views,
    uint64_t frame,uint64_t consumerFrame,bool visibilityPending,bool guardsHealthy) {
    return guardsHealthy && sequence.Continuous() && sequence.Available() &&
        frame && frame>sequence.LastFrame() && consumerFrame<frame &&
        !views.Pending() && !visibilityPending;
}
}
