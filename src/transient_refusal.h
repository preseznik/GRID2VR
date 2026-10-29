#pragma once
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace grid2vr {
// A camera transition, for example entering or leaving GRID 2's flashback, can
// refuse one eye pair before any eye or game write. Such a refusal cancels the
// pair and suspends stereo until the existing readiness gate passes again; the
// guard conditions themselves are unchanged. Identity, instruction, memory and
// post-write failures still fail the session.
inline bool TransientReplayRefusal(const char* reason,unsigned pass) {
    // Only the first eye: the second eye follows a rendered and restored first eye.
    if (pass!=1 || !reason) return false;
    for (const char* allowed:{"replay_camera_transition","replay_far_plane_changed","replay_view_changed",
                              "replay_history_camera_mismatch","replay_history_transition","eye_camera_pose"})
        if (!std::strcmp(reason,allowed)) return true;
    return false;
}
// Geometric refusals computed from read-only copies, before the native volume write.
inline bool TransientVisibilityRefusal(const char* reason) {
    if (!reason) return false;
    for (const char* allowed:{"native_volume_geometry","eye_frustum_construction","native_frustum_readback"})
        if (!std::strcmp(reason,allowed)) return true;
    return false;
}
// Stay suspended briefly so a lasting transition does not alternate blank and
// stereo frames; the readiness gate still decides when stereo resumes.
inline constexpr uint64_t transientRefusalHoldFrames=30;
inline bool TransientRefusalHold(uint64_t refusedFrame,uint64_t frame) {
    return refusedFrame && frame<=refusedFrame+transientRefusalHoldFrames;
}
}
