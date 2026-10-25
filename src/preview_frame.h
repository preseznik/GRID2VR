#pragma once
#include <cstdint>

namespace grid2vr {
// A preview can consume only a complete pair from this Present's render frame.
// Never keep the previous eye visible across a pause, reset or failed pair.
class PreviewFrame {
public:
    struct Image { uint64_t scene{}; bool audit{}; };
    void Publish(uint64_t frame,uint64_t scene,bool ready,bool audit) {
        frame_=frame;image_=ready && scene?Image{scene,audit}:Image{};
    }
    Image Consume(uint64_t frame,bool rendering) {
        const auto image=rendering && frame==frame_?image_:Image{};
        Reset();return image;
    }
    void Reset() {frame_=0;image_={};}
private:
    uint64_t frame_{};
    Image image_{};
};
}
