#pragma once
#include <cstdint>
#include <istream>

namespace grid2vr::resolution {
// Output stays at the headset recommendation. Supersampling only grows the
// scene: at most 256 MiB per owned RGBA copy (three same-frame copies).
inline bool ValidScene(unsigned width,unsigned height) {
    return width && height && width<=8192 && height<=8192 && uint64_t(width)*height<=64ull*1024*1024;
}
inline bool ValidOutput(unsigned width,unsigned height) {
    return width && height && width<=8192 && height<=8192 && uint64_t(width)*height<=16ull*1024*1024;
}
struct Plan {
    unsigned width{},height{},scale{};
    unsigned SceneWidth() const { return width*scale/100; }
    unsigned SceneHeight() const { return height*scale/100; }
    bool Valid() const {
        return ValidOutput(width,height) && scale>=50 && scale<=300 &&
            SceneWidth()>=320 && SceneHeight()>=180 && ValidScene(SceneWidth(),SceneHeight());
    }
};
inline bool Read(std::istream& stream,Plan& plan) {
    Plan parsed;char extra{};
    if (!(stream>>parsed.width>>parsed.height>>parsed.scale) || !parsed.Valid() || (stream>>extra)) return false;
    plan=parsed;return true;
}
}
