#pragma once
#include "scene_views.h"
#include <openxr/openxr.h>

namespace grid2vr {
using TrackedSceneViews=SceneViews;
inline bool TrackingOrientation(const XrQuaternionf& input,std::array<double,4>& normalized) {
    double norm{};const float values[]{input.x,input.y,input.z,input.w};
    for (float value:values) {if (!std::isfinite(value)) return false;norm+=double(value)*value;}
    if (std::abs(norm-1)>.001) return false;
    for (unsigned i=0;i<4;++i) normalized[i]=values[i]/std::sqrt(norm);
    return true;
}
inline bool TrackingPosition(const XrVector3f& input) {
    for (float value:{input.x,input.y,input.z}) if (!std::isfinite(value) || std::abs(value)>1000) return false;
    return true;
}
// Recenter position and heading without treating the user's head tilt as a
// new world horizon. Full orientation is retained only by explicit opt-out.
inline bool MakeRecenterReference(const XrPosef& head,bool keepHorizonLevel,XrPosef& reference) {
    std::array<double,4> q{};
    if (!TrackingPosition(head.position) || !TrackingOrientation(head.orientation,q)) return false;
    XrPosef candidate=head;
    if (keepHorizonLevel) {
        const auto x=q[0],y=q[1],z=q[2],w=q[3];
        double forwardX=-2*(x*z+w*y),forwardZ=-(1-2*(x*x+y*y));
        if (std::hypot(forwardX,forwardZ)<1e-6) {
            // At straight up/down use the horizontal right axis, as the
            // theater anchor does. A valid vertical pose must not stop VR.
            forwardX=2*(x*z-w*y);forwardZ=-(1-2*(y*y+z*z));
        }
        if (std::hypot(forwardX,forwardZ)<1e-6) return false;
        const double yaw=std::atan2(-forwardX,-forwardZ);
        candidate.orientation={0,static_cast<float>(std::sin(yaw*.5)),0,static_cast<float>(std::cos(yaw*.5))};
    }
    reference=candidate;return true;
}
// LOCAL-space runtime views become right/up/back offsets from the head pose
// selected at recenter. unitsPerMetre is explicit; this does not calibrate it.
// Both eyes and FOVs are produced atomically from one located sample.
inline bool MakeTrackedSceneViews(const std::array<XrView,2>& views,const XrPosef& head,
                                  const XrPosef& reference,float unitsPerMetre,TrackedSceneViews& output,
                                  bool* outOfRange=nullptr) {
    if (outOfRange) *outOfRange=false;
    if (!std::isfinite(unitsPerMetre) || unitsPerMetre<.1f || unitsPerMetre>10 ||
        !TrackingPosition(reference.position) || !TrackingPosition(head.position)) return false;
    std::array<double,4> referenceQ{},headQ{};
    if (!TrackingOrientation(reference.orientation,referenceQ) || !TrackingOrientation(head.orientation,headQ)) return false;
    ScenePose origin;
    for (unsigned i=0;i<4;++i) origin.orientation[i]=static_cast<float>(referenceQ[i]);
    PoseRotation rotation;if (!PoseAxes(origin,rotation)) return false;
    const double referencePosition[]{reference.position.x,reference.position.y,reference.position.z};
    const double headPosition[]{head.position.x,head.position.y,head.position.z};
    TrackedSceneViews candidate;
    bool rangeExceeded=false;
    double separationSquared{};
    for (unsigned eye=0;eye<2;++eye) {
        const auto& view=views[eye];std::array<double,4> q{};
        if (view.type!=XR_TYPE_VIEW || !TrackingPosition(view.pose.position) || !TrackingOrientation(view.pose.orientation,q)) return false;
        const double position[]{view.pose.position.x,view.pose.position.y,view.pose.position.z};
        double headDistance{};
        for (unsigned i=0;i<3;++i) headDistance+=(position[i]-headPosition[i])*(position[i]-headPosition[i]);
        if (headDistance>.15*.15) return false;
        const double x=-referenceQ[0],y=-referenceQ[1],z=-referenceQ[2],w=referenceQ[3];
        candidate.poses[eye].orientation={static_cast<float>(w*q[0]+x*q[3]+y*q[2]-z*q[1]),
            static_cast<float>(w*q[1]-x*q[2]+y*q[3]+z*q[0]),static_cast<float>(w*q[2]+x*q[1]-y*q[0]+z*q[3]),
            static_cast<float>(w*q[3]-x*q[0]-y*q[1]-z*q[2])};
        for (unsigned local=0;local<3;++local) {
            double value{};for (unsigned world=0;world<3;++world) value+=(position[world]-referencePosition[world])*rotation[local][world];
            candidate.poses[eye].position[local]=static_cast<float>(value*unitsPerMetre);
        }
        ScenePose rotationOnly;rotationOnly.orientation=candidate.poses[eye].orientation;
        PoseRotation checked;if (!PoseAxes(rotationOnly,checked)) return false;
        double distance{};
        for (float value:candidate.poses[eye].position) {
            if (!std::isfinite(value)) return false;
            distance+=double(value)*value;
        }
        rangeExceeded=rangeExceeded || distance>maxSceneTranslation*maxSceneTranslation;
        const float angles[]{view.fov.angleLeft,view.fov.angleRight,view.fov.angleDown,view.fov.angleUp};
        std::array<float,4> tangents{};
        for (unsigned i=0;i<4;++i) {
            if (!std::isfinite(angles[i]) || std::abs(angles[i])>=1.326f) return false;
            tangents[i]=std::tan(angles[i]);
            if (std::abs(tangents[i])>4) return false;
        }
        const auto& t=tangents;
        if (t[0]>=0 || t[1]<=0 || t[2]>=0 || t[3]<=0 || t[1]-t[0]<.1f || t[3]-t[2]<.1f) return false;
        candidate.frusta[eye]={t[0],t[1],t[2],t[3]};
    }
    const double dx=double(views[1].pose.position.x)-views[0].pose.position.x;
    const double dy=double(views[1].pose.position.y)-views[0].pose.position.y;
    const double dz=double(views[1].pose.position.z)-views[0].pose.position.z;
    separationSquared=dx*dx+dy*dy+dz*dz;
    if (separationSquared<.03*.03 || separationSquared>.09*.09) return false;
    // Only classify a distance limit after validating BOTH eyes, head, FOV and
    // separation. Bad tracking remains a failure, never a recoverable gap.
    if (rangeExceeded) { if (outOfRange) *outOfRange=true;return false; }
    output=candidate;return true;
}
}
