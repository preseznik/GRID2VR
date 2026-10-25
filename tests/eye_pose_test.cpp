#include "eye_view.h"
#include <cstdio>
int main() {
    constexpr float yaw=15.f*0.017453292519943295f;
    const float c=static_cast<float>(std::cos(double(yaw))),s=static_cast<float>(std::sin(double(yaw)));
    grid2vr::CameraContext camera{};
    const float w[]{1,0,0,0, 0,1,0,0, 0,0,1,0, 3,4,5,1};
    const float v[]{1,0,0,0, 0,1,0,0, 0,0,1,0, -3,-4,-5,1};
    const float p[]{1.5f,0,0,0, 0,2,0,0, 0,0,-1.0002f,-1, 0,0,-.20002f,0};
    float vp[16]{};
    for (unsigned r=0;r<4;++r) for (unsigned col=0;col<4;++col)
        for (unsigned k=0;k<4;++k) vp[4*r+col]+=v[4*r+k]*p[4*k+col];
    memcpy(camera.data()+0x70,w,64); memcpy(camera.data()+0x110,p,64);
    memcpy(camera.data()+0x150,v,64); memcpy(camera.data()+0x190,vp,64);
    grid2vr::SceneView view{},rotated{};
    const float up[]{0,1,0},negativeRight[]{-1,0,0},forward[]{0,0,-1};
    memcpy(view.data()+0x10,up,12); memcpy(view.data()+0x20,negativeRight,12);
    memcpy(view.data()+0x30,forward,12); memcpy(view.data()+0x40,w+12,12);
    view[0x8c]=0;
    if (!grid2vr::OffsetSceneView(camera,view,.064f,rotated,yaw)) return 1;
    float r[3]{},f[3]{},position[3]{};
    memcpy(r,rotated.data()+0x20,12); memcpy(f,rotated.data()+0x30,12); memcpy(position,rotated.data()+0x40,12);
    if (r[0]!=-c || r[1]!=0 || r[2]!=s || f[0]!=-s || f[1]!=0 || f[2]!=-c ||
        position[0]!=3+c*.064f || position[1]!=4 || position[2]!=5-s*.064f) return 2;
    for (size_t i=0;i<view.size();++i)
        if (!(i>=0x20 && i<0x2c) && !(i>=0x30 && i<0x3c) && !(i>=0x40 && i<0x4c) && view[i]!=rotated[i]) return 3;
    const auto good=rotated;
    auto bad=view; memset(bad.data()+0x10,0,12);
    if (grid2vr::OffsetSceneView(camera,bad,.064f,rotated,yaw) || rotated!=good) return 4;
    for (float invalid:{NAN,INFINITY,2.f})
        if (grid2vr::OffsetSceneView(camera,view,.064f,rotated,invalid) || rotated!=good) return 5;
    grid2vr::SceneView side{};
    if (!grid2vr::OffsetSceneView(camera,view,.064f,side,90.f*0.017453292519943295f)) return 15;
    float sideForward[3]{},sidePosition[3]{};
    memcpy(sideForward,side.data()+0x30,12); memcpy(sidePosition,side.data()+0x40,12);
    if (std::abs(sideForward[0]+1)>1e-6f || sideForward[1]!=0 || std::abs(sideForward[2])>1e-6f ||
        std::abs(sidePosition[0]-3)>1e-6f || sidePosition[1]!=4 || sidePosition[2]!=5-.064f) return 16;
    grid2vr::HistoryState entry{},prepared{};
    memcpy(entry.data()+0x10,vp,64); prepared.fill(0x53); const auto untouched=prepared;
    if (!grid2vr::PrepareEyeHistory(camera,view,entry,.064f,prepared,yaw)) return 6;
    // Independent camera basis: yaw +15 looks toward -X; the view matrix's
    // translation is minus the eye position dotted with each eye-world axis.
    const double cd=std::cos(double(yaw)),sd=std::sin(double(yaw));
    const double eyeV[]{cd,0,sd,0, 0,1,0,0, -sd,0,cd,0,
        -position[0]*cd+position[2]*sd,-position[1],-position[0]*sd-position[2]*cd,1};
    float seeded[16]{}; memcpy(seeded,prepared.data()+0x50,64);
    for (unsigned row=0;row<4;++row) for (unsigned col=0;col<4;++col) {
        double expected{};
        for (unsigned k=0;k<4;++k) expected+=eyeV[4*row+k]*p[4*k+col];
        if (std::abs(seeded[4*row+col]-expected)>1e-6) return 7;
    }
    for (size_t i=0;i<prepared.size();++i)
        if ((i<0x50 || i>=0x90) && prepared[i]!=untouched[i]) return 8;
    grid2vr::RenderedEyeHistory history;
    if (history.Seed(10,1,2,prepared) || !history.Store(10,1,2,camera)) return 9;
    if (!history.Seed(11,1,2,prepared) || memcmp(prepared.data()+0x50,camera.data()+0x190,64)) return 10;
    const auto saved=prepared;
    if (history.Seed(12,1,2,prepared) || history.Seed(11,3,2,prepared) || history.Seed(11,1,3,prepared) || prepared!=saved) return 11;
    auto invalid=camera; const float nan=NAN; memcpy(invalid.data()+0x190,&nan,4);
    if (history.Store(11,1,2,invalid) || history.Store(12,1,2,camera) || history.frame!=10) return 12;
    camera[0x190]^=1;
    if (!history.Store(11,1,2,camera) || !history.Seed(12,1,2,prepared) || memcmp(prepared.data()+0x50,camera.data()+0x190,64)) return 13;
    const auto beforeProjection=camera;
    if (!grid2vr::ApplyEyeFrustum(camera,grid2vr::diagnosticFrustum)) return 14;
    float projection[16]{}; memcpy(projection,camera.data()+0x110,64);
    for (size_t i=0;i<camera.size();++i) {
        const bool field=(i>=0x110 && i<0x114) || (i>=0x124 && i<0x128) || (i>=0x130 && i<0x138) || (i>=0x190 && i<0x1d0);
        if (!field && camera[i]!=beforeProjection[i]) return 15;
    }
    // Each requested edge ray must land at its own NDC edge; Z remains native.
    for (float x:{-1.f,.8f}) for (float y:{-.7f,.6f}) {
        const float clipX=x*projection[0]-projection[8],clipY=y*projection[5]-projection[9];
        if (std::abs(clipX-(x<0?-1.f:1.f))>1e-6 || std::abs(clipY-(y<0?-1.f:1.f))>1e-6 ||
            projection[10]!=p[10] || projection[11]!=p[11] || projection[14]!=p[14] || projection[15]!=p[15]) return 16;
    }
    const auto validProjection=camera;
    for (grid2vr::EyeFrustum invalidFov: {grid2vr::EyeFrustum{NAN,.8f,-.7f,.6f},{0,.8f,-.7f,.6f},{-1,8,-.7f,.6f}})
        if (grid2vr::ApplyEyeFrustum(camera,invalidFov) || camera!=validProjection) return 17;
    const float zero=0; memcpy(camera.data()+0x13c,&zero,4); const auto orthographic=camera;
    if (grid2vr::ApplyEyeFrustum(camera,grid2vr::diagnosticFrustum) || camera!=orthographic) return 18;
    puts("Native yaw basis, position, bootstrap projection and exact prior-render history with stale/identity refusal pass.");
}
