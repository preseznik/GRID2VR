#include "tracked_visibility.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
using namespace grid2vr;
namespace {
using Point=std::array<double,3>;
void Check(bool result,int line) {
    if (!result) { std::fprintf(stderr,"Tracked visibility check failed at line %d\n",line);std::exit(1); }
}
#define CHECK(x) Check(bool(x),__LINE__)
template<class T> T Load(const std::filesystem::path& path) {
    T value{};std::ifstream file(path,std::ios::binary);
    CHECK(file.read(reinterpret_cast<char*>(value.data()),value.size()) && file.peek()==EOF);
    return value;
}
Point Cross(Point a,Point b) {
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
}
double Dot(Point a,Point b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
// Independent quaternion-vector formula, retaining native basis distortion.
Point EyePoint(const SceneView& view,const ScenePose& pose,double x,double y,double distance) {
    Point local{x*distance,y*distance,-distance},q{};
    double norm{};for (auto value:pose.orientation) norm+=double(value)*value;
    for (size_t i=0;i<3;++i) q[i]=pose.orientation[i]/std::sqrt(norm);
    const auto t=Cross(q,local),u=Cross(q,t);
    for (size_t i=0;i<3;++i) local[i]+=2*(pose.orientation[3]/std::sqrt(norm)*t[i]+u[i])+pose.position[i];
    Point world{};constexpr size_t offsets[]{0x20,0x10,0x30};constexpr double signs[]{-1,1,-1};
    for (size_t axis=0;axis<3;++axis) {
        float origin{};memcpy(&origin,view.data()+0x40+4*axis,4);world[axis]=origin;
        for (size_t row=0;row<3;++row) {
            float basis{};memcpy(&basis,view.data()+offsets[row]+4*axis,4);
            world[axis]+=local[row]*signs[row]*basis;
        }
    }
    return world;
}
void Contained(const TrackedVisibilityBox& box,const Point& point) {
    Point delta{};for (size_t i=0;i<3;++i) delta[i]=point[i]-box.center[i];
    for (size_t axis=0;axis<3;++axis) {
        CHECK(std::abs(Dot(delta,box.axes[axis]))<=box.halfExtent[axis]-trackedVisibilityMargin+.00001);
        double clip=box.matrix[12+axis];
        for (size_t row=0;row<3;++row) clip+=point[row]*box.matrix[4*row+axis];
        CHECK(std::abs(clip)<1);
    }
}
// Reconstruct from the FLOAT matrix, including its actual inverse. This tests
// the builder contract without substituting ideal box coordinates.
VisibilityVolume BuildVolume(const TrackedVisibilityBox& box) {
    Point columns[3]{};
    for (size_t axis=0;axis<3;++axis) for (size_t row=0;row<3;++row) columns[axis][row]=box.matrix[4*row+axis];
    const double determinant=Dot(columns[0],Cross(columns[1],columns[2]));
    CHECK(std::abs(determinant)>1e-15);
    const Point inverseRows[]{Cross(columns[1],columns[2]),Cross(columns[2],columns[0]),Cross(columns[0],columns[1])};
    std::array<float,56> data{};
    for (size_t corner=0;corner<8;++corner) for (size_t row=0;row<3;++row) {
        double value{};
        for (size_t axis=0;axis<3;++axis)
            value+=(((corner&(1u<<axis))?1.:-1.)-box.matrix[12+axis])*inverseRows[axis][row]/determinant;
        data[24+4*corner+row]=static_cast<float>(value);
    }
    for (size_t axis=0;axis<3;++axis) for (size_t side=0;side<2;++side) {
        const double sign=side?1.:-1.,length=std::sqrt(Dot(columns[axis],columns[axis]));
        for (size_t row=0;row<3;++row) data[4*(2*axis+side)+row]=static_cast<float>(sign*columns[axis][row]/length);
        data[4*(2*axis+side)+3]=static_cast<float>((1+sign*box.matrix[12+axis])/length);
    }
    VisibilityVolume result{};memcpy(result.data(),data.data(),result.size());return result;
}
SceneViews Views(float yaw,float pitch,Point position) {
    constexpr double radians=3.14159265358979323846/180.;
    const double y=yaw*radians*.5,p=pitch*radians*.5;
    ScenePose center{{float(std::cos(y)*std::sin(p)),float(std::sin(y)*std::cos(p)),
                      float(-std::sin(y)*std::sin(p)),float(std::cos(y)*std::cos(p))},
                     {float(position[0]),float(position[1]),float(position[2])}};
    SceneViews result{};
    CHECK(SeparateSceneEyes(center,.0647300034f,result.poses));
    result.frusta={EyeFrustum{-1.37638187f,.839099586f,-1.42814791f,.965688765f},
                   EyeFrustum{-.839099586f,1.37638187f,-1.42814791f,.965688765f}};
    return result;
}
}
int main(int argc,char** argv) {
    CHECK(argc==4);
    const auto volume=Load<VisibilityVolume>(argv[1]);
    const auto visibility=Load<SceneView>(argv[2]),render=Load<SceneView>(argv[3]);
    const auto originalVolume=volume;
    const auto originalVisibility=visibility,originalRender=render;
    double largestRatio{};unsigned checks{};
    // Both current poses are used, not a center orientation or symmetric FOV.
    for (float yaw:{0.f,45.f,90.f,-90.f,180.f,-180.f}) for (float pitch:{0.f,40.f,-40.f}) {
        auto views=Views(yaw,pitch,{.4,1.1,-.3});
        if (yaw==45) views.poses[1].orientation=Views(yaw+5,pitch-3,{}).poses[1].orientation;
        const auto originalViews=views;
        TrackedVisibilityBox box;TrackedVisibilityCoverage coverage,measured;
        CHECK(MakeTrackedVisibilityBox(volume,visibility,render,views,box,&coverage));
        CHECK(MeasureTrackedVisibility(volume,visibility,render,views,measured));
        CHECK(coverage.eyePoints==10 && coverage.farDistance==measured.farDistance &&
              coverage.outsideNative==measured.outsideNative && coverage.farCornersOutsideNative>0);
        CHECK(coverage.minimumNativeSide<0 && coverage.minimumFarNativeSide<0);
        VisibilityBox old;
        CHECK(MakeEyeVisibilityBox(volume,visibility,render,views.frusta,old));
        const double ratio=box.halfExtent[0]*box.halfExtent[1]*box.halfExtent[2]/std::pow(old.radius,3);
        largestRatio=(std::max)(largestRatio,ratio);
        CHECK(ratio<.65);
        std::array<float,56> native{};memcpy(native.data(),volume.data(),volume.size());
        for (size_t point=0;point<8;++point)
            Contained(box,{native[24+4*point],native[25+4*point],native[26+4*point]});
        double measuredMinimum=std::numeric_limits<double>::infinity();
        unsigned measuredOutside{};
        for (size_t eye=0;eye<2;++eye) {
            const auto& f=views.frusta[eye];
            for (double z:{0.,.01,.25,.5,1.}) for (double x:{double(f.left),0.,double(f.right)})
                for (double y:{double(f.down),0.,double(f.up)}) {
                    Contained(box,EyePoint(render,views.poses[eye],x,y,z*coverage.farDistance));++checks;
                }
            for (double x:{double(f.left),double(f.right)}) for (double y:{double(f.down),double(f.up)}) {
                const auto point=EyePoint(render,views.poses[eye],x,y,coverage.farDistance);
                double minimum=std::numeric_limits<double>::infinity();
                for (size_t plane=0;plane<6;++plane)
                    minimum=(std::min)(minimum,Dot(point,{native[4*plane],native[4*plane+1],native[4*plane+2]})+native[4*plane+3]);
                if (minimum<-.02) ++measuredOutside;
                measuredMinimum=(std::min)(measuredMinimum,minimum);
            }
        }
        CHECK(measuredOutside==coverage.farCornersOutsideNative && std::abs(measuredMinimum-coverage.minimumFarNativeSide)<1e-8);
        const auto built=BuildVolume(box);
        CHECK(ValidateTrackedVisibilityBox(built,box));
        auto bad=built;memcpy(bad.data()+0x70,bad.data()+0x60,16);
        CHECK(!ValidateTrackedVisibilityBox(bad,box));
        bad=built;memcpy(bad.data()+16,bad.data(),16);
        CHECK(!ValidateTrackedVisibilityBox(bad,box));
        bad=built;float distance{};memcpy(&distance,bad.data()+12,4);distance+=1;memcpy(bad.data()+12,&distance,4);
        CHECK(!ValidateTrackedVisibilityBox(bad,box));
        bad=built;const float invalidNormal=NAN;memcpy(bad.data(),&invalidNormal,4);
        CHECK(!ValidateTrackedVisibilityBox(bad,box));
        CHECK(memcmp(&views,&originalViews,sizeof views)==0);
    }
    // Retained native affine camera fixtures and a deliberately skewed basis
    // near the accepted .001 threshold must preserve containment.
    const auto directory=std::filesystem::path(argv[1]).parent_path();
    for (const auto name:{"native-transition-view.bin","native-practice-view.bin",
                         "native-long-14834-center-view.bin","native-headset-6136-center-view.bin"}) {
        auto view=Load<SceneView>(directory/name);
        const auto views=Views(90,30,{.4,.7,-.2});
        TrackedVisibilityBox box;TrackedVisibilityCoverage coverage;
        CHECK(MakeTrackedVisibilityBox(volume,visibility,view,views,box,&coverage));
        CHECK(ValidateTrackedVisibilityBox(BuildVolume(box),box));
        for (size_t eye=0;eye<2;++eye) {
            const auto& f=views.frusta[eye];
            for (double z:{0.,.5,1.}) for (double x:{double(f.left),0.,double(f.right)})
                for (double y:{double(f.down),0.,double(f.up)})
                    Contained(box,EyePoint(view,views.poses[eye],x,y,z*coverage.farDistance));
        }
    }
    auto affine=render;
    float right[3]{},up[3]{};memcpy(right,render.data()+0x20,12);memcpy(up,render.data()+0x10,12);
    for (size_t i=0;i<3;++i) right[i]=right[i]*1.0001f+up[i]*.0002f;
    memcpy(affine.data()+0x20,right,12);
    const auto views=Views(-90,-35,{.2,1,-.3});
    TrackedVisibilityBox box;TrackedVisibilityCoverage coverage;
    CHECK(MakeTrackedVisibilityBox(volume,visibility,affine,views,box,&coverage));
    for (size_t eye=0;eye<2;++eye) for (double x:{-1.37638187,1.37638187}) {
        // Use actual per-eye boundary rather than the larger opposite-eye FOV.
        const auto& f=views.frusta[eye];
        Contained(box,EyePoint(affine,views.poses[eye],x<0?f.left:f.right,f.down,coverage.farDistance));
    }
    CHECK(ValidateTrackedVisibilityBox(BuildVolume(box),box));
    // Exercise the narrow contraction/expansion envelope independently of
    // the captured affine camera. Keep the all-rotation cube's old upper bound.
    for (float scale:{.99905f,1.00045f}) {
        auto scaled=render;
        for (size_t offset:{0x10u,0x20u,0x30u}) for (size_t axis=0;axis<3;++axis) {
            float value{};memcpy(&value,scaled.data()+offset+4*axis,4);value*=scale;
            memcpy(scaled.data()+offset+4*axis,&value,4);
        }
        CHECK(MakeTrackedVisibilityBox(volume,visibility,scaled,views,box,&coverage));
        CHECK(ValidateTrackedVisibilityBox(BuildVolume(box),box));
        VisibilityBox cube;
        CHECK(MakeEyeVisibilityBox(volume,visibility,scaled,views.frusta,cube));
        for (size_t eye=0;eye<2;++eye) {
            const auto& f=views.frusta[eye];
            for (double x:{double(f.left),double(f.right)}) for (double y:{double(f.down),double(f.up)}) {
                const auto point=EyePoint(scaled,views.poses[eye],x,y,coverage.farDistance);
                Contained(box,point);
                for (size_t axis=0;axis<3;++axis) CHECK(std::abs(point[axis]-cube.center[axis])<cube.radius);
            }
        }
    }
    const auto saved=box;const auto savedCoverage=coverage;
    const auto invalid=[&](const VisibilityVolume& v,const SceneView& r,const SceneViews& pair) {
        CHECK(!MakeTrackedVisibilityBox(v,visibility,r,pair,box,&coverage));
        CHECK(memcmp(&box,&saved,sizeof box)==0 && memcmp(&coverage,&savedCoverage,sizeof coverage)==0);
        CHECK(!MeasureTrackedVisibility(v,visibility,r,pair,coverage));
        CHECK(memcmp(&coverage,&savedCoverage,sizeof coverage)==0);
    };
    for (float scale:{.99895f,1.00055f}) {
        auto scaled=render;
        for (size_t offset:{0x10u,0x20u,0x30u}) for (size_t axis=0;axis<3;++axis) {
            float value{};memcpy(&value,scaled.data()+offset+4*axis,4);value*=scale;
            memcpy(scaled.data()+offset+4*axis,&value,4);
        }
        invalid(volume,scaled,views);
        VisibilityBox cube;CHECK(!MakeEyeVisibilityBox(volume,visibility,scaled,views.frusta,cube));
    }
    for (size_t item=0;item<6;++item) {
        auto badViews=views;
        if (item==0) badViews.poses[0].orientation[0]=NAN;
        if (item==1) badViews.poses[1].orientation={0,0,0,0};
        if (item==2) badViews.poses[1].position[0]=3;
        if (item==3) badViews.frusta[0].left=0;
        if (item==4) badViews.frusta[1].up=INFINITY;
        if (item==5) badViews.frusta[1].right=4.01f;
        invalid(volume,render,badViews);
    }
    for (size_t item=0;item<5;++item) {
        auto bad=render;float value{};
        if (item==0) { value=NAN;memcpy(bad.data()+0x40,&value,4); }
        if (item==1) { value=100001;memcpy(bad.data()+0x40,&value,4); }
        if (item==2) memset(bad.data()+0x10,0,12);
        if (item==3) for (size_t i=0;i<3;++i) { memcpy(&value,bad.data()+0x20+4*i,4);value=-value;memcpy(bad.data()+0x20+4*i,&value,4); }
        if (item==4) { value=2;memcpy(bad.data()+0x30,&value,4); }
        invalid(volume,bad,views);
    }
    auto badVolume=volume;const float nan=NAN;memcpy(badVolume.data()+0x60,&nan,4);invalid(badVolume,render,views);
    badVolume=volume;memset(badVolume.data(),0,12);invalid(badVolume,render,views);
    auto badBox=box;badBox.axes[1]=badBox.axes[0];CHECK(!ValidateTrackedVisibilityBox(BuildVolume(box),badBox));
    CHECK(volume==originalVolume && visibility==originalVisibility && render==originalRender);
    std::printf("Tracked visibility: %u interior/corner samples, largest old-cube volume ratio %.6f; native fixtures, affine basis, frozen asymmetric pairs, validation and failure atomicity passed.\n",checks,largestRatio);
}
