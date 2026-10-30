#include "frame_timing.h"
#include "performance_display.h"
#include "performance_summary.h"
#include <dxgi.h>
#include <wrl/client.h>
#include <array>
#include <fstream>
#include <memory>
#include <string>
using Microsoft::WRL::ComPtr;

namespace grid2vr::frametiming {
namespace {
constexpr unsigned ring=8,complete=(1u<<PointCount)-1;
constexpr ULONGLONG writeInterval=5000;
struct Slot {
    ComPtr<ID3D11Query> disjoint;
    std::array<ComPtr<ID3D11Query>,PointCount> stamps;
    unsigned mask{};
    bool open{},pending{},stereo{};
};
struct State {
    DWORD thread{};
    ComPtr<ID3D11DeviceContext> context;
    std::array<Slot,ring> slots;
    unsigned current{};
    std::array<LONGLONG,PointCount> cpu{};
    unsigned cpuMask{};
    LONGLONG frequency{},waitStarted{},waitEnded{};
    perf::SessionPerformance summary;
    GpuUsageSampler gpu;
    std::filesystem::path output;
    ULONGLONG nextWrite{};
};
// Deliberately leaked if Stop never runs: D3D objects must not be released
// from DllMain or a global destructor.
State* state{};
bool Owner() {return state && state->thread==GetCurrentThreadId();}
double Milliseconds(LONGLONG ticks) {return 1000.*double(ticks)/double(state->frequency);}
void Write(bool final) {
    const auto text=state->summary.Json(final);
    const auto target=state->output/L"performance-summary.json",temporary=state->output/L"performance-summary.json.tmp";
    {
        std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
        file<<text;if (!file) return;
    }
    MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING);
}
// Oldest first. Never waits: an unfinished query stays pending until its ring
// slot is needed again, then counts as dropped.
void Poll() {
    for (unsigned age=1;age<=ring;++age) {
        auto& slot=state->slots[(state->current+age)%ring];
        if (!slot.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        if (state->context->GetData(slot.disjoint.Get(),&disjoint,sizeof(disjoint),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK) return;
        slot.pending=false;
        if (disjoint.Disjoint || !disjoint.Frequency) {state->summary.GpuDisjoint();continue;}
        if (slot.mask!=complete || !slot.stereo) continue;
        std::array<UINT64,PointCount> t{};
        bool ready=true;
        for (unsigned i=0;i<PointCount && ready;++i)
            ready=state->context->GetData(slot.stamps[i].Get(),&t[i],sizeof(t[i]),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK;
        for (unsigned i=1;i<PointCount && ready;++i) ready=t[i]>=t[i-1];
        if (!ready) {state->summary.GpuDropped();continue;}
        const auto ms=[&](unsigned from,unsigned to) {return 1000.*double(t[to]-t[from])/double(disjoint.Frequency);};
        state->summary.GpuFrame({ms(FrameBegin,CenterBegin),ms(CenterBegin,CenterEnd),ms(CenterEnd,RightBegin),
            ms(RightBegin,RightEnd),ms(RightEnd,FrameEnd),ms(FrameBegin,FrameEnd)});
    }
}
void Open(LONGLONG now) {
    state->current=(state->current+1)%ring;
    auto& slot=state->slots[state->current];
    if (slot.pending) {slot.pending=false;state->summary.GpuDropped();}
    state->context->Begin(slot.disjoint.Get());
    state->context->End(slot.stamps[FrameBegin].Get());
    slot.mask=1u<<FrameBegin;slot.open=true;slot.stereo=false;
    state->cpu={};state->cpu[FrameBegin]=now;state->cpuMask=1u<<FrameBegin;
    state->waitStarted=state->waitEnded=0;
}
}
LONGLONG Now() {LARGE_INTEGER value{};QueryPerformanceCounter(&value);return value.QuadPart;}
bool Start(ID3D11Device* device,const std::filesystem::path& output,unsigned headsetWidth,unsigned headsetHeight,unsigned scale) {
    if (state || !device) return false;
    auto created=std::make_unique<State>();
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart<=0) return false;
    created->frequency=frequency.QuadPart;
    const D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT,0},stamp{D3D11_QUERY_TIMESTAMP,0};
    for (auto& slot:created->slots) {
        if (FAILED(device->CreateQuery(&disjoint,&slot.disjoint))) return false;
        for (auto& query:slot.stamps) if (FAILED(device->CreateQuery(&stamp,&query))) return false;
    }
    auto& identity=created->summary.identity;
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc))) {
        char name[256]{};
        if (WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name,sizeof(name),nullptr,nullptr)) identity.adapter=name;
        identity.vendorId=desc.VendorId;identity.deviceId=desc.DeviceId;identity.dedicatedMb=desc.DedicatedVideoMemory/(1024*1024);
    }
    identity.headsetWidth=headsetWidth;identity.headsetHeight=headsetHeight;identity.scale=scale;
    created->thread=GetCurrentThreadId();created->output=output;
    device->GetImmediateContext(&created->context);
    created->gpu.Start(device);
    state=created.release();
    Open(Now());Write(false);state->nextWrite=GetTickCount64()+writeInterval;
    return true;
}
void Mark(Point point) {
    if (!Owner() || point==FrameBegin || point>=FrameEnd) return;
    const auto bit=1u<<point;
    if (state->cpuMask&bit) return;
    state->cpu[point]=Now();state->cpuMask|=bit;
    auto& slot=state->slots[state->current];
    if (slot.open && !(slot.mask&bit)) {state->context->End(slot.stamps[point].Get());slot.mask|=bit;}
}
void Wait(LONGLONG started,LONGLONG periodNanoseconds) {
    if (!Owner()) return;
    const auto now=Now();
    state->waitStarted=started;state->waitEnded=now;
    state->summary.Wait(Milliseconds(now-started),double(periodNanoseconds)/1e6);
}
void Submitted(bool stereo) {if (Owner()) state->summary.Submitted(stereo);}
void Present(LONGLONG started) {
    if (!Owner()) return;
    const auto now=Now();
    const bool stereo=state->summary.FrameStereo();
    auto& slot=state->slots[state->current];
    if (slot.open) {
        state->context->End(slot.stamps[FrameEnd].Get());state->context->End(slot.disjoint.Get());
        slot.mask|=1u<<FrameEnd;slot.open=false;slot.pending=true;slot.stereo=stereo;
    }
    constexpr unsigned views=(1u<<CenterBegin)|(1u<<CenterEnd)|(1u<<RightBegin)|(1u<<RightEnd);
    if (stereo && (state->cpuMask&views)==views) {
        auto center=state->cpu[CenterEnd]-state->cpu[CenterBegin];
        // xrWaitFrame can run inside the center's visibility preparation;
        // time spent waiting for the headset is not center render work.
        if (state->waitStarted>=state->cpu[CenterBegin] && state->waitEnded<=state->cpu[CenterEnd])
            center-=state->waitEnded-state->waitStarted;
        state->summary.CpuViews(Milliseconds(center),Milliseconds(state->cpu[RightBegin]-state->cpu[CenterEnd]),
            Milliseconds(state->cpu[RightEnd]-state->cpu[RightBegin]));
    }
    state->summary.Boundary(Milliseconds(now),Milliseconds(now-started),state->gpu.Percent());
    Poll();Open(now);
    const auto tick=GetTickCount64();
    if (tick>=state->nextWrite) {Write(false);state->nextWrite=tick+writeInterval;}
}
void Stop() {
    if (!Owner()) return;
    auto& slot=state->slots[state->current];
    if (slot.open) {state->context->End(slot.disjoint.Get());slot.open=false;}
    Write(true);
    state->gpu.Stop();
    delete state;state=nullptr;
}
}
