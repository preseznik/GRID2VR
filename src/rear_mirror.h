#pragma once
#include "vpl_order.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace grid2vr {
// Optional rear-view mirrors (ProJict 18da44dc).
//
// GRID 2 has no mirror camera. The mirror glass is car glass that reflects the
// car's environment cube (_TEnvironmentMap), and car_basic samples that cube at
// (x, y, -z) of the world reflection vector, so a backward-facing mirror shows
// the forward view flipped. The cube also holds only static track and sky.
//
// With the option on, the engine replays a third, rear-facing camera each
// frame (engine::RearMirrorPass) and its finished image is copied here. Once a
// frame a compute pass builds a substitute cube: for every texel direction d it
// takes the world direction w=(d.x,d.y,-d.z) the glass will actually look up,
// shows the rear camera's image where that camera sees w, and the game's own
// cube at w elsewhere (which also un-flips the rest of the environment). The
// substitute is bound only in place of the game's cube for the car reflection
// shaders, then the game's view is restored after the draw. The rear image is
// tone-mapped while the cube is encoded, so a GPU-side running gain matches
// the rear pixels to the game cube over the directions both cover.
class RearMirror {
public:
    static constexpr UINT cubeSize=512;
    // Copies the rear replay's finished image (same frame, same scene id).
    bool CaptureRear(ID3D11DeviceContext* context,uint64_t scene,ID3D11Texture2D* source);
    bool RearCaptured(uint64_t scene) const { return rear_ && rearScene_==scene; }
    ID3D11Texture2D* RearImage() const { return rear_.Get(); }
    // The rear camera's view-projection rows (CameraParams +64, row-vector use).
    void SetRearCamera(const std::array<float,16>& viewProjection) { viewProjection_=viewProjection;camera_=true; }
    enum class Result { Applied,Skipped,Failed };
    // For one car reflection draw whose cube slot holds the game's cube:
    // builds the substitute once per scene id, then binds it in that slot.
    Result Apply(ID3D11DeviceContext* context,uint64_t scene,UINT cubeSlot,NativeDispatch dispatch);
    bool Restore(ID3D11DeviceContext* context);
    bool Pending() const { return pending_; }

