#pragma once
#include <cstdint>
#include <filesystem>

namespace grid2vr { struct SceneViews; }
namespace grid2vr::signvisibility {
// Explicit diagnostic only. Configure establishes the owner thread; subsequent
// calls never write game memory or change rendering.
void Configure(const std::filesystem::path& folder,bool enabled);
void Remember(unsigned target,uintptr_t drawable,uintptr_t properties);
void RememberViews(uint64_t frame,const SceneViews& views);
void Start();
void Stop();
void Snapshot(uint64_t frame,uint64_t scene,unsigned pass,const char* phase,uintptr_t mainObject,uintptr_t packet);
}
