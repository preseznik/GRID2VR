#pragma once
#include "scene_pose.h"
#include <istream>
#include <vector>

namespace grid2vr {
struct SceneViews {
    std::array<ScenePose,2> poses;
    std::array<EyeFrustum,2> frusta;
};
inline bool ValidSceneViews(const SceneViews& views) {
    for (unsigned eye=0;eye<2;++eye) {
        PoseRotation axes;
        if (!PoseAxes(views.poses[eye],axes)) return false;
        const auto& f=views.frusta[eye];
        for (float value:{f.left,f.right,f.down,f.up})
            if (!std::isfinite(value) || std::abs(value)>4) return false;
        if (f.left>=0 || f.right<=0 || f.down>=0 || f.up<=0 || f.right-f.left<.1f || f.up-f.down<.1f) return false;
    }
    return true;
}
// One immutable view pair from before visibility through final restoration.
// A dropped/failed frame terminates this bounded diagnostic; history is never
// silently continued across a gap or reused for a different tracking sample.
class SceneViewFrame {
public:
    bool Freeze(uint64_t frame,const SceneViews& views) {
        if (failed_ || pending_ || !frame || (frame_ && (frame<=frame_ || (!freshEpoch_ && frame!=frame_+1))) || !ValidSceneViews(views)) {
            failed_=true;return false;
        }
        frame_=frame;views_=views;pending_=true;freshEpoch_=false;return true;
    }
    const SceneViews* Get(uint64_t frame) const { return pending_ && !failed_ && frame==frame_?&views_:nullptr; }
    bool Finish(uint64_t frame,bool accepted) {
        const bool valid=Get(frame)!=nullptr && accepted;
        pending_=false;if (!valid) failed_=true;return valid;
    }
    bool Pending() const { return pending_; }
    uint64_t Frame() const { return frame_; }
    // The owner must prove that no eye replay began and native visibility was
    // restored. Unlike Finish(false), this deliberately starts a fresh epoch.
    bool CancelBeforeReplay(uint64_t frame) {
        if (!Get(frame)) { failed_=true;return false; }
        pending_=false;freshEpoch_=true;views_={};return true;
    }
    // Only a clean session suspension may begin a new tracking/history epoch.
    bool ResetEpoch() {
        if (pending_ || failed_) return false;
        freshEpoch_=true;views_={};return true;
    }
private:
    SceneViews views_;
    uint64_t frame_{};
    bool pending_{},failed_{},freshEpoch_{};
};
// Exactly count rows, each containing left then right quaternion, position and
// FOV tangents (22 floats). Recorded input never opens an OpenXR runtime.
inline bool ReadSceneViews(std::istream& input,unsigned count,std::vector<SceneViews>& output) {
    if (count<2 || count>900) return false;
    std::vector<SceneViews> candidate(count);
    for (auto& pair:candidate) {
        for (unsigned eye=0;eye<2;++eye) {
            for (auto& v:pair.poses[eye].orientation) input>>v;
            for (auto& v:pair.poses[eye].position) input>>v;
            auto& f=pair.frusta[eye];input>>f.left>>f.right>>f.down>>f.up;
        }
        if (!input || !ValidSceneViews(pair)) return false;
    }
    input>>std::ws;if (!input.eof()) return false;
    output=std::move(candidate);return true;
}
}
