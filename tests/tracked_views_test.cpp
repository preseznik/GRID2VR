#include "tracked_views.h"
#include <cstdio>
using namespace grid2vr;
bool Close(float a,float b) {return std::abs(a-b)<.00001f;}
XrQuaternionf Multiply(XrQuaternionf a,XrQuaternionf b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
bool RecenterTests() {
    for (float yaw:{-3.14159f,-1.2f,0.f,1.2f,3.14159f}) {
        const XrQuaternionf heading{0,std::sin(yaw/2),0,std::cos(yaw/2)};
        const XrQuaternionf tilt=Multiply({std::sin(.3f),0,0,std::cos(.3f)},{0,0,std::sin(-.2f),std::cos(-.2f)});
        XrPosef head{Multiply(heading,tilt),{2,1.7f,-3}},level{},full{};
        if (!MakeRecenterReference(head,true,level) || !MakeRecenterReference(head,false,full) ||
            memcmp(&head,&full,sizeof(head)) || memcmp(&head.position,&level.position,sizeof(head.position)) ||
            level.orientation.x!=0 || level.orientation.z!=0 ||
            !Close(level.orientation.y,heading.y) || !Close(level.orientation.w,heading.w)) return false;
        std::array<XrView,2> eyes{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};
        for (unsigned i=0;i<2;++i) {
            eyes[i].pose=head;eyes[i].pose.position.x+=i?.032f:-.032f;
            eyes[i].fov={-.8f,.8f,.8f,-.8f};
        }
        TrackedSceneViews result;
        if (!MakeTrackedSceneViews(eyes,head,level,1,result)) return false;
        for (unsigned i=0;i<2;++i) {
            const auto& q=result.poses[i].orientation;
            if (!Close(q[0],tilt.x) || !Close(q[1],tilt.y) || !Close(q[2],tilt.z) || !Close(q[3],tilt.w)) return false;
        }
        // Repeating recenter does not accumulate tilt; q and -q are equivalent.
        for (auto* value:{&head.orientation.x,&head.orientation.y,&head.orientation.z,&head.orientation.w}) *value=-*value;
        XrPosef repeated{};
        if (!MakeRecenterReference(head,true,repeated) || memcmp(&level,&repeated,sizeof(level))) return false;
        if (!MakeTrackedSceneViews(eyes,full,full,1,result) || !Close(result.poses[0].orientation[3],1)) return false;
    }
    for (float sign:{-1.f,1.f}) {
        XrPosef vertical{{sign*std::sqrt(.5f),0,0,std::sqrt(.5f)},{0,1.7f,0}},out{};
        if (!MakeRecenterReference(vertical,true,out) || out.orientation.x || out.orientation.z) return false;
    }
    XrPosef invalid{{0,0,0,2},{0,0,0}},sentinel{{0,0,0,1},{1,2,3}},out=sentinel;
    return !MakeRecenterReference(invalid,true,out) && !memcmp(&sentinel,&out,sizeof(out));
}
int main() {
    if (!RecenterTests()) return 20;
    XrPosef reference{{0,0,0,1},{2,1.6f,-3}},head=reference;
    std::array<XrView,2> views{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};
    for (unsigned eye=0;eye<2;++eye) {
        views[eye].pose=reference;views[eye].pose.position.x+=eye?.032f:-.032f;
        views[eye].fov={eye?-.7f:-.94f,eye?.94f:.7f,.77f,-.96f};
    }
    TrackedSceneViews result;
    if (!MakeTrackedSceneViews(views,head,reference,1,result) || !Close(result.poses[0].position[0],-.032f) ||
        !Close(result.poses[1].position[0],.032f) || !Close(result.frusta[0].left,std::tan(-.94f)) ||
        !Close(result.frusta[1].right,std::tan(.94f))) return 1;
    // Turn the recenter pose 90 degrees about Y. Its local right axis is -Z.
    reference.orientation={0,std::sqrt(.5f),0,std::sqrt(.5f)};head=reference;
    for (unsigned eye=0;eye<2;++eye) {
        views[eye].pose=reference;views[eye].pose.position.z+=eye?-.032f:.032f;
    }
    if (!MakeTrackedSceneViews(views,head,reference,1,result) || !Close(result.poses[0].position[0],-.032f) ||
        !Close(result.poses[1].position[0],.032f) || !Close(result.poses[0].orientation[3],1) ||
        !Close(result.poses[0].orientation[1],0)) return 2;
    // Translate in tracking space; verify the recentered camera-local direction.
    head.position.x+=.1f;
    for (auto& eye:views) eye.pose.position.x+=.1f;
    if (!MakeTrackedSceneViews(views,head,reference,2,result) || !Close(result.poses[0].position[2],.2f) ||
        !Close(result.poses[1].position[0],.064f)) return 3;
    const auto saved=result;
    auto bad=views;bad[1].fov.angleRight=1.5f;
    if (MakeTrackedSceneViews(bad,head,reference,1,result) || memcmp(&result,&saved,sizeof(result))) return 4;
    bad=views;bad[0].pose.orientation.w=10;
    if (MakeTrackedSceneViews(bad,head,reference,1,result) || memcmp(&result,&saved,sizeof(result))) return 5;
    bad=views;bad[1].pose.position=bad[0].pose.position;
    if (MakeTrackedSceneViews(bad,head,reference,1,result)) return 6;
    bad=views;for (auto& eye:bad) eye.pose.position.x+=3;auto farHead=head;farHead.position.x+=3;
    if (MakeTrackedSceneViews(bad,farHead,reference,1,result) || MakeTrackedSceneViews(views,head,reference,0,result)) return 7;
    // Canted runtime eye orientations remain distinct; never replace them with
    // a symmetric synthetic IPD or a common head orientation.
    views[0].pose.orientation=reference.orientation;
    views[1].pose.orientation={0,std::sin(.80f),0,std::cos(.80f)};
    if (!MakeTrackedSceneViews(views,head,reference,1,result) || Close(result.poses[0].orientation[1],result.poses[1].orientation[1])) return 8;
    // Frame 4101 from diagnostic-20261015-094629-923. The recenter transform is
    // reconstructed from accepted frame 4100, not a directly logged reference.
    // Both tracking flags were valid; the right eye crossed the old .5 limit.
    reference={{-.0397662607f,.284532554f,-.0364187598f,-.957148653f},{.0520775305f,-.0246775196f,.0388703151f}};
    head={{.118725009f,-.304499537f,-.0193713009f,-.944885731f},{.149272129f,.179434538f,-.383153617f}};
    const std::array<XrVector3f,2> positions{{{.123510987f,.180564493f,-.364792645f},{.175033271f,.178304583f,-.401514590f}}};
    for (unsigned eye=0;eye<2;++eye) {
        views[eye].pose={head.orientation,positions[eye]};
        views[eye].fov={eye?-.69813168f:-.942477763f,eye?.942477763f:.69813168f,.767944872f,-.959931076f};
    }
    bool outside=true;
    if (!MakeTrackedSceneViews(views,head,reference,1,result,&outside) || outside || !ValidSceneViews(result)) return 9;
    double distance{};for (auto v:result.poses[1].position) distance+=double(v)*v;
    if (std::abs(std::sqrt(distance)-.500258646)>1e-6) return 10;
    // Stand up one metre. Keep the real IPD and orientation, then move beyond
    // the supported volume: no partial/stale pair may escape either outcome.
    head.position.y+=1;for (auto& eye:views) eye.pose.position.y+=1;
    if (!MakeTrackedSceneViews(views,head,reference,1,result,&outside) || outside || !ValidSceneViews(result)) return 11;
    const auto standing=result;
    SceneViewFrame lifecycle;
    if (!lifecycle.Freeze(10,result) || !lifecycle.Finish(10,true)) return 12;
    head.position.y+=2;for (auto& eye:views) eye.pose.position.y+=2;
    if (MakeTrackedSceneViews(views,head,reference,1,result,&outside) || !outside || memcmp(&result,&standing,sizeof(result))) return 13;
    if (!lifecycle.ResetEpoch()) return 14;
    auto corrupt=views;corrupt[1].fov.angleRight=2;
    if (MakeTrackedSceneViews(corrupt,head,reference,1,result,&outside) || outside || memcmp(&result,&standing,sizeof(result))) return 15;
    head.position.y-=2;for (auto& eye:views) eye.pose.position.y-=2;
    if (!MakeTrackedSceneViews(views,head,reference,1,result,&outside) || outside || !lifecycle.Freeze(20,result) || !lifecycle.Finish(20,true)) return 16;
    puts("Tracked eye mapping: recorded boundary crossing, standing, bounded excursion/return, invalid-input refusal and atomic stereo passed.");
}
