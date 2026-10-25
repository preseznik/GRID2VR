#pragma once
#include <array>
#include <cstdint>

namespace grid2vr {
// CPU-only evidence for the current native scene and its immediate predecessor.
// No GPU resources are retained, and successful later scenes produce no dump.
class CenterDrawWitness {
public:
    static constexpr unsigned MaxDraws=4096;
    struct Draw {
        uint64_t shader{};
        unsigned ordinal{},slot{},cb0Bytes{};
        const char* kind{}; // Work receives static draw-method names.
    };
    struct Frame {
        uint64_t scene{};
        unsigned count{};
        bool truncated{};
        std::array<Draw,MaxDraws> draws{};
    };
    void Begin(uint64_t scene) {
        current_^=1;
        auto& frame=frames_[current_];
        frame.scene=scene;frame.count=0;frame.truncated=false;
    }
    void Add(Draw draw) {
        auto& frame=frames_[current_];
        if (!frame.scene) return;
        if (frame.count==MaxDraws) { frame.truncated=true;return; }
        frame.draws[frame.count++]=draw;
    }
    void Truncate() { frames_[current_].truncated=true; }
    const Frame& Current() const { return frames_[current_]; }
    const Frame& Previous() const { return frames_[current_^1]; }
private:
    std::array<Frame,2> frames_{};
    unsigned current_{};
};
}