    // Mirror glass (user feedback: bent lines, mirrors not aimed for the VR
    // seat). All three mirrors are the car's "Shiny Metal" batch, a car_basic
    // draw recognised by its material constants (MetallicSpecularValues
    // 17.45/6.48/0.08 or wet/snow 12.45/1.48/0.08, cars/settings/materials*).
    // After such a draw the same geometry is drawn again with a mirror shader
    // (depth LESS_EQUAL, no depth write) that keeps only Shiny Metal pixels in
    // the three mirror zones around the seated head, treats each mirror as a
    // flat plane aimed from the seat (left/right slightly outward, centre
    // straight back), widens the field by fovScale and shows the rear camera.
    // Everything else keeps the game's own shading. Each mirror's centre comes
    // from the previous frame's pixels (GPU feedback), so no geometry is read.
    struct OverlayShader { uint64_t hash; UINT metallicOffset,ghostOffset; };
    static constexpr std::array<OverlayShader,27> overlayShaders{{
        {0x0d2e8ab16f07aabfull,384,352}, // car_basic.cg.fp_plastic.FBF1B90E
        {0x1de47a16e81ec021ull,512,480}, // car_basic.cg.fp_light_pod.409929DD
        {0x26fecba8aea2a914ull,384,352}, // car_basic.cg.fp_carbon_fiber.4359E7BF
        {0x2b4339129a75f03aull,512,480}, // car_basic.cg.fp_carbon_fiber.409929DD
        {0x402f7420ca7fe434ull,384,352}, // car_basic.cg.fp_light_pod_cheap.F350D1ED
        {0x45e0b0fe2598602aull,384,352}, // car_basic.cg.fp_animated_light_pod.047156E8
        {0x4c26144a4da6a897ull,384,352}, // car_basic.cg.fp_plastic.6BDE810C
        {0x5036b5f4a70e7144ull,384,352}, // car_basic.cg.fp_paint.6C07276F
        {0x595323a7cd54848dull,512,480}, // car_basic.cg.fp_wheel.409929DD
        {0x67ee7fc197d98ea8ull,384,352}, // car_basic.cg.fp_plastic.4359E7BF
        {0x6e20be966255a9b5ull,384,352}, // car_basic.cg.fp_paint.FBF1B90E
        {0x781abd804b93a4b0ull,384,352}, // car_basic.cg.fp_carbon_fiber.FBF1B90E
        {0x8710ea348c27d7fbull,384,352}, // car_basic.cg.fp_carbon_fiber.6BDE810C
        {0x8882fc9009f90562ull,384,352}, // car_basic.cg.fp_paint.4359E7BF
        {0xb1841e31a7f08180ull,384,352}, // car_basic.cg.fp_carbon_fiber_wet.F4B69BE2
        {0xb2b20a8d695630a9ull,384,352}, // car_basic.cg.fp_plastic_wet.F4B69BE2
        {0xb4b3bc4b691a6570ull,384,352}, // car_basic.cg.fp_light_pod.047156E8
        {0xb598a405e2ff2502ull,384,352}, // car_basic.cg.fp_wheel.E8884D69
        {0xbb9f3f92e5284d58ull,384,352}, // car_basic.cg.fp_carbon_fiber.6C07276F
        {0xbd95611039454751ull,384,352}, // car_basic.cg.fp_paint.6BDE810C
        {0xbf584ebeb243d572ull,384,352}, // car_basic.cg.fp_basic_cheap.12907687
        {0xd075a3791f81b242ull,384,352}, // car_basic.cg.fp_paint_wet.F4B69BE2
        {0xda9f2ec75c9f8df7ull,384,352}, // car_basic.cg.fp_plastic.6C07276F
        {0xe8513731601b437eull,512,480}, // car_basic.cg.fp_paint.409929DD
        {0xf22b9b0b154da7deull,384,352}, // car_basic.cg.fp_wheel.4359E7BF
        {0xf5b2d15ea3ab03dbull,384,352}, // car_basic.cg.fp_animated_light_pod.E8884D69
        {0xfc849cd87cf4f21eull,384,352}, // car_basic.cg.fp_light_pod.E8884D69
    }};
    static const OverlayShader* FindOverlay(uint64_t hash) {
        for (const auto& shader:overlayShaders) if (shader.hash==hash) return &shader;
        return nullptr;
    }
    static constexpr float fovScale=2.f;
    // The seated cockpit camera: position, right, up, forward (world).
    void SetSeat(const std::array<float,12>& seat) { seat_=seat;seatValid_=true; }
    using Redraw=void (*)(ID3D11DeviceContext* context,void* opaque);
    enum class OverlayResult { Drawn,Skipped,Failed };
    // Call right after the game's own draw, with its bindings still in place.
    OverlayResult DrawOverlay(ID3D11DeviceContext* context,uint64_t scene,uint64_t hash,Redraw redraw,void* opaque);
    uint64_t Overlays() const { return overlays_; }
    // Diagnostic: rear-image positions instead of the image; last frame's
    // per-mirror count and bounding box (see the shader) for left, centre, right.
    void SetDebug(bool debug) { debug_=debug; }
    const std::array<float,16>& RearCamera() const { return viewProjection_; }
    bool ReadFeedback(std::array<int,96>& values);
    uint64_t Builds() const { return builds_; }
    uint64_t Applied() const { return applied_; }
private:
    bool Initialize(ID3D11DeviceContext* context);
    bool InitializeOverlay();
    bool Build(ID3D11ShaderResourceView* gameCube,ID3D11Buffer* perFrame,NativeDispatch dispatch);
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> fill_,gain_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> rear_,cube_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> rearView_,cubeView_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> cubeAccess_,statsAccess_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_,stats_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> original_;
    std::array<float,16> viewProjection_{};
    uint64_t rearScene_{},builtScene_{},builds_{},applied_{};
    UINT slot_{};
    DWORD thread_{};
    bool camera_{},pending_{},failed_{};
    // Overlay. feedback_[0] keeps each mirror's box for the cockpit stint
    // (written by the shader); feedback_[1] is the snapshot it reads.
    Microsoft::WRL::ComPtr<ID3D11PixelShader> mirrorShader_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthEqual_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> feedback_[2],overlayConstants_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> feedbackRead_[2],statsView_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> feedbackWrite_[2];
    std::array<float,12> seat_{};
    uint64_t overlayScene_{},overlays_{},lastOverlayTick_{};
    unsigned overlayRegister_{~0u};
    std::array<float,48> overlayData_{};
    bool seatValid_{},overlayReady_{},overlayFailed_{},debug_{};
};
RearMirror& Mirror();
}
