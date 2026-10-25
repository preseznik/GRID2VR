#include "visibility_volume.h"
#include <cstdio>
#include <fstream>
using namespace grid2vr;
int main(int argc,char** argv) {
    // Known box used as a native-volume fixture, displaced from the origin.
    const float center[]{300,20,-900};
    SceneView view{}; memcpy(view.data()+0x40,center,12);
    std::array<float,56> words{};
    for (unsigned axis=0;axis<3;++axis) for (unsigned side=0;side<2;++side) {
        const unsigned plane=2*axis+side;
        const float n=side?1.f:-1.f;
        words[4*plane+axis]=n; words[4*plane+3]=100-n*center[axis];
    }
    for (unsigned corner=0;corner<8;++corner) for (unsigned axis=0;axis<3;++axis)
        words[24+4*corner+axis]=center[axis]+((corner&(1u<<axis))?100.f:-100.f);
    VisibilityVolume source{}; memcpy(source.data(),words.data(),224);
    VisibilityBox box;
    if (!MakeVisibilityBox(source,view,box) || box.radius<=std::sqrt(30000.f)+8.f) return 1;
    // A rotated source corner can become the furthest point along any axis.
    for (unsigned axis=0;axis<3;++axis) for (float sign:{-1.f,1.f}) {
        const double point=center[axis]+sign*std::sqrt(30000.0);
        if (std::abs(point*box.matrix[axis*5]+box.matrix[12+axis])>=1) return 2;
    }
    std::array<float,56> expanded{};
    for (unsigned axis=0;axis<3;++axis) for (unsigned side=0;side<2;++side) {
        const unsigned plane=2*axis+side; const float n=side?1.f:-1.f;
        expanded[4*plane+axis]=n; expanded[4*plane+3]=box.radius-n*center[axis];
    }
    for (unsigned corner=0;corner<8;++corner) for (unsigned axis=0;axis<3;++axis)
        expanded[24+4*corner+axis]=center[axis]+((corner&(1u<<axis))?box.radius:-box.radius);
    VisibilityVolume result{}; memcpy(result.data(),expanded.data(),224);
    if (!ValidateVisibilityBox(result,box)) return 3;
    auto bad=result; memcpy(bad.data()+0x70,bad.data()+0x60,16);
    if (ValidateVisibilityBox(bad,box)) return 4; // Duplicate corner.
    bad=result; memcpy(bad.data()+16,bad.data(),16);
    if (ValidateVisibilityBox(bad,box)) return 5; // Duplicate plane.
    expanded[3]+=1; memcpy(bad.data(),expanded.data(),224);
    if (ValidateVisibilityBox(bad,box)) return 6;
    auto originalBox=box;
    words[0]=0; memcpy(bad.data(),words.data(),224);
    if (MakeVisibilityBox(bad,view,box) || box.matrix!=originalBox.matrix) return 7;
    words[0]=-1; words[24]=std::numeric_limits<float>::quiet_NaN(); memcpy(bad.data(),words.data(),224);
    if (MakeVisibilityBox(bad,view,box)) return 8;
    // The native volume from the 120-group run was too small for the right
    // asymmetric eye. Keep these tiny CPU inputs as a permanent regression.
    if (argc!=4) return 9;
    VisibilityVolume native{};SceneView visibility{},render{};
    std::ifstream volumeFile(argv[1],std::ios::binary),visibilityFile(argv[2],std::ios::binary),renderFile(argv[3],std::ios::binary);
    if (!volumeFile.read(reinterpret_cast<char*>(native.data()),224) || volumeFile.peek()!=EOF ||
        !visibilityFile.read(reinterpret_cast<char*>(visibility.data()),144) || visibilityFile.peek()!=EOF ||
        !renderFile.read(reinterpret_cast<char*>(render.data()),144) || renderFile.peek()!=EOF) return 10;
    const std::array<EyeFrustum,2> eyes{{{-1,.8f,-.7f,.6f},{-.8f,1,-.7f,.6f}}};
    VisibilityBox nativeBox,eyeBox;
    if (!MakeVisibilityBox(native,visibility,nativeBox) || !MakeEyeVisibilityBox(native,visibility,render,eyes,eyeBox) ||
        eyeBox.radius<nativeBox.radius+100 || eyeBox.radius>2500) return 11;
    // Wider headset FOV and a shifted render origin must also fit. Rotate a
    // far corner onto every box axis, with the full supported translation.
    auto shifted=render;float position{};memcpy(&position,shifted.data()+0x40,4);position+=30;memcpy(shifted.data()+0x40,&position,4);
    const std::array<EyeFrustum,2> wide{{{-1.38f,.85f,-1.43f,.97f},{-.85f,1.38f,-1.43f,.97f}}};
    VisibilityBox wideBox;
    if (!MakeEyeVisibilityBox(native,visibility,shifted,wide,wideBox) || wideBox.radius<=eyeBox.radius) return 12;
    const double farCorner=(1501*std::sqrt(1+1.38*1.38+1.43*1.43)+maxSceneTranslation)*std::sqrt(1.001);
    for (unsigned axis=0;axis<3;++axis) for (double sign:{-1.,1.}) {
        float origin{};memcpy(&origin,shifted.data()+0x40+4*axis,4);
        if (std::abs((origin+sign*farCorner)*wideBox.matrix[5*axis]+wideBox.matrix[12+axis])>=1) return 13;
    }
    const auto saved=eyeBox;
    auto badEyes=eyes;badEyes[1].right=std::numeric_limits<float>::infinity();
    if (MakeEyeVisibilityBox(native,visibility,render,badEyes,eyeBox) || eyeBox.matrix!=saved.matrix) return 14;
    badEyes=eyes;badEyes[1].down=0;
    if (MakeEyeVisibilityBox(native,visibility,render,badEyes,eyeBox) || eyeBox.matrix!=saved.matrix) return 15;
    auto badRender=render;memset(badRender.data()+0x10,0,12);
    if (MakeEyeVisibilityBox(native,visibility,badRender,eyes,eyeBox) || eyeBox.matrix!=saved.matrix ||
        MakeEyeVisibilityBox(native,visibility,render,{},eyeBox)) return 16;
    puts("Visibility bounds: native and asymmetric eye containment, shifted origins, wide FOV, validation and failure atomicity checked.");
}
