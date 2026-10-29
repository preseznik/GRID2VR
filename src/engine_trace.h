#pragma once
#include <cstdint>
#include <array>
#include <filesystem>
#include "scene_views.h"

namespace grid2vr::engine {
struct CameraStamp { uint64_t id{}; uintptr_t context{}; };
struct StageStamp { uint64_t id{}; unsigned slot{}; uintptr_t object{}; };
struct SceneStamp { uint64_t id{}; bool replay{}; unsigned pass{}; };
// True only when this boundary's requested color capture was saved.
using BoundaryObserver=bool (*)(StageStamp stage,const char* phase);
enum class ViewResult { Ready, Unavailable, Failed };
enum class InstallResult { Waiting, Installed, Refused };
using ViewProvider=ViewResult (*)(uint64_t frame,SceneViews& views);
using PairObserver=void (*)(uint64_t frame,uint64_t scene,bool accepted);
using HistoryReset=bool (*)();
void ObserveFaults(uintptr_t base,const std::filesystem::path& output);
// Called only after the host fingerprint has passed.
InstallResult Install(uintptr_t base, const std::filesystem::path& output,BoundaryObserver boundary,
             ViewProvider views=nullptr,PairObserver complete=nullptr,HistoryReset reset=nullptr,HistoryReset cancel=nullptr);
bool Continuous();
bool SessionRendering();
bool SessionFailed();
bool ResumeSession();
bool SuspendSession();
// Graphics Apply may resize after visibility preparation but before any scene
// draw. Cancel only that unrendered view pair before releasing GPU history.
bool SuspendForSwapchainChange();
bool StopSession();
void FailSession();
void Frame(uint64_t nextFrame);
void CaptureUntil(uint64_t endFrame);
bool ReadyForCapture();
uint64_t NativeCameraFrame();
// Real world visibility, after startup initialization; title cameras do not qualify.
bool NativeWorldReady();
// Controller-only LOD gate for sustained native-center stereo, not the startup
// readiness marker. Requires a fresh qualified consumer/camera/completed pair.
bool OpponentLodWorldReady();
const char* CaptureWaitReason();
unsigned CaptureFrameCount();
bool RepeatingEyes();
unsigned EyePairCount();
bool ObservingEyes();
bool DualEyes();
std::string SnapshotPhase(const char* phase);
CameraStamp CurrentCamera();
StageStamp CurrentStage();
SceneStamp CurrentScene();
// The renderer's second-eye submission is never a camera simulation update.
bool ReplayingEye();
bool CurrentCockpitCamera();
// Borrow the immutable pair only during a fully posed asymmetric stereo replay.
const SceneViews* CurrentReplayViews();
// Immutable original/main-eye contexts, never an auxiliary camera or live pointer.
bool CurrentLightCameras(std::array<uint8_t,512>& center,std::array<uint8_t,512>& eye);
}
