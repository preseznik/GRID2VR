#undef NDEBUG
#include "chase_camera_behavior.h"
#include "scene_views.h"
#include "replay_presentation.h"
#include <cassert>
#include <cstring>

using namespace grid2vr;
namespace {
// Synthetic native export of a chase pose. The production adapter leaves this
// conversion to GRID 2, before the existing tracked-eye composition runs.
void Export(const std::array<float,8>& pose,CameraContext& camera,SceneView& view) {
    const ChaseQuaternion q{pose[0],pose[1],pose[2],pose[3]};
    const ChaseVector axes[]{ChaseRotate(q,{1,0,0}),ChaseRotate(q,{0,1,0}),ChaseRotate(q,{0,0,1})};
    std::array<float,16> world{},inverse{};
    for(size_t row=0;row<3;++row) {
        const float values[]{axes[row].x,axes[row].y,axes[row].z};
        for(size_t col=0;col<3;++col) {world[row*4+col]=values[col];inverse[col*4+row]=values[col];}
        inverse[12+row]=-(pose[4]*values[0]+pose[5]*values[1]+pose[6]*values[2]);
    }
    world[12]=pose[4];world[13]=pose[5];world[14]=pose[6];world[15]=inverse[15]=1;
    std::memcpy(camera.data()+0x70,world.data(),64);
    std::memcpy(camera.data()+0x150,inverse.data(),64);
    constexpr size_t offsets[]{0x20,0x10,0x30};constexpr float signs[]{-1,1,-1};
    for(size_t row=0;row<3;++row)for(size_t col=0;col<3;++col) {
        const float value=world[row*4+col]*signs[row];std::memcpy(view.data()+offsets[row]+col*4,&value,4);
    }
    std::memcpy(view.data()+0x40,pose.data()+4,12);
}
std::array<float,3> Position(const SceneView& view) {
    std::array<float,3> result{};std::memcpy(result.data(),view.data()+0x40,12);return result;
}
}
int main() {
    for(float zoom:{.6f,1.f,1.8f,10.f})for(float yaw:{0.f,.7f,3.14159265f}) {
        std::array<float,8> chase{0,0,0,1,1000,42,-25,1};
        assert(OrbitChasePose(chase,{1000,40,-20},{0,1,0},{1,0,0},yaw,.3f,zoom));
        const auto base=chase;
        CameraContext camera{};SceneView view{};view.fill(0x3a);Export(chase,camera,view);
        const auto unchangedCamera=camera;
        ScenePose head{{0,std::sin(.2f),0,std::cos(.2f)},{.2f,.1f,-.3f}};
        SceneViews eyes;eyes.frusta={diagnosticFrustum,diagnosticFrustum};
        assert(SeparateSceneEyes(head,.064f,eyes.poses));
        SceneViewFrame frame;assert(frame.Freeze(123,eyes));
        // Both eyes use the same base camera and frozen tracking sample.
        eyes.poses[0].position[0]=1; // Late tracking cannot alter this pair.
        std::array<SceneView,2> rendered;
        for(size_t eye=0;eye<2;++eye)assert(ApplyScenePose(camera,view,frame.Get(123)->poses[eye],rendered[eye]));
        const auto left=Position(rendered[0]),right=Position(rendered[1]);
        float separationSquared{};for(size_t axis=0;axis<3;++axis)separationSquared+=(right[axis]-left[axis])*(right[axis]-left[axis]);
        assert(std::abs(std::sqrt(separationSquared)-.064f)<.00015f); // Zoom never scales IPD.
        const auto offset=ChaseRotate({chase[0],chase[1],chase[2],chase[3]},
            {head.position[0],head.position[1],head.position[2]});
        const float expected[]{chase[4]+offset.x,chase[5]+offset.y,chase[6]+offset.z};
        for(size_t axis=0;axis<3;++axis)assert(std::abs((left[axis]+right[axis])*.5f-expected[axis])<.00015f);
        assert(frame.Finish(123,true) && chase==base && camera==unchangedCamera);

        // Recentring resets headset alignment independently from orbit/zoom.
        SceneView recentered{};assert(ApplyScenePose(camera,view,ScenePose{},recentered));
        assert(Position(recentered)==Position(view) && chase==base);
        // A distant native chase base is valid; excessive tracked translation
        // still refuses at the existing two-unit limit.
        auto invalid=head;invalid.position={2.01f,0,0};const auto before=recentered;
        assert(!ApplyScenePose(camera,view,invalid,recentered) && recentered==before);
        assert(maxSceneTranslation==2.0 && camera==unchangedCamera);
    }
    assert(!ReplayPresentationAllowed(false,RecordedReplayState::Active));
    assert(ReplayPresentationAllowed(true,RecordedReplayState::Active));
    return 0;
}
