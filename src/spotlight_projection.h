#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace grid2vr::spotlightprojection {
inline constexpr uint64_t VertexShader=0xdedcbb651ce31ec3ull;
inline constexpr std::array<uint64_t,6> PixelShaders{
    0x1328844cfb79ab34ull,0xce3893511fabf53dull,0x0bbb6a6010ddd7c4ull,
    0x1f779a4bfb080f9eull,0x13e126614b7cc867ull,0xd9c9b5521a549073ull};
struct Layout {size_t bytes{},mapping{};bool shadow{};};
constexpr Layout PixelLayout(uint64_t vs,uint64_t ps) {
    if(vs!=VertexShader)return {};
    for(size_t i=0;i<PixelShaders.size();++i)if(ps==PixelShaders[i])
        return {i<3?432u:464u,i<3?368u:352u,i==2 || i==5};
    return {};
}
using Matrix=std::array<double,16>;
struct Change {Matrix centerToEye{},eyeToCenter{};bool valid{};};
namespace detail {
inline Matrix Multiply(const Matrix& a,const Matrix& b) {
    Matrix result{};
    for(size_t r=0;r<4;++r)for(size_t c=0;c<4;++c)for(size_t k=0;k<4;++k)
        result[r*4+c]+=a[r*4+k]*b[k*4+c];
    return result;
}
inline Matrix ReadMatrix(const uint8_t* bytes) {
    std::array<float,16> values{};memcpy(values.data(),bytes,64);
    Matrix result{};for(size_t i=0;i<16;++i)result[i]=values[i];return result;
}
inline bool Finite(const Matrix& value) {
    for(double x:value)if(!std::isfinite(x))return false;return true;
}
inline bool Affine(const Matrix& value) {
    return Finite(value) && std::abs(value[3])<1e-7 && std::abs(value[7])<1e-7 &&
        std::abs(value[11])<1e-7 && std::abs(value[15]-1)<1e-4;
}
inline bool Inverse(const Matrix& a,Matrix& result) {
    if(!Affine(a))return false;
    const double determinant=a[0]*(a[5]*a[10]-a[6]*a[9])-a[1]*(a[4]*a[10]-a[6]*a[8])+
        a[2]*(a[4]*a[9]-a[5]*a[8]);
    if(!std::isfinite(determinant) || determinant<.99 || determinant>1.01)return false;
    Matrix b{};
    b[0]=(a[5]*a[10]-a[6]*a[9])/determinant;b[1]=(a[2]*a[9]-a[1]*a[10])/determinant;
    b[2]=(a[1]*a[6]-a[2]*a[5])/determinant;b[4]=(a[6]*a[8]-a[4]*a[10])/determinant;
    b[5]=(a[0]*a[10]-a[2]*a[8])/determinant;b[6]=(a[2]*a[4]-a[0]*a[6])/determinant;
    b[8]=(a[4]*a[9]-a[5]*a[8])/determinant;b[9]=(a[1]*a[8]-a[0]*a[9])/determinant;
    b[10]=(a[0]*a[5]-a[1]*a[4])/determinant;b[15]=1;
    for(size_t c=0;c<3;++c)for(size_t k=0;k<3;++k)b[12+c]-=a[12+k]*b[k*4+c];
    if(!Finite(b))return false;result=b;return true;
}
inline bool Camera(std::span<const uint8_t> camera,Matrix& view,Matrix& inverse) {
    if(camera.size()!=512)return false;
    const auto world=ReadMatrix(camera.data()+0x70);view=ReadMatrix(camera.data()+0x150);
    if(!Affine(world) || !Affine(view))return false;
    // Match the existing native camera conditioning allowance, including its
    // small interpolation contraction. A transpose is not an exact inverse.
    for(size_t row=0;row<3;++row) {
        double diagonal{},radius{};
        for(size_t col=0;col<3;++col) {
            double dot{};for(size_t k=0;k<3;++k)dot+=world[row*4+k]*world[col*4+k];
            if(row==col)diagonal=dot;else radius+=std::abs(dot);
        }
        if(diagonal-radius<.998 || diagonal+radius>1.001)return false;
    }
    for(unsigned order=0;order<2;++order) {
        const auto& a=order?view:world;const auto& b=order?world:view;
        for(size_t row=0;row<4;++row)for(size_t col=0;col<4;++col) {
            double value{},magnitude{};
            for(size_t k=0;k<4;++k){const double term=a[row*4+k]*b[k*4+col];value+=term;magnitude+=std::abs(term);}
            constexpr double u=std::numeric_limits<float>::epsilon()/2.,gamma7=7*u/(1-7*u);
            const double tolerance=row==3 && col<3 && gamma7*magnitude>.0002?gamma7*magnitude:.0002;
            if(std::abs(value-(row==col?1.:0.))>tolerance)return false;
        }
    }
    return Inverse(view,inverse);
}
inline bool StoreMatrix(uint8_t* bytes,const Matrix& matrix) {
    std::array<float,16> values{};
    for(size_t i=0;i<16;++i){values[i]=static_cast<float>(matrix[i]);if(!std::isfinite(values[i]))return false;}
    memcpy(bytes,values.data(),64);return true;
}
}
// Row-vector convention is established by the shipped shader mul/mad sequence
// and the native camera view matrix at +0x150. These are coordinate changes,
// not new projections; asymmetric eye FOV does not enter this calculation.
inline bool MakeChange(std::span<const uint8_t> centerCamera,std::span<const uint8_t> eyeCamera,Change& result) {
    Matrix center{},centerInverse{},eye{},eyeInverse{};
    if(!detail::Camera(centerCamera,center,centerInverse) || !detail::Camera(eyeCamera,eye,eyeInverse))return false;
    Change candidate{detail::Multiply(centerInverse,eye),detail::Multiply(eyeInverse,center),true};
    if(!detail::Finite(candidate.centerToEye) || !detail::Finite(candidate.eyeToCenter))return false;
    result=candidate;return true;
}
inline bool MutablePixelByte(const Layout& layout,size_t at) {
    return (at>=32 && at<44) || (at>=48 && at<60) ||
        (at>=layout.mapping && at<layout.mapping+64) || (layout.shadow && at>=144 && at<208);
}
// The caller must bind this to the exact same-scene center draw and resource.
// Only these fields may legitimately differ when comparing an eye upload.
inline bool SamePixelInputs(uint64_t vs,uint64_t ps,std::span<const uint8_t> center,std::span<const uint8_t> eye) {
    const auto layout=PixelLayout(vs,ps);
    if(!layout.bytes || center.size()!=layout.bytes || eye.size()!=layout.bytes)return false;
    for(size_t i=0;i<layout.bytes;++i)if(!MutablePixelByte(layout,i) && center[i]!=eye[i])return false;
    return true;
}
// Derive from original center constants every time: using an eye's current
// constants would double-transform lights already refreshed by native code.
// Output is not changed on refusal; unrelated and unused bytes stay identical.
inline bool CorrectPixelConstants(uint64_t vs,uint64_t ps,std::span<const uint8_t> center,
                                  const Change& change,std::span<uint8_t> output) {
    const auto layout=PixelLayout(vs,ps);
    if(!layout.bytes || center.size()!=layout.bytes || output.size()!=layout.bytes || !change.valid ||
        !detail::Affine(change.centerToEye) || !detail::Affine(change.eyeToCenter))return false;
    std::array<uint8_t,464> candidate{};memcpy(candidate.data(),center.data(),center.size());
    for(size_t offset:{32u,48u}) {
        std::array<float,3> source{},target{};memcpy(source.data(),center.data()+offset,12);
        for(float x:source)if(!std::isfinite(x))return false;
        double length{};std::array<double,3> transformed{};
        for(size_t c=0;c<3;++c){
            transformed[c]=offset==32?change.centerToEye[12+c]:0;
            for(size_t k=0;k<3;++k)transformed[c]+=double(source[k])*change.centerToEye[k*4+c];
            length+=transformed[c]*transformed[c];
        }
        // The native producer normalizes its view-space direction too.
        if(offset==48 && (!std::isfinite(length) || length<1e-12))return false;
        for(size_t c=0;c<3;++c){target[c]=static_cast<float>(transformed[c]/(offset==48?std::sqrt(length):1));if(!std::isfinite(target[c]))return false;}
        memcpy(candidate.data()+offset,target.data(),12);
    }
    for(unsigned matrix=0;matrix<(layout.shadow?2u:1u);++matrix) {
        const size_t offset=matrix?144:layout.mapping;
        const auto original=detail::ReadMatrix(center.data()+offset);
        if(!detail::Finite(original) || !detail::StoreMatrix(candidate.data()+offset,detail::Multiply(change.eyeToCenter,original)))return false;
    }
    memcpy(output.data(),candidate.data(),output.size());return true;
}
// The exact spotlight VS reads only modelView at +128. Its native eye upload
// should already equal center modelView * centerToEye; do not transform it twice.
inline bool VertexViewMatches(std::span<const uint8_t> center,std::span<const uint8_t> eye,const Change& change) {
    if(center.size()!=192 || eye.size()!=192 || !change.valid)return false;
    const auto a=detail::ReadMatrix(center.data()+128),b=detail::ReadMatrix(eye.data()+128);
    if(!detail::Finite(a) || !detail::Finite(b))return false;
    const auto expected=detail::Multiply(a,change.centerToEye);
    for(size_t i=0;i<16;++i) {
        const double tolerance=.001+std::abs(expected[i])*2e-6;
        if(std::abs(expected[i]-b[i])>tolerance)return false;
    }
    return true;
}
}
