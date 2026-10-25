#pragma once
#include "visibility_volume.h"
#include "scene_views.h"

namespace grid2vr {
// This candidate encloses only the frozen current pair, plus the original
// native corners. It does not change the existing all-rotation default.
struct TrackedVisibilityBox {
    std::array<double,3> center{};
    std::array<std::array<double,3>,3> axes{}; // Orthonormal right/up/back.
    std::array<double,3> halfExtent{};
    alignas(16) std::array<float,16> matrix{};
};
struct TrackedVisibilityCoverage {
    double farDistance{};
    double minimumNativeSide{};
    double minimumFarNativeSide{};
    unsigned eyePoints{},outsideNative{};
    unsigned farCornersOutsideNative{};
};
inline constexpr double trackedVisibilityMargin=.25; // Native units, not metres.

namespace tracked_visibility_detail {
using Point=std::array<double,3>;
using Basis=std::array<Point,3>;
inline double Dot(const Point& a,const Point& b) {
    return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
}
inline Point Cross(const Point& a,const Point& b) {
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
}
inline bool Normalize(Point& p) {
    const double length=std::sqrt(Dot(p,p));
    if (!std::isfinite(length) || length<.9 || length>1.1) return false;
    for (auto& value:p) value/=length;
    return true;
}
struct Geometry {
    std::array<Point,18> points{}; // Eight native corners; origin + four corners per eye.
    Basis axes{};
    TrackedVisibilityCoverage coverage{};
};
inline bool Prepare(const VisibilityVolume& volume,const SceneView& visibility,
                    const SceneView& render,const SceneViews& views,Geometry& result) {
    VisibilityBox nativeCheck;
    if (!ValidSceneViews(views) || !MakeVisibilityBox(volume,visibility,nativeCheck)) return false;
    Geometry candidate;
    Basis native{};
    constexpr size_t offsets[]{0x20,0x10,0x30};
    constexpr double signs[]{-1,1,-1};
    Point origin{};
    for (size_t row=0;row<3;++row) {
        float position{};memcpy(&position,render.data()+0x40+4*row,4);
        if (!std::isfinite(position) || std::abs(position)>100000) return false;
        origin[row]=position;
        for (size_t axis=0;axis<3;++axis) {
            float value{};memcpy(&value,render.data()+offsets[row]+4*axis,4);
            if (!std::isfinite(value)) return false;
            native[row][axis]=signs[row]*value;
        }
    }
    if (!NativeBasisScaleSupported(native)) return false;
    if (Dot(Cross(native[0],native[1]),native[2])<=0) return false;
    // Use orthonormal box axes, but retain the actual near-affine native basis
    // when constructing eye rays. A transpose of that native basis is not its
    // inverse. Gram-Schmidt here cannot discard its distortion from the rays.
    candidate.axes[0]=native[0];
    if (!Normalize(candidate.axes[0])) return false;
    candidate.axes[1]=native[1];
    const double projection=Dot(candidate.axes[1],candidate.axes[0]);
    for (size_t i=0;i<3;++i) candidate.axes[1][i]-=projection*candidate.axes[0][i];
    if (!Normalize(candidate.axes[1])) return false;
    candidate.axes[2]=Cross(candidate.axes[0],candidate.axes[1]);
    if (!Normalize(candidate.axes[2])) return false;
    std::array<float,56> data{};memcpy(data.data(),volume.data(),volume.size());
    for (size_t plane=0;plane<6;++plane) {
        double side=data[4*plane+3];
        for (size_t axis=0;axis<3;++axis) side+=double(data[4*plane+axis])*nativeCheck.center[axis];
        candidate.coverage.farDistance=(std::max)(candidate.coverage.farDistance,side);
    }
    if (candidate.coverage.farDistance<25 || candidate.coverage.farDistance>25000) return false;
    for (size_t point=0;point<8;++point)
        for (size_t axis=0;axis<3;++axis) candidate.points[point][axis]=data[24+4*point+axis];
    candidate.coverage.minimumNativeSide=std::numeric_limits<double>::infinity();
    candidate.coverage.minimumFarNativeSide=std::numeric_limits<double>::infinity();
    for (size_t eye=0;eye<2;++eye) {
        PoseRotation rotation{};
        if (!PoseAxes(views.poses[eye],rotation)) return false;
        Point eyeOrigin=origin;
        Basis posed{};
        for (size_t axis=0;axis<3;++axis) {
            for (size_t k=0;k<3;++k) eyeOrigin[axis]+=double(views.poses[eye].position[k])*native[k][axis];
            for (size_t row=0;row<3;++row)
                for (size_t k=0;k<3;++k) posed[row][axis]+=rotation[row][k]*native[k][axis];
        }
        candidate.points[8+5*eye]=eyeOrigin;
        const auto& f=views.frusta[eye];
        for (size_t corner=0;corner<4;++corner) {
            const double x=(corner&1)?f.right:f.left,y=(corner&2)?f.up:f.down;
            auto& p=candidate.points[9+5*eye+corner];
            for (size_t axis=0;axis<3;++axis)
                p[axis]=eyeOrigin[axis]+candidate.coverage.farDistance*
                    (x*posed[0][axis]+y*posed[1][axis]-posed[2][axis]);
        }
    }
    for (size_t point=0;point<candidate.points.size();++point) {
        for (const double value:candidate.points[point])
            if (!std::isfinite(value) || std::abs(value)>1e6) return false;
        if (point<8) continue;
        double minimum=std::numeric_limits<double>::infinity();
        for (size_t plane=0;plane<6;++plane) {
            double side=data[4*plane+3];
            for (size_t axis=0;axis<3;++axis) side+=double(data[4*plane+axis])*candidate.points[point][axis];
            minimum=(std::min)(minimum,side);
        }
        ++candidate.coverage.eyePoints;
        if (minimum<-.02) ++candidate.coverage.outsideNative;
        candidate.coverage.minimumNativeSide=(std::min)(candidate.coverage.minimumNativeSide,minimum);
        if ((point-8)%5) {
            if (minimum<-.02) ++candidate.coverage.farCornersOutsideNative;
            candidate.coverage.minimumFarNativeSide=(std::min)(candidate.coverage.minimumFarNativeSide,minimum);
        }
    }
    result=candidate;return true;
}
}

// Read-only metric for the original-volume control. It never builds or alters
// a native volume. Eye origins can be outside the native near plane by design.
inline bool MeasureTrackedVisibility(const VisibilityVolume& volume,const SceneView& visibility,
                                     const SceneView& render,const SceneViews& views,
                                     TrackedVisibilityCoverage& output) {
    tracked_visibility_detail::Geometry geometry;
    if (!tracked_visibility_detail::Prepare(volume,visibility,render,views,geometry)) return false;
    output=geometry.coverage;return true;
}
inline bool MakeTrackedVisibilityBox(const VisibilityVolume& volume,const SceneView& visibility,
                                    const SceneView& render,const SceneViews& views,
                                    TrackedVisibilityBox& output,TrackedVisibilityCoverage* coverage=nullptr) {
    using namespace tracked_visibility_detail;
    Geometry geometry;
    if (!Prepare(volume,visibility,render,views,geometry)) return false;
    TrackedVisibilityBox candidate;candidate.axes=geometry.axes;
    Point low{},high{};
    for (size_t axis=0;axis<3;++axis) {
        low[axis]=std::numeric_limits<double>::infinity();high[axis]=-low[axis];
        for (const auto& p:geometry.points) {
            const double value=Dot(p,candidate.axes[axis]);
            low[axis]=(std::min)(low[axis],value);high[axis]=(std::max)(high[axis],value);
        }
        candidate.halfExtent[axis]=(high[axis]-low[axis])*.5+trackedVisibilityMargin;
        if (!std::isfinite(candidate.halfExtent[axis]) || candidate.halfExtent[axis]<trackedVisibilityMargin ||
            candidate.halfExtent[axis]>25008) return false;
        for (size_t i=0;i<3;++i) candidate.center[i]+=(high[axis]+low[axis])*.5*candidate.axes[axis][i];
    }
    // Native FrustumBuild consumes row-vector world-to-clip, with all three
    // cube coordinates in [-1,1]. Reverse Z as in the existing cube helper.
    for (size_t axis=0;axis<3;++axis) {
        const double scale=(axis==2?-1.:1.)/candidate.halfExtent[axis];
        for (size_t row=0;row<3;++row) candidate.matrix[4*row+axis]=static_cast<float>(candidate.axes[axis][row]*scale);
        candidate.matrix[12+axis]=static_cast<float>(-Dot(candidate.center,candidate.axes[axis])*scale);
    }
    candidate.matrix[15]=1;
    for (float value:candidate.matrix) if (!std::isfinite(value)) return false;
    // Check the matrix after its required float conversion, not just the
    // ideal double box. Refuse numerical cancellation that eats its margin.
    for (const auto& p:geometry.points) for (size_t axis=0;axis<3;++axis) {
        double clip=candidate.matrix[12+axis];
        for (size_t row=0;row<3;++row) clip+=p[row]*candidate.matrix[4*row+axis];
        if (!std::isfinite(clip) || std::abs(clip)>=1) return false;
    }
    output=candidate;if (coverage) *coverage=geometry.coverage;return true;
}

inline bool ValidateTrackedVisibilityBox(const VisibilityVolume& volume,const TrackedVisibilityBox& box) {
    using namespace tracked_visibility_detail;
    for (float value:box.matrix) if (!std::isfinite(value)) return false;
    if (box.matrix[3]!=0 || box.matrix[7]!=0 || box.matrix[11]!=0 || box.matrix[15]!=1) return false;
    for (size_t axis=0;axis<3;++axis) {
        if (!std::isfinite(box.center[axis]) || std::abs(box.center[axis])>1e6 ||
            !std::isfinite(box.halfExtent[axis]) || box.halfExtent[axis]<trackedVisibilityMargin ||
            box.halfExtent[axis]>25008) return false;
        for (double value:box.axes[axis]) if (!std::isfinite(value)) return false;
        for (size_t other=0;other<3;++other)
            if (std::abs(Dot(box.axes[axis],box.axes[other])-(axis==other?1.:0.))>1e-8) return false;
    }
    if (Dot(Cross(box.axes[0],box.axes[1]),box.axes[2])<.999999) return false;
    std::array<float,56> data{};memcpy(data.data(),volume.data(),volume.size());
    unsigned cornerMask{},planeMask{};
    for (size_t i=0;i<8;++i) {
        Point delta{};unsigned key{};
        for (size_t axis=0;axis<3;++axis) {
            const double value=data[24+4*i+axis];
            if (!std::isfinite(value)) return false;
            delta[axis]=value-box.center[axis];
        }
        for (size_t axis=0;axis<3;++axis) {
            const double value=Dot(delta,box.axes[axis]);
            if (std::abs(std::abs(value)-box.halfExtent[axis])>.02) return false;
            if (value>0) key|=1u<<axis;
        }
        if (cornerMask&(1u<<key)) return false;
        cornerMask|=1u<<key;
    }
    for (size_t plane=0;plane<6;++plane) {
        Point normal{};
        for (size_t axis=0;axis<3;++axis) normal[axis]=data[4*plane+axis];
        const double distance=data[4*plane+3];
        if (!std::isfinite(distance)) return false;
        for (double value:normal) if (!std::isfinite(value)) return false;
        if (std::abs(Dot(normal,normal)-1)>1e-4) return false;
        unsigned key=6;
        for (unsigned axis=0;axis<3;++axis) {
            const double alignment=Dot(normal,box.axes[axis]);
            if (std::abs(std::abs(alignment)-1)<1e-4) {
                if (key!=6 || std::abs(distance+Dot(normal,box.center)-box.halfExtent[axis])>.02) return false;
                key=2*axis+(alignment>0?1:0);
            } else if (std::abs(alignment)>1e-4) return false;
        }
        if (key==6 || (planeMask&(1u<<key))) return false;
        planeMask|=1u<<key;
    }
    return cornerMask==255 && planeMask==63;
}
}
