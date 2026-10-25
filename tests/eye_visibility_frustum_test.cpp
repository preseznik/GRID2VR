#include "eye_visibility_frustum.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
using namespace grid2vr;
namespace {
using Point=std::array<double,3>;
using Quaternion=std::array<double,4>;
void Check(bool value,int line) { if (!value) { std::fprintf(stderr,"Eye visibility check failed at line %d\n",line);std::exit(1); } }
#define CHECK(x) Check(bool(x),__LINE__)
template<class T> T Load(const std::filesystem::path& path) {
    T value{};std::ifstream file(path,std::ios::binary);
    CHECK(file.read(reinterpret_cast<char*>(value.data()),value.size()) && file.peek()==EOF);return value;
}
Point Cross(Point a,Point b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
Quaternion Multiply(Quaternion a,Quaternion b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
            a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
            a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
            a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
SceneViews Views(double yaw,double pitch,double roll) {
    constexpr double factor=3.14159265358979323846/360;
    const auto q=Multiply(Multiply({0,std::sin(yaw*factor),0,std::cos(yaw*factor)},
                                 {std::sin(pitch*factor),0,0,std::cos(pitch*factor)}),
                                 {0,0,std::sin(roll*factor),std::cos(roll*factor)});
    ScenePose pose{{float(q[0]),float(q[1]),float(q[2]),float(q[3])},{.4f,1.1f,-.3f}};
    SceneViews views;CHECK(SeparateSceneEyes(pose,.0647300034f,views.poses));
    views.frusta={EyeFrustum{-1.37638187f,.839099586f,-1.42814791f,.965688765f},
                  EyeFrustum{-.839099586f,1.37638187f,-1.42814791f,.965688765f}};
    return views;
}
Point EyePoint(const SceneView& view,const ScenePose& pose,double x,double y,double depth) {
    Point local{x*depth,y*depth,-depth},q{};
    double norm{};for (float value:pose.orientation) norm+=double(value)*value;
    for (size_t i=0;i<3;++i) q[i]=pose.orientation[i]/std::sqrt(norm);
    const auto first=Cross(q,local),second=Cross(q,first);
    for (size_t i=0;i<3;++i) local[i]+=2*(pose.orientation[3]/std::sqrt(norm)*first[i]+second[i])+pose.position[i];
    Point world{};constexpr size_t offsets[]{0x20,0x10,0x30};constexpr double signs[]{-1,1,-1};
    for (size_t axis=0;axis<3;++axis) {
        float value{};memcpy(&value,view.data()+0x40+4*axis,4);world[axis]=value;
        for (size_t row=0;row<3;++row) {
            memcpy(&value,view.data()+offsets[row]+4*axis,4);
            world[axis]+=local[row]*signs[row]*value;
        }
    }
    return world;
}
double Side(const std::array<double,4>& plane,const Point& point) {
    double side=plane[3];for (size_t i=0;i<3;++i) side+=plane[i]*point[i];return side;
}
VisibilityVolume BuildVolume(const EyeVisibilityFrustum& frustum) {
    // Independent generic 4x4 inversion, not the builder's plane intersections.
    double augmented[4][8]{};
    for (size_t row=0;row<4;++row) for (size_t col=0;col<4;++col) {
        augmented[row][col]=frustum.matrix[4*row+col];augmented[row][4+col]=row==col?1:0;
    }
    for (size_t col=0;col<4;++col) {
        size_t pivot=col;
        for (size_t row=col+1;row<4;++row) if (std::abs(augmented[row][col])>std::abs(augmented[pivot][col])) pivot=row;
        CHECK(std::abs(augmented[pivot][col])>1e-10);
        for (size_t i=0;i<8;++i) std::swap(augmented[col][i],augmented[pivot][i]);
        const double scale=augmented[col][col];
        for (double& value:augmented[col]) value/=scale;
        for (size_t row=0;row<4;++row) if (row!=col) {
            const double factor=augmented[row][col];
            for (size_t i=0;i<8;++i) augmented[row][i]-=factor*augmented[col][i];
        }
    }
    std::array<float,56> data{};
    for (size_t plane=0;plane<6;++plane) for (size_t i=0;i<4;++i) data[4*plane+i]=float(frustum.planes[plane][i]);
    for (unsigned corner=0;corner<8;++corner) {
        const double clip[]{corner&1?1.:-1.,corner&2?1.:-1.,corner&4?1.:-1.,1};
        double world[4]{};
        for (size_t col=0;col<4;++col) for (size_t row=0;row<4;++row) world[col]+=clip[row]*augmented[row][4+col];
        CHECK(std::abs(world[3])>1e-8);
        for (size_t axis=0;axis<3;++axis) data[24+4*corner+axis]=float(world[axis]/world[3]);
    }
    VisibilityVolume result{};memcpy(result.data(),data.data(),result.size());return result;
}
}
int main(int argc,char** argv) {
    CHECK(argc==4);
    const auto volume=Load<VisibilityVolume>(argv[1]);
    const auto visibility=Load<SceneView>(argv[2]),render=Load<SceneView>(argv[3]);
    double largestCubeRatio{},largestBoxRatio{};unsigned samples{},excludedOriginal{};
    for (double yaw:{0.,45.,90.,-90.,180.,-180.}) for (double pitch:{0.,40.,-40.}) for (double roll:{0.,25.}) {
        auto views=Views(yaw,pitch,roll);
        if (yaw==45) views.poses[1].orientation=Views(yaw+5,pitch-3,roll+2).poses[1].orientation;
        EyeVisibilityFrustum frustum;
        CHECK(MakeEyeVisibilityFrustum(volume,visibility,render,views,frustum));
        CHECK(frustum.nearDistance>7.6);
        CHECK(ValidateEyeVisibilityFrustum(BuildVolume(frustum),frustum));
        TrackedVisibilityCoverage coverage;
        CHECK(MeasureTrackedVisibility(volume,visibility,render,views,coverage));
        for (size_t eye=0;eye<2;++eye) {
            const auto& f=views.frusta[eye];
            for (double depth:{0.,.01,.25,.5,1.}) for (double x:{double(f.left),0.,double(f.right)})
                for (double y:{double(f.down),0.,double(f.up)}) {
                    const auto point=EyePoint(render,views.poses[eye],x,y,depth*coverage.farDistance);
                    for (const auto& plane:frustum.planes) CHECK(Side(plane,point)>=eyeVisibilityMargin-1e-8);
                    ++samples;
                }
        }
        TrackedVisibilityBox box;VisibilityBox cube;
        CHECK(MakeTrackedVisibilityBox(volume,visibility,render,views,box));
        CHECK(MakeEyeVisibilityBox(volume,visibility,render,views.frusta,cube));
        const auto& s=frustum.slopes;
        const double size=(s[1]-s[0])*(s[3]-s[2])*(std::pow(frustum.farDistance,3)-std::pow(frustum.nearDistance,3))/3;
        const double boxRatio=size/(8*box.halfExtent[0]*box.halfExtent[1]*box.halfExtent[2]),cubeRatio=size/(8*std::pow(cube.radius,3));
        largestBoxRatio=(std::max)(largestBoxRatio,boxRatio);largestCubeRatio=(std::max)(largestCubeRatio,cubeRatio);
        CHECK(boxRatio<.8 && cubeRatio<.15);
        if (yaw==180) {
            std::array<float,56> native{};memcpy(native.data(),volume.data(),volume.size());
            for (size_t point=0;point<8;++point) for (const auto& plane:frustum.planes)
                if (Side(plane,{native[24+4*point],native[25+4*point],native[26+4*point]})<0) { ++excludedOriginal;break; }
        }
    }
    CHECK(excludedOriginal>0);
    const auto directory=std::filesystem::path(argv[1]).parent_path();
    // Exact frame that stopped VDXR at Brands Hatch, 2026-10-23. Native
    // contraction must survive construction, float readback and both-eye rays.
    const auto failedVolume=Load<VisibilityVolume>(directory/"brands-hatch-5834-volume.bin");
    const auto failedSource=Load<SceneView>(directory/"brands-hatch-5834-source.bin");
    const auto failedRender=Load<SceneView>(directory/"brands-hatch-5834-render.bin");
    const auto failedEyeBytes=Load<std::array<uint8_t,88>>(directory/"brands-hatch-5834-eyes.bin");
    static_assert(sizeof(SceneViews)==88);
    SceneViews failedEyes;memcpy(&failedEyes,failedEyeBytes.data(),sizeof failedEyes);
    const auto checkCaptured=[&](const SceneViews& pair) {
        EyeVisibilityFrustum result;TrackedVisibilityCoverage coverage;
        CHECK(MakeEyeVisibilityFrustum(failedVolume,failedSource,failedRender,pair,result));
        CHECK(ValidateEyeVisibilityFrustum(BuildVolume(result),result));
        CHECK(MeasureTrackedVisibility(failedVolume,failedSource,failedRender,pair,coverage));
        for (size_t eye=0;eye<2;++eye) {
            const auto& f=pair.frusta[eye];
            for (double depth:{0.,.01,.25,.5,1.}) for (double x:{double(f.left),0.,double(f.right)})
                for (double y:{double(f.down),0.,double(f.up)}) {
                    const auto point=EyePoint(failedRender,pair.poses[eye],x,y,depth*coverage.farDistance);
                    for (const auto& plane:result.planes) CHECK(Side(plane,point)>=eyeVisibilityMargin-1e-8);
                    ++samples;
                }
        }
    };
    checkCaptured(failedEyes);
    for (double yaw:{-180.,-90.,0.,45.,90.,180.}) for (double pitch:{-40.,0.,40.})
        for (double roll:{0.,25.}) checkCaptured(Views(yaw,pitch,roll));
    for (const auto name:{"native-transition-view.bin","native-practice-view.bin",
                         "native-long-14834-center-view.bin","native-headset-6136-center-view.bin"}) {
        const auto view=Load<SceneView>(directory/name);const auto views=Views(90,30,20);
        EyeVisibilityFrustum frustum;TrackedVisibilityCoverage coverage;
        CHECK(MakeEyeVisibilityFrustum(volume,visibility,view,views,frustum));
        CHECK(ValidateEyeVisibilityFrustum(BuildVolume(frustum),frustum));
        CHECK(MeasureTrackedVisibility(volume,visibility,view,views,coverage));
        for (size_t eye=0;eye<2;++eye) for (double depth:{0.,.5,1.}) {
            const auto& f=views.frusta[eye];
            for (double x:{double(f.left),0.,double(f.right)}) for (double y:{double(f.down),0.,double(f.up)})
                for (const auto& plane:frustum.planes) CHECK(Side(plane,EyePoint(view,views.poses[eye],x,y,depth*coverage.farDistance))>=eyeVisibilityMargin-1e-8);
        }
    }
    EyeVisibilityFrustum frustum;auto views=Views(90,0,0);
    views.frusta={EyeFrustum{-4,4,-4,4},EyeFrustum{-4,4,-4,4}};
    CHECK(MakeEyeVisibilityFrustum(volume,visibility,render,views,frustum));
    CHECK(ValidateEyeVisibilityFrustum(BuildVolume(frustum),frustum));
    views=Views(0,0,0);CHECK(MakeEyeVisibilityFrustum(volume,visibility,render,views,frustum));
    const auto saved=frustum;
    for (unsigned item=0;item<6;++item) {
        auto bad=views;
        if (item==0) bad.poses[1].orientation=Views(180,0,0).poses[1].orientation;
        if (item==1) bad.poses[1].orientation=Views(90,0,0).poses[1].orientation;
        if (item==2) bad.poses[0].orientation[0]=NAN;
        if (item==3) bad.poses[1].position={0,3,0};
        if (item==4) bad.frusta[1].right=INFINITY;
        if (item==5) bad.frusta[0].left=0;
        CHECK(!MakeEyeVisibilityFrustum(volume,visibility,render,bad,frustum));
        CHECK(memcmp(&saved,&frustum,sizeof frustum)==0);
    }
    auto badView=render;memset(badView.data()+0x10,0,12);
    CHECK(!MakeEyeVisibilityFrustum(volume,visibility,badView,views,frustum));
    auto badVolume=volume;const float nan=NAN;memcpy(badVolume.data()+0x60,&nan,4);
    CHECK(!MakeEyeVisibilityFrustum(badVolume,visibility,render,views,frustum));
    CHECK(memcmp(&saved,&frustum,sizeof frustum)==0);
    const auto built=BuildVolume(frustum);
    badVolume=built;memcpy(badVolume.data()+16,badVolume.data(),16);CHECK(!ValidateEyeVisibilityFrustum(badVolume,frustum));
    badVolume=built;memcpy(badVolume.data()+0x70,badVolume.data()+0x60,16);CHECK(!ValidateEyeVisibilityFrustum(badVolume,frustum));
    badVolume=built;float d{};memcpy(&d,badVolume.data()+12,4);d+=1;memcpy(badVolume.data()+12,&d,4);CHECK(!ValidateEyeVisibilityFrustum(badVolume,frustum));
    badVolume=built;memcpy(badVolume.data(),&nan,4);CHECK(!ValidateEyeVisibilityFrustum(badVolume,frustum));
    auto badFrustum=frustum;badFrustum.eyePoints[0]={1e9,1e9,1e9};CHECK(!ValidateEyeVisibilityFrustum(built,badFrustum));
    // A .025-unit inward native far-plane perturbation is inside the .03
    // spatial readback budget, and the larger construction margin covers it.
    auto inward=built;float farOffset{};memcpy(&farOffset,inward.data()+23*4,4);
    farOffset-=.025f;memcpy(inward.data()+23*4,&farOffset,4);
    CHECK(ValidateEyeVisibilityFrustum(inward,frustum));
    std::array<std::array<double,4>,6> inwardPlanes{};
    CHECK(eye_visibility_detail::GeometryMatches(inward,frustum.matrix,inwardPlanes));
    auto insufficient=frustum;
    const double shift=Side(inwardPlanes[5],insufficient.eyePoints[0])-.249;
    for (size_t axis=0;axis<3;++axis) insufficient.eyePoints[0][axis]-=shift*inwardPlanes[5][axis];
    CHECK(!ValidateEyeVisibilityFrustum(inward,insufficient)); // Geometry alone is never sufficient.
    farOffset-=.01f;memcpy(inward.data()+23*4,&farOffset,4);
    CHECK(!eye_visibility_detail::GeometryMatches(inward,frustum.matrix,inwardPlanes));
    // A real rotation remains identifiable as the far face but moves that
    // plane by >.03 at its wide corners, so geometry must independently fail.
    auto rotated=built;std::array<float,4> rotatedPlane{};
    memcpy(rotatedPlane.data(),rotated.data()+20*4,16);
    Point newNormal{},oldNormal{},pivot{};
    double length{};
    for (size_t axis=0;axis<3;++axis) {
        oldNormal[axis]=rotatedPlane[axis];
        newNormal[axis]=oldNormal[axis]+.00005*frustum.axes[0][axis];
        length+=newNormal[axis]*newNormal[axis];
        pivot[axis]=frustum.origin[axis]-frustum.farDistance*frustum.axes[2][axis];
    }
    double rotatedOffset=rotatedPlane[3];
    for (size_t axis=0;axis<3;++axis) {
        newNormal[axis]/=std::sqrt(length);
        CHECK(std::abs(newNormal[axis]-oldNormal[axis])<1e-4);
        rotatedOffset-=(newNormal[axis]-oldNormal[axis])*pivot[axis];
        rotatedPlane[axis]=float(newNormal[axis]);
    }
    rotatedPlane[3]=float(rotatedOffset);memcpy(rotated.data()+20*4,rotatedPlane.data(),16);
    CHECK(!eye_visibility_detail::GeometryMatches(rotated,frustum.matrix,inwardPlanes));
    // Exact native refusal, frame 1802 of diagnostic-20261020-191043-346.
    // Preserve this rejection: the correction changes input conditioning,
    // not the existing .25-eye / .03-spatial acceptance limits.
    constexpr uint32_t refusedMatrix[]{
        0x3eaa10beu,0x3e430fe1u,0x3f63c335u,0x3f639e90u,0x31e47083u,0x3f549c13u,0xbcced7b1u,0xbcceb669u,
        0x3f2575d2u,0xbdc87db9u,0xbeea1a0au,0xbee9f460u,0x4387a11bu,0xc215ec71u,0xc234e3abu,0xc232efacu};
    constexpr uint32_t refusedVolume[]{
        0x3f639f86u,0xbccec5c3u,0xbee9f099u,0xc233ce94u,0xbf639ea5u,0x3cceaceau,0x3ee9f417u,0x44421f92u,
        0x3f7cec60u,0xbca737d8u,0x3e1ce4d8u,0x433740d4u,0x3ee6ae84u,0xbca7297cu,0xbf647c0bu,0xc37fa254u,
        0x3f3dbbf4u,0x3f0d866au,0xbec30388u,0xc261c1e2u,0x3f19f84du,0xbf3c985fu,0xbe9e411cu,0xc0c7c440u,
        0x44792ab2u,0x44344d2du,0x434837bdu,0x00000000u,0x42984bbfu,0x44344d2du,0xc4c6df4fu,0x00000000u,
        0x446f575cu,0xc480b137u,0x435c679du,0x00000000u,0x42136214u,0xc480b137u,0xc4c45953u,0x00000000u,
        0xc301b82du,0x42065b76u,0xc3b055d1u,0x00000000u,0xc302023fu,0x42065b76u,0xc3b09de2u,0x00000000u,
        0xc301bb57u,0x420427f5u,0xc3b05501u,0x00000000u,0xc3020569u,0x420427f5u,0xc3b09d13u,0x00000000u};
    EyeVisibilityFrustum refused{};VisibilityVolume refusedNative{};
    memcpy(refused.matrix.data(),refusedMatrix,sizeof refusedMatrix);
    memcpy(refusedNative.data(),refusedVolume,sizeof refusedVolume);
    std::array<std::array<double,4>,6> expected{};
    CHECK(eye_visibility_detail::ExtractPlanes(refused.matrix,expected));
    std::array<float,56> actual{};memcpy(actual.data(),refusedNative.data(),refusedNative.size());
    CHECK(std::abs(actual[7]-expected[5][3])>.053 && std::abs(actual[7]-expected[5][3])<.055);
    CHECK(std::abs(actual[2]-expected[4][2])>2.8e-5);
    CHECK(Side(expected[5],{actual[32],actual[33],actual[34]})<-.061);
    CHECK(!ValidateEyeVisibilityFrustum(refusedNative,refused));
    // Conditioned native output, frame 1682 of diagnostic-20261020-191824-487.
    // Its raw far-plane D differs by .021197 but every corner is on the same
    // intended/actual face within .03. This proves structural readback only:
    // the original exact eye points were not retained in this old receipt.
    constexpr uint32_t conditionedMatrix[]{
        0x3ec4d2fau,0x3e3669e1u,0x3f5a67bcu,0x3f596032u,0x3762e0e6u,0x3f5556dbu,0xbca3a551u,0xbca2dfdau,
        0x3f1e51c1u,0xbde2d0ccu,0xbf07c2f8u,0xbf071f27u,0x43876488u,0xc1ff25aeu,0xc1e09fc5u,0xc1c3f359u};
    constexpr uint32_t conditionedVolume[]{
        0x3f59607bu,0xbca2e456u,0xbf071eb1u,0xc1d1c400u,0xbf596036u,0x3ca2dbe9u,0x3f071f22u,0x443d33aeu,
        0x3f7f476bu,0xbc8392abu,0x3d9608f4u,0x434719e9u,0x3ec056dfu,0xbc83bb89u,0xbf6d36c5u,0xc36eb30au,
        0x3f346941u,0x3f0eddc1u,0xbee04b82u,0xc21abbb4u,0x3f13b7e5u,0xbf3bd79eu,0xbeb7a2eau,0x40cba280u,
        0x44776bf6u,0x4434b88du,0x43031585u,0x00000000u,0xc2904f06u,0x4434b623u,0xc4c52495u,0x00000000u,
        0x44700ae7u,0xc4803b6du,0x4315772eu,0x00000000u,0xc2cb577eu,0xc4803ca2u,0xc4c2d85fu,0x00000000u,
        0xc32c3876u,0x420647c6u,0xc3a400c8u,0x00000000u,0xc32ebaa2u,0x420647afu,0xc3a60554u,0x00000000u,
        0xc32c4a51u,0x41eb82d7u,0xc3a3fb39u,0x00000000u,0xc32ecc7bu,0x41eb82a6u,0xc3a5ffc5u,0x00000000u};
    std::array<float,16> conditioned{};
    memcpy(conditioned.data(),conditionedMatrix,sizeof conditionedMatrix);
    memcpy(refusedNative.data(),conditionedVolume,sizeof conditionedVolume);
    CHECK(eye_visibility_detail::GeometryMatches(refusedNative,conditioned,expected));
    // Exact fifth-pair output and all ten float64 eye points from
    // diagnostic-20261020-192652-305, frame 1686. Normals differ by
    // 1.04517e-5, but corners meet .03 and minimum actual eye margin is .29093.
    constexpr uint32_t movingMatrix[]{
        0x3ee9d007u,0x3ded16f8u,0x3f480ae7u,0x3f471db6u,0x3be407f9u,0x3f585b7du,0x3d39cc1bu,0x3d38efcfu,
        0x3f112d24u,0xbdd4294au,0xbf213b23u,0xbf207bf8u,0x4384b5acu,0xc226b241u,0xc2a15338u,0xc299cb2bu};
    constexpr uint32_t movingVolume[]{
        0x3f471d21u,0x3d38f870u,0xbf207ca7u,0xc29d343cu,0xbf471d9eu,0xbd38efcau,0x3f207c15u,0x444a5195u,
        0x3f7f7928u,0x3d2c8bdau,0xbd45f9ceu,0x4318676bu,0x3e84ea53u,0x3cfceb02u,0xbf7718d4u,0xc38a5d75u,
        0x3f1cef0eu,0x3f1c5c3du,0xbf004b27u,0xc2a2b0d6u,0x3f11c017u,0xbf301ffdu,0xbee66a65u,0xc1f25520u,
        0x447a5285u,0x4442e962u,0x40f13442u,0x00000000u,0xc3822aa5u,0x443e1af5u,0xc4c2e523u,0x00000000u,
        0x44862920u,0xc471f50du,0xc1e7f37du,0x00000000u,0xc33c565du,0xc476c37au,0xc4c77626u,0x00000000u,
        0xc32766deu,0x4206e409u,0xc3a5529fu,0x00000000u,0xc32a5490u,0x4206b656u,0xc3a7241eu,0x00000000u,
        0xc3273c15u,0x41ed54ccu,0xc3a55d7bu,0x00000000u,0xc32a29c9u,0x41ecf965u,0xc3a72ef7u,0x00000000u};
    constexpr uint64_t movingEyes[]{
        0xc06512bb721524e1ull,0x404009e14d064995ull,0xc074cb604c7f6a4aull,
        0xc067814732b278a4ull,0xc08ed0f576cc0c0cull,0xc098eb2ab0cce914ull,
        0x4089d326e2e83339ull,0xc08e554a4379241eull,0xc074f0cb04a5ebdfull,
        0xc0703f7cc547d3e2ull,0x4087bdc256c3793eull,0xc098592cc9a11048ull,
        0x408793ba4cf0e770ull,0x4088396d8a16612dull,0xc072a8d367f688abull,
        0xc0651172a13adc10ull,0x404009f557f7670eull,0xc074ca942256f6d8ull,
        0x404d01b28d95bf44ull,0xc08eb2f6746463dcull,0xc09424f429795cb0ull,
        0x4090c1c9ec3716abull,0xc08e374b41117bedull,0xc03d7f0e757ba500ull,
        0xc02bd45b477bf500ull,0x4087dbc1592b2170ull,0xc09392f6424d83e4ull,
        0x408f44274276e18eull,0x4088576c8c7e095eull,0x401c01ad55ea3940ull};
    EyeVisibilityFrustum moving{};VisibilityVolume movingNative{};
    memcpy(moving.matrix.data(),movingMatrix,sizeof movingMatrix);
    memcpy(moving.eyePoints.data(),movingEyes,sizeof movingEyes);
    memcpy(movingNative.data(),movingVolume,sizeof movingVolume);
    CHECK(ValidateEyeVisibilityFrustum(movingNative,moving));
    CHECK(eye_visibility_detail::GeometryMatches(movingNative,moving.matrix,expected));
    double actualMargin=std::numeric_limits<double>::infinity();
    for (const auto& plane:expected) for (const auto& point:moving.eyePoints) actualMargin=(std::min)(actualMargin,Side(plane,point));
    CHECK(actualMargin>.29093 && actualMargin<.29094);
    // Exact next refusal, diagnostic-20261020-193119-195. Its eyes still fit,
    // but a corner exceeds .03 by .0001404; preserve geometric rejection.
    constexpr uint32_t boundaryMatrix[]{
        0x3ee8f1feu,0x3dea8acdu,0x3f487f5cu,0x3f479197u,0x3be2798bu,0x3f5873cau,0x3d43aadeu,0x3d42c2d3u,
        0x3f118654u,0xbdd0c5c2u,0xbf209e98u,0xbf1fe01eu,0x43817553u,0xc243d115u,0xc2f5e5fcu,0xc2edf99bu};
    constexpr uint32_t boundaryVolume[]{
        0x3f479175u,0x3d42cc51u,0xbf1fe03du,0xc2f160a2u,0xbf479185u,0xbd42c572u,0x3f1fe031u,0x4454ceabu,
        0x3f7f7d12u,0x3d34559cu,0xbd399af9u,0x42e23d7du,0x3e865aacu,0x3d0692ebu,0xbf76e2b6u,0xc398bf98u,
        0x3f1d06c2u,0x3f1cd886u,0xbeff2bddu,0xc2e66eb6u,0x3f126a29u,0xbf2fad52u,0xbee61932u,0xc270ea60u,
        0x4481c20eu,0x44439a1au,0xc0f54614u,0x00000000u,0xc35aa97au,0x443ed433u,0xc4c53b5au,0x00000000u,
        0x448b2b88u,0xc471248fu,0xc23ac55du,0x00000000u,0xc30f5dadu,0xc475ea75u,0xc4ca1c3fu,0x00000000u,
        0xc3000c2au,0x420c0404u,0xc3af3645u,0x00000000u,0xc302f713u,0x420bd6a1u,0xc3b108e1u,0x00000000u,
        0xc2ffbed2u,0x41f795b3u,0xc3af41dfu,0x00000000u,0xc302ca54u,0x41f73aecu,0xc3b1147bu,0x00000000u};
    constexpr uint64_t boundaryEyes[]{
        0xc060271a08528663ull,0x4040ae03a492f5f5ull,0xc07607edc0dd7e53ull,
        0xc061e2452f3f3872ull,0xc08eb5d8dd6c4afbull,0xc0993ff4ceee60c2ull,
        0x408b1b6804e14c71ull,0xc08e3b0b29eb4f17ull,0xc07617101674216full,
        0xc06b4971eb7b49aeull,0x4087d4ec98a77d8eull,0xc098a402e3d2017cull,
        0x4088c19cd5d24822ull,0x40884fba4c287970ull,0xc073a7486a02a45dull,
        0xc06025d26fc569e3ull,0x4040ae178c83ab2bull,0xc0760721194f9068ull,
        0x405981bf1bb96016ull,0xc08e980f94997d96ull,0xc09477057466fde2ull,
        0x409162189a142349ull,0xc08e1d41e11881b1ull,0xc0479a9562b4afa8ull,
        0x403acd968d04f670ull,0x4087f2b5e17a4af3ull,0xc093db13894a9e9eull,
        0x40903533028ca121ull,0x40886d8394fb46d6ull,0xc020715ffca31c00ull};
    EyeVisibilityFrustum boundary{};VisibilityVolume boundaryNative{};
    memcpy(boundary.matrix.data(),boundaryMatrix,sizeof boundaryMatrix);
    memcpy(boundary.eyePoints.data(),boundaryEyes,sizeof boundaryEyes);
    memcpy(boundaryNative.data(),boundaryVolume,sizeof boundaryVolume);
    CHECK(!eye_visibility_detail::GeometryMatches(boundaryNative,boundary.matrix,expected));
    CHECK(!ValidateEyeVisibilityFrustum(boundaryNative,boundary));
    std::array<float,56> boundaryWords{};memcpy(boundaryWords.data(),boundaryNative.data(),boundaryNative.size());
    actualMargin=std::numeric_limits<double>::infinity();
    for (size_t plane=0;plane<6;++plane) for (const auto& point:boundary.eyePoints)
        actualMargin=(std::min)(actualMargin,Side({boundaryWords[plane*4],boundaryWords[plane*4+1],
            boundaryWords[plane*4+2],boundaryWords[plane*4+3]},point));
    CHECK(actualMargin>.27935 && actualMargin<.27936);
    // Exact Chicago refusal retains its geometric rejection even though all
    // captured eyes have .2689 margin. Improve construction, never this guard.
    const auto fixtures=std::filesystem::path(argv[1]).parent_path();
    EyeVisibilityFrustum chicago{};
    const auto chicagoMatrix=Load<std::array<uint8_t,64>>(fixtures/"chicago-visibility-refused-matrix.bin");
    const auto chicagoEyes=Load<std::array<uint8_t,240>>(fixtures/"chicago-visibility-refused-eyes.bin");
    const auto chicagoVolume=Load<VisibilityVolume>(fixtures/"chicago-visibility-refused-volume.bin");
    memcpy(chicago.matrix.data(),chicagoMatrix.data(),chicagoMatrix.size());
    memcpy(chicago.eyePoints.data(),chicagoEyes.data(),chicagoEyes.size());
    CHECK(!ValidateEyeVisibilityFrustum(chicagoVolume,chicago));
    // Exact first eligible SkipIntro frame, diagnostic-20261024-143019-258.
    // The startup camera is near (10000,-10000,0): native reconstruction loses
    // .03869 units on a side face although every eye retains >.2679 margin.
    const auto startupVolume=Load<VisibilityVolume>(fixtures/"miami-startup-1406-volume.bin");
    const auto startupSource=Load<SceneView>(fixtures/"miami-startup-1406-source.bin");
    const auto startupRender=Load<SceneView>(fixtures/"miami-startup-1406-render.bin");
    const auto startupEyeBytes=Load<std::array<uint8_t,88>>(fixtures/"miami-startup-1406-eyes.bin");
    const auto startupMatrix=Load<std::array<uint8_t,64>>(fixtures/"miami-startup-1406-refused-matrix.bin");
    const auto startupPoints=Load<std::array<uint8_t,240>>(fixtures/"miami-startup-1406-refused-points.bin");
    const auto startupRefused=Load<VisibilityVolume>(fixtures/"miami-startup-1406-refused-volume.bin");
    SceneViews startupEyes{};memcpy(&startupEyes,startupEyeBytes.data(),sizeof startupEyes);
    EyeVisibilityFrustum startup{};
    CHECK(startupSource==startupRender);
    CHECK(MakeEyeVisibilityFrustum(startupVolume,startupSource,startupRender,startupEyes,startup));
    CHECK(memcmp(startup.matrix.data(),startupMatrix.data(),startupMatrix.size())==0);
    CHECK(memcmp(startup.eyePoints.data(),startupPoints.data(),startupPoints.size())==0);
    CHECK(!ValidateEyeVisibilityFrustum(startupRefused,startup));
    std::array<float,56> startupWords{};memcpy(startupWords.data(),startupRefused.data(),startupRefused.size());
    actualMargin=std::numeric_limits<double>::infinity();
    for (size_t plane=0;plane<6;++plane) for (const auto& point:startup.eyePoints)
        actualMargin=(std::min)(actualMargin,Side({startupWords[plane*4],startupWords[plane*4+1],
            startupWords[plane*4+2],startupWords[plane*4+3]},point));
    CHECK(actualMargin>.2679 && actualMargin<.2680);
    CHECK(Side(startup.planes[1],{startupWords[32],startupWords[33],startupWords[34]})<-.038);
    const auto startupSaved=startup;
    EyeVisibilityFrustum local{};
    CHECK(MakeLocalEyeVisibilityFrustum(startup,local));
    CHECK((local.origin==Point{0,0,0}));
    const auto localNative=BuildVolume(local);
    CHECK(ValidateEyeVisibilityFrustum(localNative,local));
    VisibilityVolume restored{};
    CHECK(RestoreLocalEyeVisibilityVolume(localNative,startup,restored));
    CHECK(ValidateEyeVisibilityFrustum(restored,startup));
    CHECK(memcmp(&startup,&startupSaved,sizeof startup)==0);
    const auto restoredSaved=restored;
    for (unsigned mutation=0;mutation<4;++mutation) {
        auto corrupt=localNative;
        if (mutation==0) memcpy(corrupt.data()+16,corrupt.data(),16);
        if (mutation==1) memcpy(corrupt.data()+0x70,corrupt.data()+0x60,16);
        if (mutation==2) { float distance{};memcpy(&distance,corrupt.data()+12,4);distance+=1;memcpy(corrupt.data()+12,&distance,4); }
        if (mutation==3) memcpy(corrupt.data(),&nan,4);
        CHECK(!RestoreLocalEyeVisibilityVolume(corrupt,startup,restored));
        CHECK(restored==restoredSaved);
    }
    auto wrongWorld=startup;wrongWorld.matrix[12]+=1;
    CHECK(!RestoreLocalEyeVisibilityVolume(localNative,wrongWorld,restored));
    CHECK(restored==restoredSaved); // World geometry is still independently checked.
    wrongWorld=startup;wrongWorld.origin[0]=NAN;
    const auto localSaved=local;
    CHECK(!MakeLocalEyeVisibilityFrustum(wrongWorld,local));
    CHECK(memcmp(&local,&localSaved,sizeof local)==0);
    CHECK(!RestoreLocalEyeVisibilityVolume(localNative,wrongWorld,restored));
    CHECK(restored==restoredSaved);
    // Longer native depth ranges must keep all sampled eye rays and the same
    // .03/.25 acceptance checks. Native builder output still requires a run.
    // Beyond eight times this fixture, its native corner radius exceeds the
    // existing 25,000-unit input limit; that is not an accepted source volume.
    for(double scale:{.45,1.,2.,4.,8.}) {
        auto longerVolume=volume;auto longerVisibility=visibility,longerRender=render;
        std::array<float,56> words{};memcpy(words.data(),volume.data(),volume.size());
        for(size_t plane=0;plane<6;++plane)words[plane*4+3]*=float(scale);
        for(size_t corner=0;corner<8;++corner)for(size_t axis=0;axis<3;++axis)words[24+corner*4+axis]*=float(scale);
        memcpy(longerVolume.data(),words.data(),longerVolume.size());
        for(auto* view:{&longerVisibility,&longerRender})for(size_t axis=0;axis<3;++axis) {
            float position{};memcpy(&position,view->data()+0x40+axis*4,4);position*=float(scale);
            memcpy(view->data()+0x40+axis*4,&position,4);
        }
        for(double yaw:{0.,110.,-110.}) {
            const auto posed=Views(yaw,20,8);EyeVisibilityFrustum longer;TrackedVisibilityCoverage depth;
            CHECK(MakeEyeVisibilityFrustum(longerVolume,longerVisibility,longerRender,posed,longer));
            CHECK(ValidateEyeVisibilityFrustum(BuildVolume(longer),longer));
            CHECK(MeasureTrackedVisibility(longerVolume,longerVisibility,longerRender,posed,depth));
            if(scale==.45) {
                CHECK(depth.farDistance<750.);
                double retreat{};
                for(size_t axis=0;axis<3;++axis)retreat+=(longer.origin[axis]-(longer.eyePoints[0][axis]+longer.eyePoints[5][axis])*.5)*longer.axes[2][axis];
                CHECK(std::abs(retreat-8.)<1e-7);
            }
            CHECK(longer.nearDistance>8.*scale && longer.farDistance*longer.farDistance/longer.nearDistance<110000.);
            for(size_t eye=0;eye<2;++eye)for(double fraction:{0.,.01,.5,1.}) {
                const auto& f=posed.frusta[eye];
                for(double x:{double(f.left),0.,double(f.right)})for(double y:{double(f.down),0.,double(f.up)})
                    for(const auto& plane:longer.planes)CHECK(Side(plane,EyePoint(longerRender,posed.poses[eye],x,y,fraction*depth.farDistance))>=eyeVisibilityMargin-1e-8);
            }
        }
    }
    // Same native fixture, poses and VDXR tangents measured before the retreat
    // change: 2-unit volume=7440594511.89. Bound the conditioning size cost.
    const auto& slopes=frustum.slopes;
    const double conditionedSize=(slopes[1]-slopes[0])*(slopes[3]-slopes[2])*
        (std::pow(frustum.farDistance,3)-std::pow(frustum.nearDistance,3))/3;
    const double conditioningRatio=conditionedSize/7440594511.89;
    CHECK(frustum.nearDistance>7.6 && conditioningRatio>1 && conditioningRatio<1.03);
    std::printf("Depth-scaled apex conditioning: fixture volume ratio versus two-unit apex %.9f, near %.9f.\n",conditioningRatio,frustum.nearDistance);
    std::printf("Eye-only perspective visibility: %u sampled points, maximum OBB ratio %.6f, cube ratio %.6f; native fixtures, divergence rejection, readback and atomicity passed.\n",samples,largestBoxRatio,largestCubeRatio);
}
