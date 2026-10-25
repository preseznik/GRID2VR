#include "eye_projection.h"
#include <cstdio>
#include <fstream>

using namespace grid2vr;
int main(int argc,char** argv) {
    if(argc<2) return 1;
    unsigned nativeRoundingCases{};
    for(int file=1;file<argc;++file) {
        CameraContext native{};
        std::ifstream input(argv[file],std::ios::binary);
        if(!input.read(reinterpret_cast<char*>(native.data()),native.size()) || input.peek()!=EOF) return 2;
        std::array<float,16> p{},v{},vp{};
        memcpy(p.data(),native.data()+0x110,64);memcpy(v.data(),native.data()+0x150,64);memcpy(vp.data(),native.data()+0x190,64);
        // The retained native builder rounds in float. Recomputing an unchanged
        // depth column in double is not guaranteed to preserve its exact bytes.
        for(unsigned row=0;row<4;++row) for(unsigned col=2;col<4;++col) {
            double sum{};
            for(unsigned k=0;k<4;++k) sum+=double(v[row*4+k])*p[k*4+col];
            const float rounded=static_cast<float>(sum);
            nativeRoundingCases+=memcmp(&rounded,&vp[row*4+col],4)!=0;
        }
        for(const EyeFrustum fov: {diagnosticFrustum,EyeFrustum{-1.2f,.9f,-1.f,.8f},EyeFrustum{-.9f,1.2f,-1.f,.8f}}) {
            auto camera=native;
            if(!ApplyEyeFrustum(camera,fov)) return 3;
            for(size_t byte=0;byte<camera.size();++byte) {
                const bool lateralP=(byte>=0x110 && byte<0x114) || (byte>=0x124 && byte<0x128) || (byte>=0x130 && byte<0x138);
                const bool lateralVp=byte>=0x190 && byte<0x1d0 && (byte-0x190)%16<8;
                if(!lateralP && !lateralVp && camera[byte]!=native[byte]) {
                    printf("Unrequested depth/state change: %s byte 0x%zx\n",argv[file],byte);return 4;
                }
            }
            // New X/Y still implement the requested frustum, with the existing
            // lateral math. Verify several world points, not only matrix fields.
            std::array<float,16> actual{};memcpy(actual.data(),camera.data()+0x190,64);
            for(const std::array<double,4> point: {std::array<double,4>{0,0,0,1},{120,4,-73,1},{-7,31,19,1}}) {
                double x{},y{},z{};
                for(unsigned row=0;row<4;++row) {x+=point[row]*v[row*4];y+=point[row]*v[row*4+1];z+=point[row]*v[row*4+2];}
                const double expected[]{x*(2.f/(fov.right-fov.left))+z*((fov.right+fov.left)/(fov.right-fov.left)),
                    y*(2.f/(fov.up-fov.down))+z*((fov.up+fov.down)/(fov.up-fov.down))};
                for(unsigned col=0;col<2;++col) {
                    double clip{};for(unsigned row=0;row<4;++row)clip+=point[row]*actual[row*4+col];
                    if(std::abs(clip-expected[col])>1e-3+1e-6*std::abs(expected[col]))return 5;
                }
            }
            const auto projected=camera;
            if(!ApplyEyeFrustum(camera,fov) || camera!=projected)return 6;
            auto invalid=camera;const float nan=NAN;memcpy(invalid.data()+0x198,&nan,4);
            const auto untouched=invalid;
            if(ApplyEyeFrustum(invalid,fov) || invalid!=untouched)return 7;
        }
    }
    if(!nativeRoundingCases)return 8; // Keep a real regression fixture, not just exact toy matrices.
    printf("Native depth/W and unrelated camera bytes preserved across %d fixtures; %u rounding differences reproduced.\n",argc-1,nativeRoundingCases);
}
