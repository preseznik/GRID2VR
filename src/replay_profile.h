#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace grid2vr::profile {
// CPU wall time only. Nested buckets are inclusive and must not be summed.
// Driver calls include any CPU-side waiting; this does not infer GPU time.
enum Bucket { DrawHook,DrawDriver,DispatchHook,DispatchDriver,Replay,
    Snapshot,EngineLog,GraphicsLog,GraphicsBoundary,MemoryRead,Count };
inline constexpr const char* names[]{"draw_hook","draw_driver","dispatch_hook","dispatch_driver","replay",
    "snapshot","engine_log","graphics_log","graphics_boundary","memory_read"};
struct Counters { std::array<LONGLONG,Count> ticks{};std::array<uint64_t,Count> calls{}; };
inline thread_local bool active{};
inline thread_local Counters counters;
inline thread_local uint64_t currentFrame{},endFrame{};
inline thread_local LONGLONG started{},frequency{};
inline thread_local std::ofstream csv;
inline LONGLONG Now() { LARGE_INTEGER value{};QueryPerformanceCounter(&value);return value.QuadPart; }
class Scope {
public:
    explicit Scope(Bucket bucket):bucket_(bucket),enabled_(active),begin_(enabled_?Now():0) {}
    ~Scope() { if (enabled_) { counters.ticks[bucket_]+=Now()-begin_;++counters.calls[bucket_]; } }
private:
    Bucket bucket_;bool enabled_;LONGLONG begin_;
};
inline void NextFrame(uint64_t nextFrame) {
    if (!active) return;
    const auto now=Now();const double factor=1000.0/frequency;
    csv<<currentFrame<<','<<GetCurrentThreadId()<<','<<(now-started)*factor;
    for (unsigned i=0;i<Count;++i) csv<<','<<counters.ticks[i]*factor<<','<<counters.calls[i];
    csv<<'\n';csv.flush();
    counters={};currentFrame=nextFrame;started=Now();active=nextFrame<endFrame;
}
inline void Start(const std::filesystem::path& output,uint64_t first,uint64_t end) {
    if (active || csv.is_open() || !first || first>=end || !std::filesystem::exists(output/L"profile-replay.request")) return;
    LARGE_INTEGER value{};QueryPerformanceFrequency(&value);frequency=value.QuadPart;
    csv.open(output/L"replay-profile.csv");csv<<"frame,thread,wall_ms";
    for (const auto* name:names) csv<<','<<name<<"_ms,"<<name<<"_calls";
    csv<<'\n';csv.flush();counters={};currentFrame=first;endFrame=end;started=Now();active=csv.good() && frequency>0;
}
}
