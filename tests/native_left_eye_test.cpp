#include "scene_pose.h"
#include <cstdio>
using namespace grid2vr;
namespace {
// The native camera the game builds from a view: world rows right/up/back/position,
// and its inverse, as ApplyScenePose checks them.
CameraContext CameraFromView(const SceneView& view) {
    std::array<float,3> right{},up{},back{},position{};
    memcpy(right.data(),view.data()+0x20,12);memcpy(up.data(),view.data()+0x10,12);
    memcpy(back.data(),view.data()+0x30,12);memcpy(position.data(),view.data()+0x40,12);
    for (size_t i=0;i<3;++i) {right[i]=-right[i];back[i]=-back[i];}
    const std::array<std::array<float,3>,3> rows{right,up,back};
    float world[16]{},inverse[16]{};
    for (size_t r=0;r<3;++r) for (size_t c=0;c<3;++c) {world[4*r+c]=rows[r][c];inverse[4*c+r]=rows[r][c];}
    for (size_t c=0;c<3;++c) {
        world[12+c]=position[c];
        double t{};for (size_t k=0;k<3;++k) t-=double(position[k])*rows[c][k];
        inverse[12+c]=static_cast<float>(t);
    }
    world[15]=inverse[15]=1;
    CameraContext camera{};
    memcpy(camera.data()+0x70,world,64);memcpy(camera.data()+0x150,inverse,64);
    return camera;
}
SceneView View(const std::array<float,3>& up,const std::array<float,3>& negativeRight,const std::array<float,3>& forward,
               const std::array<float,3>& position) {
    SceneView view{};view.fill(0x3a);
    memcpy(view.data()+0x10,up.data(),12);memcpy(view.data()+0x20,negativeRight.data(),12);
    memcpy(view.data()+0x30,forward.data(),12);memcpy(view.data()+0x40,position.data(),12);
    return view;
}
// Axes agree to float precision; positions near 1560 units to a few float steps.
bool Close(const SceneView& a,const SceneView& b) {
    for (size_t offset=0x10;offset<0x4c;offset+=4) {
        if (offset==0x1c || offset==0x2c || offset==0x3c) continue;
        float x{},y{};memcpy(&x,a.data()+offset,4);memcpy(&y,b.data()+offset,4);
        if (std::abs(x-y)>(offset>=0x40?5e-4f:2e-6f)) {std::printf("offset 0x%zx: %.9g vs %.9g\n",offset,x,y);return false;}
    }
    for (size_t i=0;i<a.size();++i)
        if ((i<0x10 || i>=0x4c || i%16>=12) && a[i]!=b[i]) return false;
    return true;
}
}
int main() {
    // A recorded Quest 3 pair (run 20261031-003728-363, frame 22686): one head
    // orientation, eyes 63.4 mm apart in native units.
    const ScenePose left{{.00515590096f,-.0153557686f,-.0200766698f,-.999667227f},{-.0401612893f,-.00331564806f,.0119402092f}};
    const ScenePose right{{.00515590096f,-.0153557686f,-.0200766698f,-.999667227f},{.0232875366f,-.000775614753f,.00997660216f}};
    ScenePose relative{};
    if (!RelativeScenePose(left,right,relative)) return 1;
    // Same orientation: identity relative rotation, separation along the eye's own right axis.
    if (std::abs(std::abs(relative.orientation[3])-1)>1e-6f) return 2;
    const double separation=std::hypot(double(relative.position[0]),relative.position[1],relative.position[2]);
    if (std::abs(separation-.0634)>.0005 || relative.position[0]<.063f) return 3;

    // A yawed native camera far from the origin, as at Chicago.
    const auto center=View({0,1,0},{0,0,1},{-1,0,0},{1559.7655f,12.5f,-830.25f});
    const auto centerCamera=CameraFromView(center);
    SceneView viaView{},viaCamera{};
    // The Prepare-entry swap has no current camera; it must match the camera path exactly.
    if (!PoseSceneView(center,left,viaView) || !ApplyScenePose(centerCamera,center,left,viaCamera) || viaView!=viaCamera) return 4;
    // Posing the game's left camera by the relative pose reproduces today's right eye.
    SceneView rightToday{},rightFromLeft{};
    if (!ApplyScenePose(centerCamera,center,right,rightToday) ||
        !ApplyScenePose(CameraFromView(viaView),viaView,relative,rightFromLeft)) return 5;
    if (!Close(rightToday,rightFromLeft)) return 6;
    // Canted displays: different eye orientations.
    const ScenePose cantedLeft{{0,.0871557f,0,.9961947f},{-.032f,0,0}},cantedRight{{0,-.0871557f,0,.9961947f},{.032f,0,0}};
    SceneView cantedLeftView{},cantedToday{},cantedFromLeft{};ScenePose cantedRelative{};
    if (!RelativeScenePose(cantedLeft,cantedRight,cantedRelative) || !PoseSceneView(center,cantedLeft,cantedLeftView) ||
        !ApplyScenePose(centerCamera,center,cantedRight,cantedToday) ||
        !ApplyScenePose(CameraFromView(cantedLeftView),cantedLeftView,cantedRelative,cantedFromLeft) ||
        !Close(cantedToday,cantedFromLeft)) return 7;
    // Every quaternion branch round-trips through the axes.
    for (const auto& q:{std::array<float,4>{0,0,0,1},{1,0,0,0},{0,1,0,0},{0,0,1,0},{.5f,.5f,.5f,.5f},{.7071068f,0,.7071068f,0}}) {
        PoseRotation axes{},back{};std::array<float,4> recovered{};
        if (!PoseAxes({q,{}},axes) || !AxesPose(axes,recovered) || !PoseAxes({recovered,{}},back)) return 8;
        for (size_t i=0;i<3;++i) for (size_t j=0;j<3;++j) if (std::abs(axes[i][j]-back[i][j])>1e-6) return 9;
    }
    // Invalid tracking and unsupported views are refused without output.
    auto untouched=viaView;
    if (RelativeScenePose({{0,0,0,0},{}},right,relative) || RelativeScenePose(left,{{0,0,0,1},{3,0,0}},relative)) return 10;
    if (PoseSceneView(center,{{0,0,0,1},{3,0,0}},viaView) || viaView!=untouched) return 11;
    auto mirrored=center;const float flipped[]{1,0,0};memcpy(mirrored.data()+0x30,flipped,12);
    if (PoseSceneView(mirrored,left,viaView) || viaView!=untouched) return 12;
    auto nonFinite=center;const float bad=NAN;memcpy(nonFinite.data()+0x44,&bad,4);
    if (PoseSceneView(nonFinite,left,viaView) || viaView!=untouched) return 13;
    std::puts("Native left eye: relative eye pose, Prepare-entry view pose and camera-path equality passed.");
    return 0;
}
