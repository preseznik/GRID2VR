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
// camera in garage run 20261024-224755-324. The OSD/radial programs witnessed
// on PC4090/PC5090 also retain their clip-derived artwork in TEXCOORD5.
// See docs/evidence/render-slider-ui-candidate-2026-10-24.json before promotion.
inline constexpr std::array<uint64_t,11> screenUiShaders{0xe057d253ecd4296eull,0x4f6292c5bc65747bull,0x6980d3e81754047dull,
    0x588d9cc776122d0cull,0xa904c98e05d8c928ull,0xad7b09c4361f558dull,0x5d848c8b6735974full,
    0x0ef1c3aff7d5fd94ull,0x7115b259dffd0f6aull,0x0f53b20492bb7c46ull,0x4dd4ed841ce0d62eull};
inline size_t ScreenUiShaderIndex(uint64_t shader) {
    const auto found=std::find(screenUiShaders.begin(),screenUiShaders.end(),shader);
    return size_t(found-screenUiShaders.begin());
}
inline bool ScreenUiVertexShader(uint64_t shader) { return ScreenUiShaderIndex(shader)<screenUiShaders.size(); }
// Observation only. These exact installed ui.pssg programs include overlay and
// frosted/bokeh text paths which are not qualified for stereo projection. Keep
// them separate from screenUiShaders: recording a draw must never enable it.
inline constexpr std::array<uint64_t,7> screenUiObservedShaders{0x7daadac4d591f788ull,
        0x50bfdc0327161633ull,0x6d11a1a3430512b8ull,0xc4acd869afab5c6full,0x7d34bb36e718a024ull,
        0xb544d9c782ef20e1ull,0xe5aa765123374f27ull};
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
    // PC4090's native UI view/inverse matrices have a different, repeatable
    // floating-point result. Retain its complete normalized fingerprint rather
    // than accepting approximately screen-shaped world cameras.
    return hash==0x56b67c51213d9ad8ull || hash==0xf1fb9ea46321746full || hash==0xd508427cf507be3bull?hash:0;
}
inline bool ScreenUiCamera(std::span<const uint8_t> data) { return ScreenUiCameraIdentity(data)!=0; }
// The two retained native UI camera families use the renderer's pixel aspect,
// even when hardware_settings_config requests 16:9. Restore their logical
// widescreen canvas before vertex clipping, independently of eye texture size.
// Every other camera byte remains guarded by the existing exact identities.
inline bool FlatScreenUiCamera(std::span<const uint8_t> source,unsigned width,unsigned height,std::array<uint8_t,320>& output) {
    if (source.size()!=320 || !width || !height || width>8192 || height>8192) return false;
    const auto value=[&](size_t offset) {float f;memcpy(&f,source.data()+offset,4);return f;};
    const float px=value(0),py=value(20);
    if (!std::isfinite(px) || !std::isfinite(py) || px<=.01f || px>16 || py<=.01f || py>16 ||
        std::abs(double(py)/px-double(width)/height)>1e-5) return false;
    const bool ortho=value(60)==1;
    const float originalX=ortho?.1403159201145172f:1.4894007444381714f;
    const float originalY=ortho?.24945054948329926f:2.6478235721588135f;
    if (ortho && std::abs(py-originalY)>originalY*2e-6f) return false;
    const double sx=px/originalX,sy=py/originalY;
    struct Field {unsigned offset,bits;int axis;}; // +/-1=X, +/-2=Y; negative is inverse
    constexpr Field orthographic[]={{0,0x3e0faefa,1},{20,0x3e7f6ff7,2},{64,0x3e0faefa,1},{84,0x3e7f6ff7,2},{240,0x40e40e8a,-1},
        {260,0x4080482d,-2},{280,0xc69c4000,0},{296,0x461c4000,0},{300,0x3f800000,0}};
    constexpr Field perspective[]={{0,0x3fbea4af,1},{20,0x402975f1,2},{64,0x3fbea4af,1},{68,0xa90055a1,2},
        {84,0x402975f1,2},{96,0xb425e79c,1},{100,0xb493788a,2},{112,0x93bea4b0,1},{116,0x9fdbdf63,2},
        {240,0x3f2be197,-1},{260,0x3ec15dc9,-2},{300,0x40a00069,0}};
    // +284/+296 are perspective inverse-projection terms computed from the
    // lens; their last bit varies with render size (2150x2284 headset run
    // 20261029-132258-607 rounds +284 one ULP). Bound them like +300, then try
    // only the two roundings seen in the recorded native fingerprints.
    struct Rounding {unsigned offset;std::array<uint32_t,2> bits;};
    constexpr Rounding rounding[]={{284,{0xc09fff98,0xc09fff97}},{296,{0xbf800000,0xbf7fffff}}};
    const std::span<const Field> fields=ortho?std::span<const Field>(orthographic):std::span<const Field>(perspective);
    for (unsigned variant=0;variant<(ortho?1u:2u);++variant) {
        std::array<uint8_t,320> candidate{};std::copy(source.begin(),source.end(),candidate.begin());
        bool valid=true;
        for (const auto& field:fields) {
            auto bits=field.bits;
            // The alternate view has ~2.27e-13 translation residuals. Only
            // those two lens-scaled fields differ here; its remaining view,
            // projection and inverse fields must match the full fingerprint.
            if (variant && field.offset==112) bits=0x2abea4af;
            if (variant && field.offset==116) bits=0x2b2975f0;
            float original;memcpy(&original,&bits,4);
            const double scale=std::abs(field.axis)==1?sx:std::abs(field.axis)==2?sy:1.;
            const double expected=field.axis<0?original/scale:original*scale,actual=value(field.offset);
            if (!std::isfinite(actual) || std::abs(actual-expected)>std::max(1e-35,std::abs(expected)*2e-6)) {valid=false;break;}
            memcpy(candidate.data()+field.offset,&bits,4);
        }
        if (!valid) continue;
        if (ortho) {
            if (ScreenUiCamera(candidate)) {output=candidate;return true;}
            continue;
        }
        for (const auto& field:rounding) {
            float original;memcpy(&original,&field.bits[0],4);const double actual=value(field.offset);
            if (!std::isfinite(actual) || std::abs(actual-original)>std::abs(original)*2e-6) {valid=false;break;}
        }
        if (!valid) continue;
        for (unsigned choice=0;choice<4;++choice) {
            for (unsigned i=0;i<2;++i) memcpy(candidate.data()+rounding[i].offset,&rounding[i].bits[(choice>>i)&1],4);
            if (ScreenUiCamera(candidate)) {output=candidate;return true;}
        }
    }
    return false;
}
// Stereo retains the native vertex shader and artwork varyings. Only its clip
// position is mapped back to the verified 16:9 canvas by the post-VS plane.
// The aspect-dependent camera must match the actual viewport; unknown camera
// fields still have to pass the complete canonical identity above.
inline uint64_t ScreenUiCanvasIdentity(std::span<const uint8_t> source,unsigned width,unsigned height,std::array<float,2>& scale) {
    if (const auto identity=ScreenUiCameraIdentity(source)) {scale={1,1};return identity;}
    std::array<uint8_t,320> canonical{};
    if (!FlatScreenUiCamera(source,width,height,canonical)) return 0;
    std::array<float,2> candidate{};
    for (unsigned axis=0;axis<2;++axis) {
        float before{},after{};memcpy(&before,source.data()+axis*20,4);memcpy(&after,canonical.data()+axis*20,4);
        candidate[axis]=after/before;
    }
    // An unmodified 16:9 lens requires the original exact camera identity.
    // Do not turn the aspect family's rounding allowance into a weaker legacy
    // guard for a camera with an unrelated one-bit change to its matrix.
    if (candidate[0]==1 && candidate[1]==1) return 0;
    scale=candidate;return ScreenUiCameraIdentity(canonical);
}
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
    if (shader==0x0f53b20492bb7c46ull) return 55; // T0, T1, T2, T4, T5
    if (shader==0x4dd4ed841ce0d62eull) return 63; // T0 through T5
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
