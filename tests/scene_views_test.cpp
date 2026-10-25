#include "scene_views.h"
#include <sstream>
using namespace grid2vr;
int main() {
    SceneViews input{{ScenePose{},ScenePose{}},{diagnosticFrustum,diagnosticFrustum}};
    input.poses[0].position[0]=-.032f;input.poses[1].position[0]=.032f;
    SceneViewFrame frame;
    if (frame.Get(9) || !frame.Freeze(9,input) || !frame.Get(9) || frame.Get(10)) return 1;
    input.poses[0].position[1]=.1f;
    if (frame.Get(9)->poses[0].position[1]!=0 || !frame.Finish(9,true) || frame.Get(9)) return 2;
    if (!frame.Freeze(10,input) || frame.Finish(11,true) || frame.Freeze(11,input)) return 3;
    SceneViewFrame gap;
    if (!gap.Freeze(9,input) || !gap.Finish(9,true) || gap.Freeze(11,input)) return 4;
    SceneViewFrame dropped;
    if (!dropped.Freeze(9,input) || dropped.Finish(9,false) || dropped.Freeze(10,input)) return 5;
    SceneViewFrame duplicate;
    if (!duplicate.Freeze(9,input) || duplicate.Freeze(9,input) || duplicate.Get(9) || duplicate.Finish(9,true)) return 6;
    std::ostringstream rows;
    for (unsigned i=0;i<2;++i) for (unsigned eye=0;eye<2;++eye) {
        for (auto v:input.poses[eye].orientation) rows<<v<<' ';
        for (auto v:input.poses[eye].position) rows<<v<<' ';
        rows<<"-1 .8 -.7 .6 ";
    }
    std::vector<SceneViews> result;std::istringstream good(rows.str());
    if (!ReadSceneViews(good,2,result) || result.size()!=2) return 7;
    std::istringstream extra(rows.str()+"1"),shortInput(rows.str());
    if (ReadSceneViews(extra,2,result) || ReadSceneViews(shortInput,3,result) || result.size()!=2) return 8;
    std::string extended;
    for (unsigned i=0;i<450;++i) extended+=rows.str();
    std::istringstream longInput(extended),tooLong(extended);
    if (!ReadSceneViews(longInput,900,result) || result.size()!=900 || ReadSceneViews(tooLong,901,result)) return 10;
    SceneViewFrame session;
    if (!session.Freeze(20,input) || session.ResetEpoch() || !session.Finish(20,true) ||
        !session.ResetEpoch() || session.Get(20) || !session.Freeze(80,input) ||
        !session.Finish(80,true) || !session.ResetEpoch() || session.Freeze(79,input) || session.ResetEpoch()) return 11;
    if (dropped.ResetEpoch() || gap.ResetEpoch()) return 12;
    SceneViewFrame menu;
    if (!menu.Freeze(30,input) || !menu.CancelBeforeReplay(30) || menu.Pending() || menu.Get(30) ||
        !menu.ResetEpoch() || !menu.Freeze(90,input) || !menu.Finish(90,true)) return 13;
    if (menu.CancelBeforeReplay(90) || menu.ResetEpoch() || dropped.CancelBeforeReplay(9)) return 14;
    input.frusta[0].left=1;
    if (ValidSceneViews(input)) return 9;
    return 0;
}
