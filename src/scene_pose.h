#pragma once
#include "replay_camera.h"
#include "replay_state.h"
#include "eye_projection.h"
#include <algorithm>
#include <limits>

namespace grid2vr {
// Right/up/back camera-local coordinates, as in OpenXR. Translation is already
// in native units; this helper does not establish metres or an eye separation.
struct ScenePose {
    std::array<float,4> orientation{0,0,0,1}; // x,y,z,w
    std::array<float,3> position{};
};
using PoseRotation=std::array<std::array<double,3>,3>;
// Shared with the visibility bound. Two native units accommodate standing and
// leaning at the current one-unit-per-metre scale without unbounded translation.
inline constexpr double maxSceneTranslation=2.0;
// Native camera interpolation can contract the basis: Brands Hatch frame 5834
// has singular scale .999345 and was refused by the former symmetric .001
// Gram-error bound. Allow at most ~.1% contraction; retain the original
// sqrt(1.001) expansion bound used by the all-rotation visibility volume.
inline constexpr double minNativeBasisSquaredScale=.998;
inline constexpr double maxNativeBasisSquaredScale=1.001;
template<class T> inline bool NativeBasisScaleSupported(const std::array<std::array<T,3>,3>& basis) {
    // Gershgorin bounds on B*B^T enclose every squared singular value. This
    // validates scale/conditioning without changing the native matrix or rays.
    for (size_t row=0;row<3;++row) {
        double diagonal{},radius{};
        for (size_t col=0;col<3;++col) {
            double dot{};for (size_t i=0;i<3;++i) dot+=double(basis[row][i])*basis[col][i];
            if (!std::isfinite(dot)) return false;
            if (row==col) diagonal=dot;else radius+=std::abs(dot);
        }
        if (diagonal-radius<minNativeBasisSquaredScale || diagonal+radius>maxNativeBasisSquaredScale) return false;
    }
    return true;
}
inline bool PoseAxes(const ScenePose& pose,PoseRotation& axes) {
    double norm{},distance{};
    for (float v:pose.orientation) { if (!std::isfinite(v)) return false; norm+=double(v)*v; }
    for (float v:pose.position) { if (!std::isfinite(v)) return false; distance+=double(v)*v; }
    // Tracking supplies a unit quaternion. Refuse invalid tracking/scale input;
    // normalize its small rounding error only. Visibility reserves eight units.
    if (std::abs(norm-1)>.001 || distance>maxSceneTranslation*maxSceneTranslation) return false;
    const double inv=1/std::sqrt(norm);
    const double x=pose.orientation[0]*inv,y=pose.orientation[1]*inv;
    const double z=pose.orientation[2]*inv,w=pose.orientation[3]*inv;
    axes={{{1-2*(y*y+z*z),2*(x*y+z*w),2*(x*z-y*w)},
           {2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w)},
           {2*(x*z+y*w),2*(y*z-x*w),1-2*(x*x+y*y)}}};
    return true;
}
inline bool ApplyScenePose(const CameraContext& camera,const SceneView& source,
                           const ScenePose& pose,SceneView& result) {
    PoseRotation rotation{};
    if (!PoseAxes(pose,rotation)) return false;
    std::array<std::array<float,3>,3> basis{};
    constexpr size_t offsets[]{0x20,0x10,0x30};
    constexpr float signs[]{-1,1,-1};
    for (size_t row=0;row<3;++row) {
        std::array<float,3> native{};
        memcpy(basis[row].data(),source.data()+offsets[row],12);
        memcpy(native.data(),camera.data()+0x70+16*row,12);
        for (size_t i=0;i<3;++i) {
            basis[row][i]*=signs[row];
            if (!std::isfinite(basis[row][i]) || !std::isfinite(native[i]) ||
                std::abs(basis[row][i]-native[i])>.0001f) return false;
        }
    }
    // A bounded near-unit basis alone is insufficient; verify its handedness,
    // exact native/view agreement and native inverse too. Never normalize it.
    if (!NativeBasisScaleSupported(basis)) return false;
    const auto& r=basis[0];const auto& u=basis[1];const auto& b=basis[2];
    const double handedness=(double(r[1])*u[2]-double(r[2])*u[1])*b[0]+
        (double(r[2])*u[0]-double(r[0])*u[2])*b[1]+(double(r[0])*u[1]-double(r[1])*u[0])*b[2];
    if (handedness<=0) return false;
    std::array<float,16> world{},inverse{};
    memcpy(world.data(),camera.data()+0x70,64);memcpy(inverse.data(),camera.data()+0x150,64);
    for (size_t i=0;i<16;++i)
        if (!std::isfinite(world[i]) || !std::isfinite(inverse[i])) return false;
    for (const auto* matrix:{&world,&inverse}) {
        for (size_t row=0;row<3;++row) if (std::abs((*matrix)[row*4+3])>1e-7f) return false;
        if (std::abs((*matrix)[15]-1)>.0001f) return false;
    }
    for (size_t row=0;row<4;++row) for (size_t col=0;col<4;++col) {
        double wv{},vw{},wvMagnitude{},vwMagnitude{};
        for (size_t k=0;k<4;++k) {
            const double a=double(world[row*4+k])*inverse[k*4+col];
            const double b=double(inverse[row*4+k])*world[k*4+col];
            wv+=a;vw+=b;wvMagnitude+=std::abs(a);vwMagnitude+=std::abs(b);
        }
        // Large native float translations cancel here. A fixed absolute test
        // rejected an otherwise valid Okutama camera at x=1559.7655. Allow the
        // standard seven-operation float dot-product roundoff bound only in
        // the translation row; keep the linear/affine checks unchanged.
        constexpr double u=std::numeric_limits<float>::epsilon()/2.;
        constexpr double gamma7=7*u/(1-7*u);
        const bool translation=row==3 && col<3;
        const double wvTolerance=translation?std::max(.0002,gamma7*wvMagnitude):.0002;
        const double vwTolerance=translation?std::max(.0002,gamma7*vwMagnitude):.0002;
        if (std::abs(wv-(row==col?1:0))>wvTolerance || std::abs(vw-(row==col?1:0))>vwTolerance) return false;
    }
    std::array<float,3> position{},nativePosition{};
    memcpy(position.data(),source.data()+0x40,12);memcpy(nativePosition.data(),camera.data()+0xa0,12);
    for (size_t i=0;i<3;++i) {
        if (!std::isfinite(position[i]) || !std::isfinite(nativePosition[i]) ||
            std::abs(position[i])>100000 || std::abs(nativePosition[i])>100000) return false;
        // Source position and native inverted-camera translation round
        // independently. At the startup camera near +/-10000, two float ULPs
        // are .001953125, exceeding the ordinary .001 agreement bound. Permit
        // only that representational error, capped at .01 native units; all
        // basis, affine and bidirectional inverse checks above still apply.
        const float magnitude=(std::max)(std::abs(position[i]),std::abs(nativePosition[i]));
        const double ulp=double(std::nextafter(magnitude,std::numeric_limits<float>::infinity()))-magnitude;
        const double tolerance=(std::max)(double(.001f),(std::min)(.01,2*ulp));
        if (std::abs(double(position[i])-nativePosition[i])>tolerance) return false;
    }
    auto candidate=source;
    for (size_t row=0;row<3;++row) {
        std::array<float,3> axis{};
        for (size_t i=0;i<3;++i) {
            double value{};
            for (size_t k=0;k<3;++k) value+=rotation[row][k]*basis[k][i];
            axis[i]=static_cast<float>(signs[row]*value);
        }
        memcpy(candidate.data()+offsets[row],axis.data(),12);
    }
    for (size_t i=0;i<3;++i) {
        double value=position[i];
        for (size_t k=0;k<3;++k) value+=double(pose.position[k])*basis[k][i];
        position[i]=static_cast<float>(value);
    }
    memcpy(candidate.data()+0x40,position.data(),12);
    result=candidate;return true;
}
inline bool SeparateSceneEyes(const ScenePose& center,float separation,std::array<ScenePose,2>& result) {
    PoseRotation axes{};
    if (!std::isfinite(separation) || separation<=0 || separation>.1f || !PoseAxes(center,axes)) return false;
    std::array<ScenePose,2> candidate{center,center};
    for (size_t eye=0;eye<2;++eye) {
        const double distance=double(separation)*(eye? .5:-.5);
        for (size_t i=0;i<3;++i)
            candidate[eye].position[i]=static_cast<float>(double(center.position[i])+distance*axes[0][i]);
        PoseRotation checked{};
        if (!PoseAxes(candidate[eye],checked)) return false;
    }
    result=candidate;return true;
}
inline bool PreparePoseHistory(const CameraContext& previous,const SceneView& previousView,
                               const HistoryState& entry,const ScenePose& pose,HistoryState& prepared,
                               const EyeFrustum* frustum=nullptr) {
    SceneView posed{};PoseRotation rotation{};
    if (memcmp(previous.data()+0x190,entry.data()+0x10,64) ||
        !ApplyScenePose(previous,previousView,pose,posed) || !PoseAxes(pose,rotation)) return false;
    auto projected=previous;
    if (frustum && !ApplyEyeFrustum(projected,*frustum)) return false;
    std::array<float,16> v{},p{},vp{};
    memcpy(v.data(),projected.data()+0x150,64);memcpy(p.data(),projected.data()+0x110,64);
    std::array<float,3> source{},target{};
    memcpy(source.data(),previousView.data()+0x40,12);memcpy(target.data(),posed.data()+0x40,12);
    std::array<double,16> moved{},rotated{};
    for (size_t i=0;i<16;++i) {
        if (!std::isfinite(v[i]) || !std::isfinite(p[i])) return false;
        moved[i]=v[i];
    }
    for (size_t col=0;col<4;++col) for (size_t k=0;k<3;++k)
        moved[12+col]-=(double(target[k])-source[k])*v[k*4+col];
    // V' = T(-world delta) * V * inverse(local orientation). Row-vector matrices.
    for (size_t row=0;row<4;++row) {
        for (size_t col=0;col<3;++col) for (size_t k=0;k<3;++k)
            rotated[row*4+col]+=moved[row*4+k]*rotation[col][k];
        rotated[row*4+3]=moved[row*4+3];
    }
    for (size_t row=0;row<4;++row) for (size_t col=0;col<4;++col) {
        double value{};
        for (size_t k=0;k<4;++k) value+=rotated[row*4+k]*p[k*4+col];
        vp[row*4+col]=static_cast<float>(value);
        if (!std::isfinite(vp[row*4+col])) return false;
    }
    memcpy(prepared.data()+0x50,vp.data(),64);return true;
}
}
