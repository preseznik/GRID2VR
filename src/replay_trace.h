#pragma once
#include <d3d11.h>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include "vpl_order.h"

namespace grid2vr::replaytrace {
void Configure(const std::filesystem::path& folder,bool enabled,NativeDispatch dispatch=nullptr);
bool Active();
bool Detailed();
bool Compact();
bool ScreenUiEnabled();
bool FirstPair();
bool PairHealthy();
bool LastPair();
// Refuses a pending pair, temporary binding or prior restoration failure.
bool ResetHistory();
// Close a center-only native menu frame; never discard a started eye replay.
bool CancelBeforeReplay();
void Upload(ID3D11Resource* resource,const void* data,UINT bytes);
void ShaderIdentity(ID3D11DeviceChild* shader,uint64_t hash);
uint64_t ShaderFingerprint(ID3D11DeviceChild* shader);
void AfterDraw(ID3D11DeviceContext* context);
// Only during the guarded shadow draw, before AfterDraw restores native state.
bool ShadowControlBindings(ID3D11DeviceContext* context,bool native);
void SceneBoundary(ID3D11DeviceContext* context,const char* phase);
void BeforeCompute(ID3D11DeviceContext* context,const std::string& mark);
bool VplOrderRequested(const std::string& mark);
void AfterCompute(ID3D11DeviceContext* context,const std::string& mark,UINT x,UINT y,UINT z,NativeDispatch dispatch);
// Borrowed, first-scene-only resource for passive restoration readback.
ID3D11Texture2D* ComputeScratchResource();
// Records mapped constant-buffer evidence, not complete D3D pipeline state.
std::string Work(ID3D11DeviceContext* context,const char* kind,std::span<const UINT> args);
}
