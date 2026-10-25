#pragma once
#include "tracked_visibility.h"

namespace grid2vr {
// Diagnostic only: this volume covers the frozen eyes, not the original
// screen camera. It must not become the normal default without validation.
struct EyeVisibilityFrustum {
    std::array<double,3> origin{};
    std::array<std::array<double,3>,3> axes{}; // Orthonormal right/up/back.
    std::array<double,4> slopes{}; // Left, right, down, up.
    double nearDistance{},farDistance{};
    std::array<std::array<double,3>,10> eyePoints{};
    std::array<std::array<double,4>,6> planes{}; // Inward, extracted from float matrix.
    alignas(16) std::array<float,16> matrix{};
};
inline constexpr double eyeVisibilityMargin=.25;
// Float far-plane reconstruction is sensitive to far^2/near. Eight units work
// at Brands Hatch's ~732-unit depth, but Chicago's ~1501-unit depth produced a
// .031025 corner residual against the .03 guard. Scale construction retreat
// with depth squared above 750; frozen rendering poses and acceptance bounds
// remain unchanged. This improves conditioning, not a substitute for readback.
namespace eye_visibility_detail {
using tracked_visibility_detail::Point;
using tracked_visibility_detail::Dot;
using tracked_visibility_detail::Cross;
using tracked_visibility_detail::Normalize;
inline bool ExtractPlanes(const std::array<float,16>& matrix,std::array<std::array<double,4>,6>& output) {
    for (float value:matrix) if (!std::isfinite(value)) return false;
    std::array<std::array<double,4>,6> planes{};
    for (size_t axis=0;axis<3;++axis) for (size_t side=0;side<2;++side) {
        auto& plane=planes[axis*2+side];
        const double sign=side?-1.:1.;
        for (size_t row=0;row<4;++row) plane[row]=double(matrix[row*4+3])+sign*matrix[row*4+axis];
        const double length=std::sqrt(plane[0]*plane[0]+plane[1]*plane[1]+plane[2]*plane[2]);
        if (!std::isfinite(length) || length<1e-9) return false;
        for (double& value:plane) value/=length;
    }
    output=planes;return true;
}
inline double Side(const std::array<double,4>& plane,const Point& point) {
    return plane[3]+plane[0]*point[0]+plane[1]*point[1]+plane[2]*point[2];
}
inline bool BuildMatrix(EyeVisibilityFrustum& candidate) {
    std::array<double,16> view{},projection{};
    for (size_t col=0;col<3;++col) {
        for (size_t row=0;row<3;++row) view[row*4+col]=candidate.axes[col][row];
        view[12+col]=-Dot(candidate.origin,candidate.axes[col]);
    }
    view[15]=1;
    const auto& s=candidate.slopes;
    const double n=candidate.nearDistance,f=candidate.farDistance;
    projection[0]=2/(s[1]-s[0]);projection[5]=2/(s[3]-s[2]);
    projection[8]=(s[1]+s[0])/(s[1]-s[0]);projection[9]=(s[3]+s[2])/(s[3]-s[2]);
    // The existing native FrustumBuild uses clip Z [-1,1].
    projection[10]=-(f+n)/(f-n);projection[11]=-1;projection[14]=-2*f*n/(f-n);
    for (size_t row=0;row<4;++row) for (size_t col=0;col<4;++col) {
        double value{};
        for (size_t k=0;k<4;++k) value+=view[row*4+k]*projection[k*4+col];
        candidate.matrix[row*4+col]=static_cast<float>(value);
    }
    return ExtractPlanes(candidate.matrix,candidate.planes);
}
}
inline bool MakeEyeVisibilityFrustum(const VisibilityVolume& volume,const SceneView& visibility,
                                    const SceneView& render,const SceneViews& views,EyeVisibilityFrustum& output) {
    using namespace eye_visibility_detail;
    tracked_visibility_detail::Geometry geometry;
    if (!tracked_visibility_detail::Prepare(volume,visibility,render,views,geometry)) return false;
    EyeVisibilityFrustum candidate;
    std::copy(geometry.points.begin()+8,geometry.points.end(),candidate.eyePoints.begin());
    PoseRotation rotation{};
    if (!PoseAxes(views.poses[0],rotation)) return false;
    constexpr size_t offsets[]{0x20,0x10,0x30};constexpr double signs[]{-1,1,-1};
    for (size_t row=0;row<3;++row) for (size_t axis=0;axis<3;++axis)
        for (size_t k=0;k<3;++k) {
            float native{};memcpy(&native,render.data()+offsets[k]+4*axis,4);
            candidate.axes[row][axis]+=rotation[row][k]*signs[k]*native;
        }
    if (!Normalize(candidate.axes[0])) return false;
    const double projection=Dot(candidate.axes[1],candidate.axes[0]);
    for (size_t axis=0;axis<3;++axis) candidate.axes[1][axis]-=projection*candidate.axes[0][axis];
    if (!Normalize(candidate.axes[1])) return false;
    candidate.axes[2]=Cross(candidate.axes[0],candidate.axes[1]);
    if (!Normalize(candidate.axes[2])) return false;
    const double depthScale=(std::max)(1.,geometry.coverage.farDistance/750.);
    const double apexRetreat=8.*depthScale*depthScale;
    for (size_t axis=0;axis<3;++axis)
        candidate.origin[axis]=(candidate.eyePoints[0][axis]+candidate.eyePoints[5][axis])*.5+
            apexRetreat*candidate.axes[2][axis];
    candidate.nearDistance=std::numeric_limits<double>::infinity();
    candidate.slopes={std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),
                      std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()};
    // Budget .05 for construction/readback: this exceeds the .03 spatial
    // readback bound. Actual native-eye containment still requires .25.
    constexpr double supportMargin=eyeVisibilityMargin+.05;
    for (const auto& point:candidate.eyePoints) {
        Point delta{};for (size_t axis=0;axis<3;++axis) delta[axis]=point[axis]-candidate.origin[axis];
        const double depth=-Dot(delta,candidate.axes[2]);
        if (!std::isfinite(depth) || depth<=supportMargin+.05 || depth>50000) return false;
        candidate.nearDistance=(std::min)(candidate.nearDistance,depth-supportMargin);
        candidate.farDistance=(std::max)(candidate.farDistance,depth+supportMargin);
        for (size_t axis=0;axis<2;++axis) {
            const double coordinate=Dot(delta,candidate.axes[axis]);
            // Tangents to a radius-margin sphere around this point. Expanding
            // tangent ratios alone would give less than the requested normal
            // distance for wide FOV planes.
            const double denominator=depth*depth-supportMargin*supportMargin;
            const double root=std::sqrt(depth*depth+coordinate*coordinate-supportMargin*supportMargin);
            const double low=(coordinate*depth-supportMargin*root)/denominator;
            const double high=(coordinate*depth+supportMargin*root)/denominator;
            candidate.slopes[2*axis]=(std::min)(candidate.slopes[2*axis],low);
            candidate.slopes[2*axis+1]=(std::max)(candidate.slopes[2*axis+1],high);
        }
    }
    if (candidate.nearDistance<.05 || candidate.farDistance-candidate.nearDistance<25 ||
        candidate.farDistance>50001) return false;
    for (double value:candidate.slopes) if (!std::isfinite(value) || std::abs(value)>8) return false;
    if (candidate.slopes[1]-candidate.slopes[0]<.1 || candidate.slopes[3]-candidate.slopes[2]<.1) return false;
    // Perspective far depth is sensitive to float cancellation. Check the
    // actual matrix planes, adding only bounded far padding if necessary.
    bool contained=false;
    for (unsigned attempt=0;attempt<8;++attempt) {
        if (!BuildMatrix(candidate)) return false;
        contained=true;
        bool farFailure=false;
        for (const auto& point:candidate.eyePoints) for (size_t plane=0;plane<6;++plane) {
            if (Side(candidate.planes[plane],point)<eyeVisibilityMargin) {
                if (plane!=5) return false;
                contained=false;farFailure=true;
            }
        }
        if (contained) break;
        if (farFailure) candidate.farDistance+=(std::max)(1.,candidate.farDistance*.0001);
        if (candidate.farDistance>50009) return false;
    }
    if (!contained) return false;
    output=candidate;return true;
}
namespace eye_visibility_detail {
inline bool GeometryMatches(const VisibilityVolume& volume,const std::array<float,16>& matrix,
                            std::array<std::array<double,4>,6>& output) {
    std::array<std::array<double,4>,6> expected{};
    if (!ExtractPlanes(matrix,expected)) return false;
    std::array<float,56> data{};memcpy(data.data(),volume.data(),volume.size());
    std::array<std::array<double,4>,6> actualPlanes{};
    unsigned planeMask{},cornerMask{};
    for (size_t plane=0;plane<6;++plane) {
        std::array<double,4> actual{};
        for (size_t i=0;i<4;++i) {
            actual[i]=data[4*plane+i];
            if (!std::isfinite(actual[i])) return false;
        }
        const double norm=actual[0]*actual[0]+actual[1]*actual[1]+actual[2]*actual[2];
        if (std::abs(norm-1)>1e-4) return false;
        unsigned match=6;
        for (unsigned other=0;other<6;++other) {
            bool same=true;
            // Identification only, not geometric acceptance. Native world-
            // space corner reconstruction can round normals by >1e-5 while
            // its planes still contain all eyes and satisfy .03 incidence.
            // Those independent spatial checks below remain authoritative.
            for (size_t axis=0;axis<3;++axis) same= same && std::abs(actual[axis]-expected[other][axis])<1e-4;
            if (same) { if (match!=6) return false;match=other; }
        }
        if (match==6 || (planeMask&(1u<<match))) return false;
        planeMask|=1u<<match;
        actualPlanes[match]=actual;
    }
    for (size_t corner=0;corner<8;++corner) {
        Point point{};
        for (size_t axis=0;axis<3;++axis) {
            point[axis]=data[24+4*corner+axis];
            if (!std::isfinite(point[axis]) || std::abs(point[axis])>1e6) return false;
        }
        unsigned key{};
        for (size_t axis=0;axis<3;++axis) {
            const double low=Side(expected[axis*2],point),high=Side(expected[axis*2+1],point);
            if (low<-.03 || high<-.03) return false;
            const bool onLow=std::abs(low)<.03,onHigh=std::abs(high)<.03;
            if (onLow==onHigh) return false;
            // Absolute plane D differences depend on the world origin and
            // small normal rounding. Instead require each corner to lie on
            // the SAME actual and intended face, with the same .03 spatial
            // bound. This also bounds plane displacement geometrically.
            const double actualLow=Side(actualPlanes[axis*2],point),actualHigh=Side(actualPlanes[axis*2+1],point);
            if (actualLow<-.03 || actualHigh<-.03 ||
                (std::abs(actualLow)<.03)!=onLow || (std::abs(actualHigh)<.03)!=onHigh) return false;
            if (onHigh) key|=1u<<axis;
        }
        if (cornerMask&(1u<<key)) return false;
        cornerMask|=1u<<key;
    }
    if (planeMask!=63 || cornerMask!=255) return false;
    output=actualPlanes;return true;
}
}
inline bool ValidateEyeVisibilityFrustum(const VisibilityVolume& volume,const EyeVisibilityFrustum& frustum) {
    std::array<std::array<double,4>,6> actual{};
    if (!eye_visibility_detail::GeometryMatches(volume,frustum.matrix,actual)) return false;
    for (const auto& point:frustum.eyePoints) {
        for (double value:point) if (!std::isfinite(value)) return false;
        for (const auto& plane:actual)
            if (eye_visibility_detail::Side(plane,point)<eyeVisibilityMargin) return false;
    }
    return true;
}
// Retry native float corner reconstruction around the frustum origin. Large
// world translations can lose precision during inversion even when the input
// matrix and both-eye containment are sound. This changes construction only;
// the translated result must still pass the original world-space checks.
inline bool MakeLocalEyeVisibilityFrustum(const EyeVisibilityFrustum& world,EyeVisibilityFrustum& output) {
    for (double value:world.origin) if (!std::isfinite(value) || std::abs(value)>1e6) return false;
    auto local=world;
    local.origin={};
    for (auto& point:local.eyePoints) for (size_t axis=0;axis<3;++axis) {
        point[axis]-=world.origin[axis];
        if (!std::isfinite(point[axis])) return false;
    }
    if (!eye_visibility_detail::BuildMatrix(local)) return false;
    for (const auto& point:local.eyePoints) for (const auto& plane:local.planes)
        if (eye_visibility_detail::Side(plane,point)<eyeVisibilityMargin) return false;
    output=local;return true;
}
inline bool RestoreLocalEyeVisibilityVolume(const VisibilityVolume& nativeLocal,const EyeVisibilityFrustum& world,
                                           VisibilityVolume& output) {
    EyeVisibilityFrustum local;
    if (!MakeLocalEyeVisibilityFrustum(world,local) || !ValidateEyeVisibilityFrustum(nativeLocal,local)) return false;
    std::array<float,56> data{};memcpy(data.data(),nativeLocal.data(),nativeLocal.size());
    for (size_t plane=0;plane<6;++plane) {
        double distance=data[plane*4+3];
        for (size_t axis=0;axis<3;++axis) distance-=double(data[plane*4+axis])*world.origin[axis];
        data[plane*4+3]=static_cast<float>(distance);
    }
    for (size_t corner=0;corner<8;++corner) for (size_t axis=0;axis<3;++axis)
        data[24+corner*4+axis]=static_cast<float>(double(data[24+corner*4+axis])+world.origin[axis]);
    VisibilityVolume translated{};memcpy(translated.data(),data.data(),translated.size());
    if (!ValidateEyeVisibilityFrustum(translated,world)) return false;
    output=translated;return true;
}
}
