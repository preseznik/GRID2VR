#pragma once
#include <cmath>
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
// Field of view crops each eye about its optical axis in tangent space, left
// and right by fovX, up and down by fovY, each 10-100% (a helmet crop keeps fovX
// at 100).
// The scene shrinks by the same factors, so its pixels stay square.
struct Plan {
    unsigned width{},height{},scale{},fovX{100},fovY{100};
    unsigned SceneWidth() const { return width*scale*fovX/10000; }
    unsigned SceneHeight() const { return height*scale*fovY/10000; }
    bool Valid() const {
        return ValidOutput(width,height) && scale>=50 && scale<=300 && fovX>=10 && fovX<=100 && fovY>=10 && fovY<=100 &&
            SceneWidth()>=320 && SceneHeight()>=180 && ValidScene(SceneWidth(),SceneHeight());
    }
};
// "width height scale", then optionally one field of view percent for both
// axes, or two: horizontal and vertical.
inline bool Read(std::istream& stream,Plan& plan) {
    Plan parsed;unsigned fovX{},fovY{};char extra{};
    if (!(stream>>parsed.width>>parsed.height>>parsed.scale)) return false;
    if (stream>>fovX) {
        parsed.fovX=parsed.fovY=fovX;
        if (stream>>fovY) parsed.fovY=fovY;
        else if (!stream.eof()) return false;
    } else if (!stream.eof()) return false;
    if (!parsed.Valid() || (stream>>extra)) return false;
    plan=parsed;return true;
}
inline float CropAngle(float angle,unsigned fov) { return std::atan(std::tan(angle)*fov/100.f); }
}
