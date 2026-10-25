#include "scene_pose.h"
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <utility>
using namespace grid2vr;
int main(int argc,char** argv) {
    // A native camera already yawed +90 degrees, away from the identity basis.
    const float w[]{0,0,-1,0, 0,1,0,0, 1,0,0,0, 3,4,5,1};
    const float v[]{0,0,1,0, 0,1,0,0, -1,0,0,0, 5,-4,-3,1};
    const float p[]{1.5f,0,0,0, 0,2,0,0, 0,0,-1.0002f,-1, 0,0,-.20002f,0};
    CameraContext camera{};SceneView view{},posed{};
    memcpy(camera.data()+0x70,w,64);memcpy(camera.data()+0x110,p,64);memcpy(camera.data()+0x150,v,64);
    float vp[16]{};
    for (size_t r=0;r<4;++r) for (size_t c=0;c<4;++c)
        for (size_t k=0;k<4;++k) vp[4*r+c]+=v[4*r+k]*p[4*k+c];
    memcpy(camera.data()+0x190,vp,64);
    view.fill(0x3a);
    const float up[]{0,1,0},negativeRight[]{0,0,1},forward[]{-1,0,0};
    memcpy(view.data()+0x10,up,12);memcpy(view.data()+0x20,negativeRight,12);
    memcpy(view.data()+0x30,forward,12);memcpy(view.data()+0x40,w+12,12);
    // This quaternion cycles X->Y, Y->Z, Z->X. Translation remains in the
    // original camera coordinates: it must not be rotated a second time.
    ScenePose pose{{.5f,.5f,.5f,.5f},{.125f,.25f,-.125f}};
    if (!ApplyScenePose(camera,view,pose,posed)) return 1;
    const float expectedUp[]{1,0,0},expectedNegativeRight[]{0,-1,0},expectedForward[]{0,0,1};
    const float expectedPosition[]{2.875f,4.25f,4.875f};
    for (auto pair:{std::pair<size_t,const float*>{0x10,expectedUp},{0x20,expectedNegativeRight},
                    {0x30,expectedForward},{0x40,expectedPosition}}) {
        float actual[3]{};memcpy(actual,posed.data()+pair.first,12);
        for (size_t i=0;i<3;++i) if (std::abs(actual[i]-pair.second[i])>1e-6f) return 2;
    }
    for (size_t i=0;i<view.size();++i)
        if (!(i>=0x10 && i<0x1c) && !(i>=0x20 && i<0x2c) && !(i>=0x30 && i<0x3c) &&
            !(i>=0x40 && i<0x4c) && view[i]!=posed[i]) return 3;
    auto opposite=pose;for (auto& value:opposite.orientation) value=-value;
    SceneView equivalent{};
    if (!ApplyScenePose(camera,view,opposite,equivalent) || equivalent!=posed) return 4;
    const auto good=posed;
    for (size_t i=0;i<7;++i) {
        auto bad=pose;
        if (i<4) bad.orientation[i]=NAN;else bad.position[i-4]=INFINITY;
        if (ApplyScenePose(camera,view,bad,posed) || posed!=good) return 5;
    }
    for (ScenePose bad:{ScenePose{{0,0,0,0},{}},ScenePose{{0,0,0,2},{}},ScenePose{{0,0,0,1},{1.5f,1.5f,0}}})
        if (ApplyScenePose(camera,view,bad,posed) || posed!=good) return 6;
    auto wrong=view;const float invalid=1;memcpy(wrong.data()+0x10,&invalid,4);
    if (ApplyScenePose(camera,wrong,pose,posed) || posed!=good) return 7;
    auto mismatched=camera;memset(mismatched.data()+0xa0,0,12);
    if (ApplyScenePose(mismatched,view,pose,posed) || posed!=good) return 8;
    HistoryState entry{},prepared{};prepared.fill(0x57);const auto untouched=prepared;
    memcpy(entry.data()+0x10,vp,64);
    if (!PreparePoseHistory(camera,view,entry,pose,prepared)) return 9;
    // Independently inverted expected world camera: right=Y, up=X, back=-Z.
    const double eyeV[]{0,1,0,0, 1,0,0,0, 0,0,-1,0, -4.25,-2.875,4.875,1};
    float actual[16]{};memcpy(actual,prepared.data()+0x50,64);
    for (size_t r=0;r<4;++r) for (size_t c=0;c<4;++c) {
        double expected{};for (size_t k=0;k<4;++k) expected+=eyeV[4*r+k]*p[4*k+c];
        if (std::abs(actual[4*r+c]-expected)>1e-6) return 10;
    }
    for (size_t i=0;i<prepared.size();++i)
        if ((i<0x50 || i>=0x90) && prepared[i]!=untouched[i]) return 11;
    const auto goodHistory=prepared;entry[0x10]^=1;
    if (PreparePoseHistory(camera,view,entry,pose,prepared) || prepared!=goodHistory) return 12;
    entry[0x10]^=1;
    if (!PreparePoseHistory(camera,view,entry,pose,prepared,&diagnosticFrustum)) return 13;
    // Invalid projection must not partially overwrite history.
    const auto asymHistory=prepared;auto invalidCamera=camera;
    const float nan=NAN;memcpy(invalidCamera.data()+0x110,&nan,4);
    if (PreparePoseHistory(invalidCamera,view,entry,pose,prepared) || prepared!=asymHistory) return 14;
    std::array<ScenePose,2> eyes{};
    if (!SeparateSceneEyes(pose,.064f,eyes)) return 15;
    // The rotated local right is +Y, so only the Y eye coordinate separates.
    for (size_t i=0;i<2;++i) {
        if (eyes[i].orientation!=pose.orientation || eyes[i].position[0]!=pose.position[0] ||
            eyes[i].position[2]!=pose.position[2] ||
            std::abs(eyes[i].position[1]-(.25f+(i?.032f:-.032f)))>1e-7f) return 16;
    }
    const auto goodEyes=eyes;
    for (float separation:{0.f,-.064f,.2f,NAN})
        if (SeparateSceneEyes(pose,separation,eyes) || eyes[0].position!=goodEyes[0].position || eyes[1].position!=goodEyes[1].position) return 17;
    if (SeparateSceneEyes(ScenePose{{0,0,0,1},{float(maxSceneTranslation),0,0}},.064f,eyes)) return 18;
    SceneView standingView{};
    if (!ApplyScenePose(camera,view,ScenePose{{0,0,0,1},{.2f,1.1f,-.4f}},standingView)) return 30;
    const float standingExpected[]{2.6f,5.1f,4.8f};
    for (size_t i=0;i<3;++i) {
        float value{};memcpy(&value,standingView.data()+0x40+4*i,4);
        if (std::abs(value-standingExpected[i])>1e-6f) return 31;
    }
    // Native transitions can carry small affine scale. Preserve it in the
    // posed world matrix, and use its true inverse for bootstrap history.
    auto affine=camera;auto affineView=view;
    // Slightly more contraction than the captured Brands Hatch frame 5834
    // (~.999345 singular scale), retaining the true inverse and history.
    constexpr float scale=.9993f;
    float aw[16],av[16];memcpy(aw,w,64);memcpy(av,v,64);
    for (size_t row=0;row<3;++row) for (size_t col=0;col<3;++col) aw[4*row+col]*=scale;
    for (size_t row=0;row<4;++row) for (size_t col=0;col<3;++col) av[4*row+col]/=scale;
    auto install=[&](CameraContext& target,SceneView& targetView) {
        memcpy(target.data()+0x70,aw,64);memcpy(target.data()+0x150,av,64);
        for (size_t row=0;row<3;++row) {
            constexpr size_t offsets[]{0x20,0x10,0x30};constexpr float signs[]{-1,1,-1};
            float axis[3]{};for (size_t col=0;col<3;++col) axis[col]=aw[4*row+col]*signs[row];
            memcpy(targetView.data()+offsets[row],axis,12);
        }
    };
    install(affine,affineView);
    if (!ApplyScenePose(affine,affineView,pose,posed)) return 19;
    for (auto pair:{std::pair<size_t,const float*>{0x10,expectedUp},{0x20,expectedNegativeRight},{0x30,expectedForward}}) {
        float actualAxis[3]{};memcpy(actualAxis,posed.data()+pair.first,12);
        for (size_t i=0;i<3;++i) if (std::abs(actualAxis[i]-pair.second[i]*scale)>1e-7f) return 20;
    }
    if (!PreparePoseHistory(affine,affineView,entry,pose,prepared)) return 21;
    float target[3]{};memcpy(target,posed.data()+0x40,12);
    const double inverseEye[]{0,1./scale,0,0, 1./scale,0,0,0, 0,0,-1./scale,0,
        -double(target[1])/scale,-double(target[0])/scale,double(target[2])/scale,1};
    memcpy(actual,prepared.data()+0x50,64);
    for (size_t row=0;row<4;++row) for (size_t col=0;col<4;++col) {
        double expected{};for (size_t k=0;k<4;++k) expected+=inverseEye[4*row+k]*p[4*k+col];
        if (std::abs(actual[4*row+col]-expected)>1e-6) return 22;
    }
    const auto affineGood=posed;
    auto badInverse=affine;const float broken=1;memcpy(badInverse.data()+0x150,&broken,4);
    if (ApplyScenePose(badInverse,affineView,pose,posed) || posed!=affineGood) return 23;
    for (float badScale:{.9989f,.99f,0.f,-1.f,1.001f}) {
        memcpy(aw,w,64);memcpy(av,v,64);
        for (size_t row=0;row<3;++row) for (size_t col=0;col<3;++col) aw[4*row+col]*=badScale;
        if (badScale) for (size_t row=0;row<4;++row) for (size_t col=0;col<3;++col) av[4*row+col]/=badScale;
        install(affine,affineView);
        if (ApplyScenePose(affine,affineView,pose,posed) || posed!=affineGood) return 24;
    }
    // Exact native transition that tripped the former orthonormality guard:
    // diagnostic-20261001-165816-139, scene 194, unmodified center camera.
    if (argc!=5) return 25;
    std::ifstream nativeCamera(argv[1],std::ios::binary),nativeView(argv[2],std::ios::binary);
    CameraContext captured{};SceneView capturedView{};
    if (!nativeCamera.read(reinterpret_cast<char*>(captured.data()),captured.size()) ||
        !nativeView.read(reinterpret_cast<char*>(capturedView.data()),capturedView.size()) ||
        nativeCamera.peek()!=EOF || nativeView.peek()!=EOF ||
        !ApplyScenePose(captured,capturedView,pose,posed)) return 26;
    // Exact normal-practice camera rejected by the former absolute inverse
    // guard: diagnostic-20261014-180208-642, frame 1011. Translation cancellation
    // leaves 0.000224945 residual at a 1559-unit world coordinate.
    std::ifstream practiceCamera(argv[3],std::ios::binary),practiceView(argv[4],std::ios::binary);
    if (!practiceCamera.read(reinterpret_cast<char*>(captured.data()),captured.size()) ||
        !practiceView.read(reinterpret_cast<char*>(capturedView.data()),capturedView.size()) ||
        practiceCamera.peek()!=EOF || practiceView.peek()!=EOF ||
        !ApplyScenePose(captured,capturedView,pose,posed)) return 27;
    memcpy(entry.data()+0x10,captured.data()+0x190,64);
    if (!PreparePoseHistory(captured,capturedView,entry,pose,prepared,&diagnosticFrustum)) return 28;
    const auto practiceGood=posed;
    // Rounding allowance must not admit a mismatched inverse translation or
    // basis; rejection still leaves the caller's output bytes untouched.
    for (auto change:{std::pair<size_t,float>{0x180,.01f},{0x180,-.01f},{0x150,.001f}}) {
        auto bad=captured;float value{};memcpy(&value,bad.data()+change.first,4);
        value+=change.second;memcpy(bad.data()+change.first,&value,4);
        if (ApplyScenePose(bad,capturedView,pose,posed) || posed!=practiceGood) return 29;
    }
    // Exact first-eye startup rejection, diagnostic-20261024-154914-467.
    // The native inverse is sound; source/native Y differ by two float ULPs
    // at -10000. Keep the source origin and requested eye displacement intact.
    const auto fixtures=std::filesystem::path(argv[1]).parent_path();
    std::ifstream startupCamera(fixtures/"native-startup-1236-context.bin",std::ios::binary),
        startupView(fixtures/"native-startup-1236-view.bin",std::ios::binary),
        startupPose(fixtures/"native-startup-1236-pose.bin",std::ios::binary);
    ScenePose capturedPose{};
    if (!startupCamera.read(reinterpret_cast<char*>(captured.data()),captured.size()) || startupCamera.peek()!=EOF ||
        !startupView.read(reinterpret_cast<char*>(capturedView.data()),capturedView.size()) || startupView.peek()!=EOF ||
        !startupPose.read(reinterpret_cast<char*>(&capturedPose),sizeof capturedPose) || startupPose.peek()!=EOF ||
        !ApplyScenePose(captured,capturedView,capturedPose,posed)) return 32;
    float sourceY{},nativeY{};
    memcpy(&sourceY,capturedView.data()+0x44,4);memcpy(&nativeY,captured.data()+0xa4,4);
    if (double(sourceY)-nativeY!=.001953125) return 33;
    const auto startupGood=posed;
    for (size_t axis=0;axis<3;++axis) {
        float sourcePosition{},actualPosition{};memcpy(&sourcePosition,capturedView.data()+0x40+4*axis,4);
        double expected=sourcePosition;
        constexpr size_t offsets[]{0x20,0x10,0x30};constexpr double signs[]{-1,1,-1};
        for (size_t k=0;k<3;++k) {
            float basisValue{};memcpy(&basisValue,capturedView.data()+offsets[k]+4*axis,4);
            expected+=double(capturedPose.position[k])*signs[k]*basisValue;
        }
        memcpy(&actualPosition,posed.data()+0x40+4*axis,4);
        if (actualPosition!=static_cast<float>(expected)) return 34;
    }
    for (size_t i=0;i<capturedView.size();++i)
        if (!(i>=0x10 && i<0x1c) && !(i>=0x20 && i<0x2c) && !(i>=0x30 && i<0x3c) &&
            !(i>=0x40 && i<0x4c) && capturedView[i]!=posed[i]) return 35;
    auto roundedView=capturedView;
    float rounded=nativeY;
    for (unsigned steps=1;steps<=3;++steps) {
        rounded=std::nextafter(rounded,std::numeric_limits<float>::infinity());
        memcpy(roundedView.data()+0x44,&rounded,4);
        posed=startupGood;
        if (ApplyScenePose(captured,roundedView,capturedPose,posed)!=(steps<=2) ||
            (steps==3 && posed!=startupGood)) return 36;
    }
    memcpy(entry.data()+0x10,captured.data()+0x190,64);
    if (!PreparePoseHistory(captured,capturedView,entry,capturedPose,prepared,&diagnosticFrustum)) return 37;
    // The former ordinary-coordinate bound is unchanged.
    for (float difference:{.0009f,.0011f}) {
        auto nearby=view;const float x=3+difference;memcpy(nearby.data()+0x40,&x,4);
        posed=startupGood;
        if (ApplyScenePose(camera,nearby,pose,posed)!=(difference<.001f) ||
            (difference>.001f && posed!=startupGood)) return 38;
    }
    auto zeroCamera=camera;auto zeroView=view;
    memset(zeroCamera.data()+0xa0,0,12);memset(zeroCamera.data()+0x180,0,12);memset(zeroView.data()+0x40,0,12);
    const float ordinaryBoundary=.001f;memcpy(zeroView.data()+0x40,&ordinaryBoundary,4);
    if (!ApplyScenePose(zeroCamera,zeroView,pose,posed)) return 42;
    // A coarser float grid must not exceed the absolute .01-unit ceiling.
    auto farCamera=camera;auto farView=view;
    const float distantPosition[]{65536,65536,65536},distantInverse[]{65536,-65536,-65536};
    memcpy(farCamera.data()+0xa0,distantPosition,12);memcpy(farCamera.data()+0x180,distantInverse,12);
    memcpy(farView.data()+0x40,distantPosition,12);
    rounded=65536;
    for (unsigned steps=1;steps<=2;++steps) {
        rounded=std::nextafter(rounded,std::numeric_limits<float>::infinity());
        memcpy(farView.data()+0x44,&rounded,4);posed=startupGood;
        if (ApplyScenePose(farCamera,farView,pose,posed)!=(steps==1) ||
            (steps==2 && posed!=startupGood)) return 39;
    }
    for (float invalidPosition:{NAN,INFINITY,100001.f,-100001.f}) {
        auto invalidView=capturedView;memcpy(invalidView.data()+0x40,&invalidPosition,4);posed=startupGood;
        if (ApplyScenePose(captured,invalidView,capturedPose,posed) || posed!=startupGood) return 40;
    }
    for (auto change:{std::pair<size_t,float>{0x180,.02f},{0x180,-.02f},{0x150,.001f}}) {
        auto bad=captured;float value{};memcpy(&value,bad.data()+change.first,4);
        value+=change.second;memcpy(bad.data()+change.first,&value,4);posed=startupGood;
        if (ApplyScenePose(bad,capturedView,capturedPose,posed) || posed!=startupGood) return 41;
    }
    puts("Full orientation, camera-local translation, inverse history and fail-closed byte ownership pass.");
}
