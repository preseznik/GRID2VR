#include "frame_timing.h"
#include "performance_summary.h"
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
using Microsoft::WRL::ComPtr;
using grid2vr::perf::Limit;
namespace {
// Drive the summary as the render thread does: wait, submit, Present boundary.
std::unique_ptr<grid2vr::perf::SessionPerformance> Run(double seconds,double intervalMs,double waitMs,double presentMs,double gpu,
                                                     unsigned screenEvery=0) {
    auto summary=std::make_unique<grid2vr::perf::SessionPerformance>();
    double now=1000;
    for (unsigned frame=1;now<1000+seconds*1000;++frame) {
        summary->Wait(waitMs,1000./90);
        summary->Submitted(!(screenEvery && frame%screenEvery==0));
        now+=intervalMs;summary->Boundary(now,presentMs,gpu);
    }
    return summary;
}
// Phases by time: frame interval, xrWaitFrame time and the GPU sample.
struct Phase {double intervalMs,waitMs,gpu;};
template<class F> std::unique_ptr<grid2vr::perf::SessionPerformance> Drive(double seconds,F phase) {
    auto summary=std::make_unique<grid2vr::perf::SessionPerformance>();
    for (double now=1000;now<1000+seconds*1000;) {
        const Phase p=phase((now-1000)/1000);
        summary->Wait(p.waitMs,1000./90);summary->Submitted(true);
        now+=p.intervalMs;summary->Boundary(now,.1,p.gpu);
    }
    return summary;
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);std::stringstream text;text<<file.rdbuf();return text.str();
}
long long Field(const std::string& text,const std::string& prefix) {
    const auto at=text.find(prefix);
    return at==std::string::npos?-1:std::stoll(text.substr(at+prefix.size()));
}
}
int main(int argc,char** argv) {
    if (argc!=2) return 1;
    grid2vr::perf::Milliseconds ms;
    for (unsigned i=1;i<=100;++i) ms.Add(i);
    if (std::abs(ms.Percentile(.5)-50.05)>.001 || ms.Percentile(.99)>99.06 || ms.Max()!=100 || ms.Count()!=100) return 2;
    ms.Add(-1);ms.Add(NAN);ms.Add(1e9);
    if (ms.Count()!=101 || ms.Percentile(1)!=1e9 || grid2vr::perf::Milliseconds{}.Percentile(.5)!=-1) return 3;

    // 80 FPS with the adapter saturated: GPU-limited below a 90 Hz target.
    const auto gpu=Run(30,12.5,.2,.1,97);
    if (gpu->Estimate()!=Limit::Gpu || std::abs(gpu->Fps().Percentile(.5)-80)>.2 || gpu->Fps().Count()<28 ||
        std::abs(gpu->TargetFps()-90)>.01 || gpu->GpuPercent().Count()!=gpu->Fps().Count()) return 4;
    // Same rate with spare GPU and no blocking: the render thread is the limit.
    if (Run(30,12.5,.2,.1,60)->Estimate()!=Limit::Cpu || Run(30,12.5,.2,.1,-1)->Estimate()!=Limit::Cpu) return 5;
    // Spare GPU but the thread waits several ms: neither proof holds.
    if (Run(30,12.5,4,.1,70)->Estimate()!=Limit::Mixed) return 6;
    if (Run(30,1000./90,3,.1,70)->Estimate()!=Limit::AtTarget || Run(5,12.5,.2,.1,97)->Estimate()!=Limit::Insufficient) return 7;
    // A screen frame every half second never completes a window or a stereo interval.
    const auto menus=Run(30,12.5,.2,.1,97,40);
    if (menus->Fps().Count()!=0 || menus->Estimate()!=Limit::Insufficient || menus->StereoFrames()==0) return 8;

    // Chicago with 7 AI: the median holds 90, but the start and every tenth
    // second drop to 80 with the render thread never blocked and the GPU at 60%.
    const auto cpuDips=Drive(120,[](double t) {
        return t<20 || int(t)%10==5 ? Phase{12.5,.05,60} : Phase{1000./90,3,60};
    });
    if (cpuDips->Estimate()!=Limit::AtTarget || std::string(cpuDips->DipLimit())!="cpu" ||
        cpuDips->SlowWindows()<25 || cpuDips->SlowWindows()>40) return 20;
    const auto cpuJson=cpuDips->Json(false);
    if (cpuJson.find("\"dips\":{\"threshold\":0.95")==std::string::npos || cpuJson.find("\"limit\":\"cpu\"}")==std::string::npos ||
        cpuJson.find("\"bucketSeconds\":10,\"truncated\":false")==std::string::npos || cpuJson.find("\"buckets\":[[0,")==std::string::npos) return 21;
    // Bucket [t, windows, fps, low, ...]: the slow start is visible at 10-20 s.
    const auto bucket=[](const std::string& json,const char* prefix,unsigned& windows,double& fps,double& low) {
        const auto at=json.find(prefix,json.find("\"buckets\":"));
        return at!=std::string::npos && sscanf_s(json.c_str()+at+strlen(prefix),"%u,%lf,%lf",&windows,&fps,&low)==3;
    };
    unsigned windows{};double fps{},low{};
    if (cpuDips->TimelineBuckets()!=12 || !bucket(cpuJson,"[10,",windows,fps,low) || windows!=10 || fps!=80 || low!=80) return 22;
    // GPU-saturated dips, few dips, and a session longer than the timeline.
    const auto gpuDips=Drive(120,[](double t) {return int(t)%10<2 ? Phase{12.5,3,97} : Phase{1000./90,3,60};});
    if (std::string(gpuDips->DipLimit())!="gpu") {std::fputs(gpuDips->Json(false).c_str(),stderr);return 23;}
    const auto few=Drive(120,[](double t) {return int(t)==60 ? Phase{12.5,.05,60} : Phase{1000./90,3,60};});
    if (std::string(few->DipLimit())!="none" || !bucket(few->Json(false),"[30,",windows,fps,low) || std::abs(fps-90)>.01 || std::abs(low-90)>.01) return 24;
    const auto hour=Drive(3700,[](double) {return Phase{1000./90,3,60};});
    if (hour->TimelineBuckets()!=360 || hour->Json(true).find("\"truncated\":true")==std::string::npos) return 25;

    gpu->identity.adapter="Quoted \"GPU\"\\";gpu->identity.headsetWidth=3072;gpu->identity.scale=70;
    const auto json=gpu->Json(true);
    if (json.find("\"limit\":\"gpu\"")==std::string::npos || json.find("Quoted \\\"GPU\\\"\\\\")==std::string::npos ||
        json.find("\"final\":true")==std::string::npos || json.find("\"targetFps\":90.00")==std::string::npos ||
        json.find("\"headset\":{\"width\":3072")==std::string::npos || json.find("\"gpuMs\":{\"frames\":0")==std::string::npos) return 9;

    const auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    if (!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))) return 10;
    const auto path=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(path);
    std::filesystem::remove(path/L"performance-summary.json");
    namespace timing=grid2vr::frametiming;
    timing::Mark(timing::CenterBegin);timing::Present(timing::Now());timing::Stop();
    if (std::filesystem::exists(path/L"performance-summary.json")) return 11;
    if (!timing::Start(device.Get(),path,3072,3264,70) || timing::Start(device.Get(),path,1,1,100)) return 12;
    for (unsigned frame=0;frame<60;++frame) {
        timing::Wait(timing::Now(),11111111);
        timing::Mark(timing::CenterBegin);context->ClearState();timing::Mark(timing::CenterEnd);
        timing::Mark(timing::RightBegin);timing::Mark(timing::RightEnd);
        timing::Submitted(true);
        context->Flush();timing::Present(timing::Now());Sleep(2);
    }
    // Calls from any other thread are ignored, including Stop.
    std::thread foreign([] {timing::Mark(timing::CenterBegin);timing::Submitted(false);timing::Stop();});foreign.join();
    timing::Stop();timing::Stop();
    const auto receipt=Read(path/L"performance-summary.json");
    if (receipt.find("\"final\":true")==std::string::npos || receipt.find("\"headset\":{\"width\":3072,\"height\":3264,\"scale\":70}")==std::string::npos ||
        Field(receipt,"\"stereoFrames\":")!=60 || Field(receipt,"\"gpuMs\":{\"frames\":")<=0 ||
        receipt.find("\"cpuViewMs\":{\"center\":{\"count\":60")==std::string::npos || std::filesystem::exists(path/L"performance-summary.json.tmp")) {
        std::fputs(receipt.c_str(),stderr);return 13;
    }
    puts("VR performance summary: bounded statistics, limit estimate, receipt, owner thread and non-blocking GPU timestamps passed.");
    return 0;
}
