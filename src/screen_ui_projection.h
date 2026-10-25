#pragma once
#include "scene_views.h"
#include <span>
#include <sstream>

namespace grid2vr {
// Installed ui.pssg: these instanced VS programs all produce clip XYZW from
// cb3[4..7]. The same programs also draw world tags, so the camera guard is
// mandatory. Include colour/mask geometry as well as image/distance-field text.
// Frosted/bokeh background and text use original clip XY/W as a background UV;
// post-VS projection preserves it. Both were observed with the exact native UI
// camera in garage run 20261024-224755-324. Clip-derived artwork programs with
// additional varyings (4dd4ed841ce0d62e,0f53b20492bb7c46) remain excluded.
// See docs/evidence/render-slider-ui-candidate-2026-10-24.json before promotion.
inline constexpr std::array<uint64_t,9> screenUiShaders{0xe057d253ecd4296eull,0x4f6292c5bc65747bull,0x6980d3e81754047dull,
    0x588d9cc776122d0cull,0xa904c98e05d8c928ull,0xad7b09c4361f558dull,0x5d848c8b6735974full,
    0x0ef1c3aff7d5fd94ull,0x7115b259dffd0f6aull};
inline size_t ScreenUiShaderIndex(uint64_t shader) {
    const auto found=std::find(screenUiShaders.begin(),screenUiShaders.end(),shader);
    return size_t(found-screenUiShaders.begin());
}
inline bool ScreenUiVertexShader(uint64_t shader) { return ScreenUiShaderIndex(shader)<screenUiShaders.size(); }
// Observation only. These exact installed ui.pssg programs include overlay and
// frosted/bokeh text paths which are not qualified for stereo projection. Keep
// them separate from screenUiShaders: recording a draw must never enable it.
inline constexpr std::array<uint64_t,9> screenUiObservedShaders{0x7daadac4d591f788ull,
        0x50bfdc0327161633ull,0x6d11a1a3430512b8ull,0xc4acd869afab5c6full,0x7d34bb36e718a024ull,
        0xb544d9c782ef20e1ull,0xe5aa765123374f27ull,0x0f53b20492bb7c46ull,0x4dd4ed841ce0d62eull};
inline size_t ScreenUiWitnessIndex(uint64_t shader) {
    const auto projected=ScreenUiShaderIndex(shader);if (projected<screenUiShaders.size()) return projected;
    return screenUiShaders.size()+size_t(std::find(screenUiObservedShaders.begin(),screenUiObservedShaders.end(),shader)-screenUiObservedShaders.begin());
}
inline bool ScreenUiWitnessShader(uint64_t shader) { return ScreenUiWitnessIndex(shader)<screenUiShaders.size()+screenUiObservedShaders.size(); }
inline uint64_t ScreenUiHash(std::span<const uint8_t> data) {
    uint64_t hash=14695981039346656037ull;
    for (auto byte:data) { hash^=byte;hash*=1099511628211ull; }
    return hash;
}
inline uint64_t ScreenUiCameraIdentity(std::span<const uint8_t> data) {
    if (data.size()!=320) return 0;
    const auto hash=ScreenUiHash(data.first(308));
    // Exact native perspective and orthographic UI buffers observed in four
    // independent captures. Live 20261024 evidence differs only in the final
    // twelve uninitialized padding bytes. Ignore those in identity, preserve
    // them in output. Keep every actual native camera field bit-exact.
    return hash==0x56b67c51213d9ad8ull || hash==0xf1fb9ea46321746full?hash:0;
}
inline bool ScreenUiCamera(std::span<const uint8_t> data) { return ScreenUiCameraIdentity(data)!=0; }
struct ScreenUiOptions {
    // Preserve legacy request-less diagnostics. The launcher supplies its own
    // explicit defaults (one metre, fixed to the native car camera).
    float distanceMetres=2;
    bool followView=true;
    unsigned hiddenMask{}; // Recorded only: GRID race-only element mapping is unverified.
};
inline bool ValidScreenUiOptions(const ScreenUiOptions& value) {
    return std::isfinite(value.distanceMetres) && value.distanceMetres>=1 && value.distanceMetres<=20 &&
        std::abs(value.distanceMetres*2-std::round(value.distanceMetres*2))<1e-5 && value.hiddenMask<=31;
}
inline bool ReadScreenUiOptions(std::istream& input,ScreenUiOptions& result) {
    ScreenUiOptions candidate;int follow{};
    if (!(input>>candidate.distanceMetres>>follow>>candidate.hiddenMask) || (follow!=0 && follow!=1)) return false;
    candidate.followView=follow!=0;input>>std::ws;
    if (!input.eof() || !ValidScreenUiOptions(candidate)) return false;
    result=candidate;return true;
}
// Post-VS clip-space homography. Native artwork UVs and all other varyings have
// already been computed and are never recomputed from the projected position.
struct ScreenUiPlane {
    std::array<std::array<float,4>,3> rows{}; // dot(row.xyz, native clip XYW) -> new XYW
};
inline bool ScreenUiPlaneProjection(const SceneViews& views,unsigned eye,const ScreenUiOptions& options,ScreenUiPlane& output) {
    if (eye>=2 || !ValidSceneViews(views) || !ValidScreenUiOptions(options)) return false;
    PoseRotation axes{};if (!PoseAxes(views.poses[eye],axes)) return false;
    ScenePose anchor;
    if (options.followView) {
        double dot{},length{};
        for (unsigned i=0;i<4;++i) dot+=double(views.poses[0].orientation[i])*views.poses[1].orientation[i];
        for (unsigned i=0;i<4;++i) {
            anchor.orientation[i]=views.poses[0].orientation[i]+(dot<0?-1.f:1.f)*views.poses[1].orientation[i];
            length+=double(anchor.orientation[i])*anchor.orientation[i];
        }
        if (length<1e-8) return false;
        for (auto& v:anchor.orientation) v=float(v/std::sqrt(length));
        for (unsigned i=0;i<3;++i) anchor.position[i]=(views.poses[0].position[i]+views.poses[1].position[i])*.5f;
    }
    PoseRotation anchorAxes{};if (!PoseAxes(anchor,anchorAxes)) return false;
    constexpr double px=1.4894007444381714,py=2.6478235721588135;
    const double depth=options.distanceMetres;
    std::array<std::array<double,3>,3> local{};
    for (unsigned axis=0;axis<3;++axis) for (unsigned k=0;k<3;++k) {
        local[axis][0]+=depth/px*anchorAxes[0][k]*axes[axis][k];
        local[axis][1]+=depth/py*anchorAxes[1][k]*axes[axis][k];
        local[axis][2]+=(anchor.position[k]-depth*anchorAxes[2][k]-views.poses[eye].position[k])*axes[axis][k];
    }
    const auto& f=views.frusta[eye];ScreenUiPlane candidate;
    for (unsigned i=0;i<3;++i) {
        candidate.rows[0][i]=float((2*local[0][i]+(f.right+f.left)*local[2][i])/(f.right-f.left));
        candidate.rows[1][i]=float((2*local[1][i]+(f.up+f.down)*local[2][i])/(f.up-f.down));
        candidate.rows[2][i]=float(-local[2][i]);
        for (const auto& row:candidate.rows) if (!std::isfinite(row[i])) return false;
    }
    output=candidate;return true;
}
inline unsigned ScreenUiVaryingMask(uint64_t shader) {
    if (!ScreenUiVertexShader(shader)) return 0;
    return shader==0x6980d3e81754047dull || shader==0x588d9cc776122d0cull || shader==0xad7b09c4361f558dull ||
        shader==0x7115b259dffd0f6aull?31:23;
}
struct ScreenUiTransform { float sx{},sy{},bx{},by{}; };
inline bool ScreenUiTransforms(const SceneViews& views,std::array<ScreenUiTransform,2>& result) {
    if (!ValidSceneViews(views)) return false;
    PoseRotation axes[2];
    if (!PoseAxes(views.poses[0],axes[0]) || !PoseAxes(views.poses[1],axes[1])) return false;
    // An XY-only correction requires parallel eye orientations. Do not apply
    // an approximate transform to canted views; a full UI layer is needed there.
    for (size_t r=0;r<3;++r) for (size_t c=0;c<3;++c)
        if (std::abs(axes[0][r][c]-axes[1][r][c])>1e-5) return false;
    std::array<ScreenUiTransform,2> candidate{};
    for (unsigned eye=0;eye<2;++eye) {
        std::array<double,3> offset{};
        for (size_t axis=0;axis<3;++axis) for (size_t k=0;k<3;++k)
            offset[axis]+=.5*(views.poses[eye].position[k]-views.poses[1-eye].position[k])*axes[eye][axis][k];
        for (auto value:offset) if (std::abs(value)>.1) return false;
        // Place the native UI canvas two native units ahead of the eye midpoint,
        // preserving its native perspective angular size (16:9). Both eyes see
        // the same plane. World camera matrices, IPD and tracking are untouched.
        constexpr double depth=2.,px=1.4894007444381714,py=2.6478235721588135;
        const auto& f=views.frusta[eye];const double z=depth+offset[2];
        candidate[eye]={float(2*depth/(px*z*(f.right-f.left))),float(2*depth/(py*z*(f.up-f.down))),
            float(-(f.right+f.left+2*offset[0]/z)/(f.right-f.left)),
            float(-(f.up+f.down+2*offset[1]/z)/(f.up-f.down))};
    }
    result=candidate;return true;
}
inline bool ProjectScreenUi(std::span<const uint8_t> source,const SceneViews& views,unsigned eye,std::array<uint8_t,320>& output) {
    std::array<ScreenUiTransform,2> transforms{};
    if (eye>=2 || !ScreenUiCamera(source) || !ScreenUiTransforms(views,transforms)) return false;
    std::array<uint8_t,320> candidate{};std::copy(source.begin(),source.end(),candidate.begin());
    const auto& t=transforms[eye];
    // Shader assembly: clip = x*cb3[4] + y*cb3[5] + z*cb3[6] + cb3[7].
    // Modify only the X/Y components of these four vectors; Z/W stay bit exact.
    for (size_t row=0;row<4;++row) {
        std::array<float,4> v{};memcpy(v.data(),source.data()+64+16*row,16);
        v[0]=t.sx*v[0]+t.bx*v[3];v[1]=t.sy*v[1]+t.by*v[3];
        if (!std::isfinite(v[0]) || !std::isfinite(v[1])) return false;
        memcpy(candidate.data()+64+16*row,v.data(),8);
    }
    output=candidate;return true;
}
}
