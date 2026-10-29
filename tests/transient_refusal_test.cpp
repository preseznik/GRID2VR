#include "transient_refusal.h"
#include "eye_visibility_frustum.h"
#include "replay_camera.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
using namespace grid2vr;
namespace {
void Check(bool value,int line) { if (!value) { std::fprintf(stderr,"Transient refusal check failed at line %d\n",line);std::exit(1); } }
#define CHECK(x) Check(bool(x),__LINE__)
template<class T> T Load(const std::filesystem::path& path) {
    T value{};std::ifstream file(path,std::ios::binary);
    CHECK(file.read(reinterpret_cast<char*>(value.data()),value.size()) && file.peek()==EOF);return value;
}
}

int main(int argc,char** argv) {
    CHECK(argc==3);
    const std::filesystem::path fixtures=argv[1];
    // Camera-transition refusals before the first eye suspend; the second eye,
    // identity/instruction/memory refusals and unknown reasons still fail.
    for (const char* reason:{"replay_camera_transition","replay_far_plane_changed","replay_view_changed",
                             "replay_history_camera_mismatch","replay_history_transition","eye_camera_pose"}) {
        CHECK(TransientReplayRefusal(reason,1));
        CHECK(!TransientReplayRefusal(reason,2));
        CHECK(!TransientReplayRefusal(reason,0));
    }
    for (const char* reason:{"visibility_not_ready","views_not_frozen","nested_replay","native_stereo","stage6_missing",
                             "replay_inputs_missing","center_capture_or_gpu_guard","scene_frame_changed",
                             "identity_instructions_or_memory","replay_input_read","replay_clip_instructions",
                             "replay_counter_changed","empty_packet_center_witness","eye_clip_instructions",
                             "eye_builder_instructions","eye_previous_camera_or_history","replay_camera_transitio",""})
        CHECK(!TransientReplayRefusal(reason,1));
    CHECK(!TransientReplayRefusal(nullptr,1));
    for (const char* reason:{"native_volume_geometry","eye_frustum_construction","native_frustum_readback"})
        CHECK(TransientVisibilityRefusal(reason));
    for (const char* reason:{"native_guard","pending_volume","volume_not_writable","volume_read","visibility_view_read",
                             "frozen_views","render_view_read","write_readback",""})
        CHECK(!TransientVisibilityRefusal(reason));
    CHECK(!TransientVisibilityRefusal(nullptr));
    CHECK(!TransientRefusalHold(0,1) && TransientRefusalHold(17027,17027) && TransientRefusalHold(17027,17057) &&
          !TransientRefusalHold(17027,17058));

    // Player run 20261029-095744-252: the exact frame-17027 refusal is unchanged.
    const auto entry=Load<CameraContext>(fixtures/"flashback-17027-entry-camera.bin");
    const auto complete=Load<CameraContext>(fixtures/"flashback-17027-complete-camera.bin");
    constexpr uint32_t entryNear=0x3ca0a0a1u,completeNear=0x3e4ccccdu,entryFar=0x451c4000u;
    CHECK(CameraWord(entry,0xe8)==entryNear && CameraWord(complete,0xe8)==completeNear &&
          CameraWord(entry,0xec)==entryFar && CameraWord(complete,0xec)==0x42c80000u);
    CHECK(!ReplayCameraSupported(entry,complete,entryNear,completeNear,entryFar,false));
    CHECK(!ReplayCameraSupported(entry,complete,entryNear,completeNear,entryFar,true));
    CHECK(ReplayCameraSupported(entry,entry,entryNear,entryNear,entryFar,false));
    CHECK(TransientReplayRefusal("replay_camera_transition",1));

    // Player run 20261029-100504-919: the exact frame-20918 refusal is unchanged.
    const auto volume=Load<VisibilityVolume>(fixtures/"flashback-20918-volume.bin");
    const auto source=Load<SceneView>(fixtures/"flashback-20918-source.bin");
    const auto render=Load<SceneView>(fixtures/"flashback-20918-render.bin");
    const auto eyeBytes=Load<std::array<uint8_t,88>>(fixtures/"flashback-20918-eyes.bin");
    static_assert(sizeof(SceneViews)==88);
    SceneViews eyes;memcpy(&eyes,eyeBytes.data(),sizeof eyes);
    CHECK(ValidSceneViews(eyes));
    VisibilityBox box;CHECK(MakeVisibilityBox(volume,source,box));
    std::array<std::array<double,3>,3> native{};
    constexpr size_t offsets[]{0x20,0x10,0x30};constexpr double signs[]{-1,1,-1};
    for (size_t row=0;row<3;++row) for (size_t axis=0;axis<3;++axis) {
        float value{};memcpy(&value,render.data()+offsets[row]+4*axis,4);native[row][axis]=signs[row]*value;
    }
    CHECK(!NativeBasisScaleSupported(native));
    EyeVisibilityFrustum frustum;
    CHECK(!MakeEyeVisibilityFrustum(volume,source,render,eyes,frustum));
    CHECK(TransientVisibilityRefusal("eye_frustum_construction"));

    // The suspension is only safe because every RenderReplay refusal precedes
    // its first game write. Keep that ordering explicit in the source.
    std::ifstream file(argv[2]);std::stringstream text;text << file.rdbuf();
    const auto code=text.str();
    const auto begin=code.find("bool RenderReplay(");CHECK(begin!=std::string::npos);
    const auto end=code.find("\n}\n",begin);CHECK(end!=std::string::npos);
    const auto write=code.find("ApplyReplayHistorySeed(",begin);CHECK(write!=std::string::npos && write<end);
    const auto body=code.substr(write,end-write);
    CHECK(body.find("refuse(")==std::string::npos && body.find("refuseInputs(")==std::string::npos);
    CHECK(code.find("refuse(\"",begin)<write);
    std::puts("Flashback camera/visibility refusals suspend before any write; the guards and both player inputs stay refused.");
}
