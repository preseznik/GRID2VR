#include "spotlight_projection.h"
#include <algorithm>
#include <cstdio>
#include <vector>
using namespace grid2vr::spotlightprojection;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
namespace {
using Vec=std::array<double,4>;
using Camera=std::array<uint8_t,512>;
Matrix Identity(){return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};}
// Independent fixture algebra and world-space oracle. Production uses an
// adjugate inverse; these fixtures use rigid transforms and their transpose.
Matrix Product(const Matrix& a,const Matrix& b){Matrix c{};for(int r=0;r<4;++r)for(int k=0;k<4;++k)for(int j=0;j<4;++j)c[r*4+j]+=a[r*4+k]*b[k*4+j];return c;}
Vec Transform(Vec a,const Matrix& b){Vec c{};for(int k=0;k<4;++k)for(int j=0;j<4;++j)c[j]+=a[k]*b[k*4+j];return c;}
Matrix InverseRigid(const Matrix& a){auto b=Identity();for(int r=0;r<3;++r)for(int c=0;c<3;++c)b[r*4+c]=a[c*4+r];for(int c=0;c<3;++c)for(int k=0;k<3;++k)b[12+c]-=a[12+k]*b[k*4+c];return b;}
Matrix Pose(double yaw,double pitch,double roll,Vec position){
    auto y=Identity(),p=y,r=y;
    y[0]=y[10]=cos(yaw);y[2]=-sin(yaw);y[8]=sin(yaw);
    p[5]=p[10]=cos(pitch);p[6]=sin(pitch);p[9]=-sin(pitch);
    r[0]=r[5]=cos(roll);r[1]=sin(roll);r[4]=-sin(roll);
    auto w=Product(Product(y,p),r);for(int i=0;i<3;++i)w[12+i]=position[i];return w;
}
void PutMatrix(uint8_t* dst,const Matrix& a){for(int i=0;i<16;++i){const float f=float(a[i]);memcpy(dst+i*4,&f,4);}}
Matrix GetMatrix(const uint8_t* src){Matrix m{};for(int i=0;i<16;++i){float f{};memcpy(&f,src+i*4,4);m[i]=f;}return m;}
Vec GetVector(const uint8_t* src){Vec v{};for(int i=0;i<4;++i){float f{};memcpy(&f,src+i*4,4);v[i]=f;}return v;}
void PutVector(uint8_t* dst,Vec v){for(int i=0;i<4;++i){const float f=float(v[i]);memcpy(dst+i*4,&f,4);}}
Camera Native(const Matrix& world){Camera c{};PutMatrix(c.data()+0x70,world);PutMatrix(c.data()+0x150,InverseRigid(world));return c;}
double Dot(Vec a,Vec b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
Vec Unit(Vec a){const double length=sqrt(Dot(a,a));for(int i=0;i<3;++i)a[i]/=length;return a;}
double Error(Vec a,Vec b,int n=3){double e=0;for(int i=0;i<n;++i)e=std::max(e,std::abs(a[i]-b[i]));return e;}
struct Pixel {double distance,cone,diffuse;Vec clip,shadow;};
Pixel Evaluate(Vec point,Vec normal,Vec light,Vec direction,const Matrix& mapping,const Matrix& shadow){
    Vec toward{};for(int i=0;i<3;++i)toward[i]=light[i]-point[i];
    const double distance=sqrt(Dot(toward,toward));toward=Unit(toward);
    return {distance,-Dot(toward,direction),Dot(toward,normal),Transform(point,mapping),Transform(point,shadow)};
}
}
int main(){
    const Vec light{2,3,-4,1},direction=Unit({.15,-.35,-.92,0}),normal{0,1,0,0};
    const Matrix projector{.8,.11,.05,.03, -.2,.75,.07,-.08, .13,.19,.2,-1, -.3,.7,-.1,1.5};
    const Matrix shadow{.7,-.05,.03,.02, .14,.6,-.04,.01, -.1,.23,.95,-1, .5,.2,-.3,2};
    const auto centerWorld=Pose(.17,-.04,.02,{1,2,3,1}),centerView=InverseRigid(centerWorld);
    const auto centerCamera=Native(centerWorld);
    unsigned comparisons=0;bool staleWasWrong=false;
    for(unsigned variant=0;variant<PixelShaders.size();++variant){
        const auto hash=PixelShaders[variant];const auto layout=PixelLayout(VertexShader,hash);
        CHECK(layout.bytes==(variant<3?432:464));CHECK(layout.mapping==(variant<3?368:352));
        CHECK(layout.shadow==(variant==2 || variant==5));
        std::vector<uint8_t> source(layout.bytes,0x5a),output(layout.bytes,0xcc);
        auto lightCenter=Transform(light,centerView),dirCenter=Transform(direction,centerView);lightCenter[3]=23;dirCenter[3]=-17;
        PutVector(source.data()+32,lightCenter);PutVector(source.data()+48,dirCenter);
        PutMatrix(source.data()+layout.mapping,Product(centerWorld,projector));
        if(layout.shadow)PutMatrix(source.data()+144,Product(centerWorld,shadow));
        const auto untouched=source;
        for(unsigned scenario=0;scenario<5;++scenario){
            const auto eyeWorld=Pose(.17+.23*scenario,-.04-.075*scenario,.02+.08*scenario,
                {1+(scenario%2?-.032:.032),2+.13*scenario,3-.11*scenario,1});
            const auto eyeView=InverseRigid(eyeWorld);const auto eyeCamera=Native(eyeWorld);Change change{};
            CHECK(MakeChange(centerCamera,eyeCamera,change));
            CHECK(CorrectPixelConstants(VertexShader,hash,source,change,output));CHECK(source==untouched);
            for(size_t i=0;i<source.size();++i)if(!MutablePixelByte(layout,i))CHECK(output[i]==source[i]);
            CHECK(SamePixelInputs(VertexShader,hash,source,output));
            CHECK(GetVector(output.data()+32)[3]==23 && GetVector(output.data()+48)[3]==-17);
            for(int z=-2;z>=-18;z-=4)for(int x=-3;x<=4;x+=2){
                const Vec worldPoint{double(x),0,double(z),1};
                const auto expected=Evaluate(worldPoint,normal,light,direction,projector,shadow);
                const auto pointEye=Transform(worldPoint,eyeView),normalEye=Transform(normal,eyeView);
                const auto actual=Evaluate(pointEye,normalEye,GetVector(output.data()+32),GetVector(output.data()+48),
                    GetMatrix(output.data()+layout.mapping),layout.shadow?GetMatrix(output.data()+144):Product(eyeWorld,shadow));
                CHECK(std::abs(actual.distance-expected.distance)<.00003);
                CHECK(std::abs(actual.cone-expected.cone)<.000003);
                CHECK(std::abs(actual.diffuse-expected.diffuse)<.000003);
                CHECK(Error(actual.clip,expected.clip,4)<.00002);
                if(layout.shadow)CHECK(Error(actual.shadow,expected.shadow,4)<.00002);
                const auto stale=Transform(pointEye,GetMatrix(source.data()+layout.mapping));
                if(Error(stale,expected.clip,4)>.5)staleWasWrong=true;
                ++comparisons;
            }
            // VS ray positions already rebuilt by the native camera must pass;
            // blindly retaining the center model-view must fail after rotation.
            std::array<uint8_t,192> centerVs{},eyeVs{};
            const auto model=Pose(.3,.2,-.1,{3,2,-8,1});
            PutMatrix(centerVs.data()+128,Product(model,centerView));PutMatrix(eyeVs.data()+128,Product(model,eyeView));
            CHECK(VertexViewMatches(centerVs,eyeVs,change));
            if(scenario>0)CHECK(!VertexViewMatches(centerVs,centerVs,change));
        }
        Change change{};CHECK(MakeChange(centerCamera,centerCamera,change));
        const auto before=output;
        CHECK(!CorrectPixelConstants(VertexShader^1,hash,source,change,output));CHECK(output==before);
        CHECK(!CorrectPixelConstants(VertexShader,hash^1,source,change,output));CHECK(output==before);
        CHECK(!CorrectPixelConstants(VertexShader,hash,std::span(source).first(source.size()-16),change,output));CHECK(output==before);
        auto invalid=source;const float nan=std::numeric_limits<float>::quiet_NaN();memcpy(invalid.data()+32,&nan,4);
        CHECK(!CorrectPixelConstants(VertexShader,hash,invalid,change,output));CHECK(output==before);
        invalid=source;memset(invalid.data()+48,0,12);CHECK(!CorrectPixelConstants(VertexShader,hash,invalid,change,output));CHECK(output==before);
        invalid=source;memcpy(invalid.data()+layout.mapping+40,&nan,4);CHECK(!CorrectPixelConstants(VertexShader,hash,invalid,change,output));CHECK(output==before);
        invalid=source;invalid[16]^=1;CHECK(!SamePixelInputs(VertexShader,hash,source,invalid));
        auto invalidChange=change;invalidChange.centerToEye[0]=std::numeric_limits<double>::infinity();
        CHECK(!CorrectPixelConstants(VertexShader,hash,source,invalidChange,output));CHECK(output==before);
        invalidChange=change;invalidChange.eyeToCenter[12]=std::numeric_limits<double>::quiet_NaN();
        CHECK(!CorrectPixelConstants(VertexShader,hash,source,invalidChange,output));CHECK(output==before);
        invalidChange=change;invalidChange.valid=false;
        CHECK(!CorrectPixelConstants(VertexShader,hash,source,invalidChange,output));CHECK(output==before);
        // Identity must preserve the center fields to float rounding, including
        // the direction's expected native normalization, without moving its pool.
        CHECK(CorrectPixelConstants(VertexShader,hash,source,change,output));
        CHECK(Error(GetVector(output.data()+32),GetVector(source.data()+32),4)<1e-6);
        CHECK(Error(GetVector(output.data()+48),GetVector(source.data()+48),4)<1e-6);
    }
    CHECK(staleWasWrong && comparisons==600);
    Change change{};auto bad=centerCamera;
    const float nan=std::numeric_limits<float>::quiet_NaN();memcpy(bad.data()+0x70,&nan,4);
    CHECK(!MakeChange(centerCamera,bad,change));CHECK(!change.valid);
    bad=centerCamera;const float zero=0;memcpy(bad.data()+0x150,&zero,4);CHECK(!MakeChange(centerCamera,bad,change));
    bad=centerCamera;const float scale=1.1f;memcpy(bad.data()+0x70,&scale,4);CHECK(!MakeChange(centerCamera,bad,change));
    CHECK(!MakeChange(std::span(centerCamera).first(511),centerCamera,change));
    // Native large world coordinates must not trigger a false inverse rejection.
    auto farWorld=Pose(.6,.2,.1,{1559.7655,97.9,-1636.7,1});
    const auto farCamera=Native(farWorld);farWorld[12]+=.032;
    CHECK(MakeChange(farCamera,Native(farWorld),change));
    // Deliberately contracted but valid native basis: use the true affine inverse.
    auto contracted=centerWorld;for(int r=0;r<3;++r)for(int c=0;c<3;++c)contracted[r*4+c]*=.9997;
    auto inverse=InverseRigid(centerWorld);for(int r=0;r<3;++r)for(int c=0;c<3;++c)inverse[r*4+c]/=.9997;
    for(int c=0;c<3;++c){inverse[12+c]=0;for(int k=0;k<3;++k)inverse[12+c]-=contracted[12+k]*inverse[k*4+c];}
    Camera contractedCamera{};PutMatrix(contractedCamera.data()+0x70,contracted);PutMatrix(contractedCamera.data()+0x150,inverse);
    CHECK(MakeChange(contractedCamera,centerCamera,change));
    puts("Spotlight correction: six shader layouts, 600 world-point pixel invariants, stale counterexample, both-eye transforms, exact unchanged bytes and invalid-input refusal pass.");
}
