#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace grid2vr::perf {
// Fixed-size histogram: memory stays bounded however long a session runs.
// Percentiles are reported at bin resolution, as the bin's upper edge.
template<unsigned Bins,unsigned PerUnit> class Histogram {
public:
    void Add(double value) {
        if (!std::isfinite(value) || value<0) return;
        const auto bin=std::min<double>(value*PerUnit,Bins-1);
        ++bins_[static_cast<unsigned>(bin)];++count_;sum_+=value;max_=std::max(max_,value);
    }
    uint64_t Count() const {return count_;}
    double Mean() const {return count_?sum_/double(count_):-1;}
    double Max() const {return count_?max_:-1;}
    double Percentile(double p) const {
        if (!count_) return -1;
        const auto target=std::max<uint64_t>(1,static_cast<uint64_t>(std::ceil(p*double(count_))));
        uint64_t seen{};
        for (unsigned i=0;i+1<Bins;++i) {seen+=bins_[i];if (seen>=target) return std::min(double(i+1)/PerUnit,max_);}
        return max_;
    }
private:
    std::array<uint32_t,Bins> bins_{};
    uint64_t count_{};
    double sum_{},max_{};
};
using Milliseconds=Histogram<4000,20>;   // 0.05 ms bins to 200 ms
using FramesPerSecond=Histogram<2500,10>; // 0.1 FPS bins to 250 FPS
using Percent=Histogram<101,1>;

enum class Limit {Insufficient,AtTarget,Gpu,Cpu,Mixed};
inline const char* LimitName(Limit limit) {
    switch (limit) {
    case Limit::AtTarget:return "at_target";case Limit::Gpu:return "gpu";
    case Limit::Cpu:return "cpu";case Limit::Mixed:return "mixed";
    default:return "insufficient";
    }
}
// An estimate from medians, not a proof. GPU-bound frames keep the adapter
// busy; CPU-bound frames leave the render thread no time blocked on the GPU,
// XR runtime or Present.
inline Limit Classify(double targetFps,double fpsMedian,uint64_t windows,double gpuMedian,uint64_t gpuSamples,double blockedMedian) {
    if (windows<10 || !(targetFps>0) || fpsMedian<0) return Limit::Insufficient;
    if (fpsMedian>=targetFps*.97) return Limit::AtTarget;
    const bool gpuKnown=gpuSamples>=5 && gpuMedian>=0;
    if (gpuKnown && gpuMedian>=88) return Limit::Gpu;
    if ((!gpuKnown || gpuMedian<80) && blockedMedian>=0 && blockedMedian<1) return Limit::Cpu;
    return Limit::Mixed;
}
// One slow second, judged from that second alone. gpuPercent<0 means unknown.
inline Limit ClassifySlow(double gpuPercent,double blockedMean) {
    if (gpuPercent>=88) return Limit::Gpu;
    if (gpuPercent<80 && blockedMean>=0 && blockedMean<1) return Limit::Cpu;
    return Limit::Mixed;
}

enum GpuSegment : unsigned {PreScene,Center,Left,Right,PostScene,WholeFrame,GpuSegments};

// Stereo session performance in submission order. Frames count only while the
// headset receives a stereo projection, so menus and the flat screen are excluded.
class SessionPerformance {
public:
    void Wait(double milliseconds,double periodMilliseconds) {
        if (std::isfinite(milliseconds) && milliseconds>=0) waitMs_+=milliseconds;
        if (std::isfinite(periodMilliseconds) && periodMilliseconds>0) periodMs_=periodMilliseconds;
    }
    void Submitted(bool stereo) {if (stereo) frameStereo_=true;else frameScreen_=true;}
    bool FrameStereo() const {return frameStereo_;}
    void CpuViews(double center,double left,double right) {
        cpuView_[0].Add(center);cpuView_[1].Add(left);cpuView_[2].Add(right);
    }
    // Present returned at nowMs. gpuPercent is the latest one-second sample (-1 if none).
    void Boundary(double nowMs,double presentMs,double gpuPercent) {
        const bool stereo=frameStereo_ && !frameScreen_;
        if (stereo) ++stereoFrames_;
        if (frameScreen_) ++screenFrames_;
        if (stereo && sessionStart_<0) sessionStart_=nowMs;
        if (stereo && previousStereo_ && lastBoundary_>=0 && nowMs>lastBoundary_) {
            const double interval=nowMs-lastBoundary_,blocked=waitMs_+presentMs,busy=std::max(0.,interval-blocked);
            frame_.Add(interval);wait_.Add(waitMs_);present_.Add(presentMs);blocked_.Add(blocked);cpu_.Add(busy);
            windowBusy_+=busy;windowBlocked_+=blocked;++windowIntervals_;
        }
        // One-second windows of uninterrupted stereo; a menu or screen frame
        // discards the partial window instead of mixing phases.
        if (!stereo) windowStart_=-1;
        else if (windowStart_<0) StartWindow(nowMs);
        else {
            ++windowFrames_;
            if (nowMs-windowStart_>=1000) {
                const double fps=double(windowFrames_)*1000./(nowMs-windowStart_);
                const double blocked=windowIntervals_?windowBlocked_/double(windowIntervals_):-1;
                const double busy=windowIntervals_?windowBusy_/double(windowIntervals_):-1;
                fps_.Add(fps);
                if (gpuPercent>=0) gpu_.Add(gpuPercent);
                // A median can hold the headset rate while some seconds drop.
                // Each slow second keeps its own GPU or CPU evidence.
                if (periodMs_>0 && fps<TargetFps()*DipThreshold) ++slow_[static_cast<unsigned>(ClassifySlow(gpuPercent,blocked))];
                AddTimeline(nowMs,fps,busy,gpuPercent);
                StartWindow(nowMs);
            }
        }
        previousStereo_=stereo;lastBoundary_=nowMs;waitMs_=0;frameStereo_=frameScreen_=false;
    }
    void GpuFrame(const std::array<double,GpuSegments>& segments) {
        ++gpuFrames_;for (unsigned i=0;i<GpuSegments;++i) gpuSegment_[i].Add(segments[i]);
    }
    void GpuDropped() {++gpuDropped_;}
    void GpuDisjoint() {++gpuDisjoint_;}
    double TargetFps() const {return periodMs_>0?1000./periodMs_:-1;}
    Limit Estimate() const {
        return Classify(TargetFps(),fps_.Percentile(.5),fps_.Count(),gpu_.Percentile(.5),gpu_.Count(),blocked_.Percentile(.5));
    }
    static constexpr double DipThreshold=.95,DipShare=.05;
    uint64_t SlowWindows() const {return slow_[size_t(Limit::Gpu)]+slow_[size_t(Limit::Cpu)]+slow_[size_t(Limit::Mixed)];}
    // What limited the slow seconds: none below 5% of windows, otherwise a
    // majority class, else mixed.
    const char* DipLimit() const {
        const auto slow=SlowWindows();
        if (fps_.Count()<10) return "insufficient";
        if (!slow || double(slow)<DipShare*double(fps_.Count())) return "none";
        if (slow_[size_t(Limit::Gpu)]*2>slow) return "gpu";
        if (slow_[size_t(Limit::Cpu)]*2>slow) return "cpu";
        return "mixed";
    }
    unsigned TimelineBuckets() const {return buckets_;}
    const FramesPerSecond& Fps() const {return fps_;}
    const Percent& GpuPercent() const {return gpu_;}
    uint64_t StereoFrames() const {return stereoFrames_;}
    uint64_t GpuFrames() const {return gpuFrames_;}

    struct Identity {
        std::string adapter;unsigned vendorId{},deviceId{};uint64_t dedicatedMb{};
        unsigned headsetWidth{},headsetHeight{},scale{};
    } identity;

    std::string Json(bool final) const {
        std::string out="{\"schema\":1,\"final\":";out+=final?"true":"false";
        out+=",\"adapter\":{\"name\":\""+Escape(identity.adapter)+"\"";
        out+=",\"vendorId\":"+std::to_string(identity.vendorId)+",\"deviceId\":"+std::to_string(identity.deviceId);
        out+=",\"dedicatedMb\":"+std::to_string(identity.dedicatedMb)+"}";
        out+=",\"headset\":{\"width\":"+std::to_string(identity.headsetWidth)+",\"height\":"+std::to_string(identity.headsetHeight);
        out+=",\"scale\":"+std::to_string(identity.scale)+"}";
        out+=",\"displayPeriodMs\":"+Number(periodMs_)+",\"targetFps\":"+Number(TargetFps());
        out+=",\"stereoFrames\":"+std::to_string(stereoFrames_)+",\"screenFrames\":"+std::to_string(screenFrames_);
        out+=",\"fps\":{\"windows\":"+std::to_string(fps_.Count())+",\"p5\":"+Number(fps_.Percentile(.05));
        out+=",\"p50\":"+Number(fps_.Percentile(.5))+",\"p95\":"+Number(fps_.Percentile(.95))+",\"mean\":"+Number(fps_.Mean())+"}";
        out+=",\"frameMs\":"+Stats(frame_)+",\"cpuBusyMs\":"+Stats(cpu_)+",\"blockedMs\":"+Stats(blocked_);
        out+=",\"waitMs\":"+Stats(wait_)+",\"presentMs\":"+Stats(present_);
        out+=",\"cpuViewMs\":{\"center\":"+Stats(cpuView_[0])+",\"left\":"+Stats(cpuView_[1])+",\"right\":"+Stats(cpuView_[2])+"}";
        out+=",\"gpuMs\":{\"frames\":"+std::to_string(gpuFrames_)+",\"dropped\":"+std::to_string(gpuDropped_);
        out+=",\"disjoint\":"+std::to_string(gpuDisjoint_);
        static constexpr const char* names[GpuSegments]{"preScene","center","left","right","postScene","frame"};
        for (unsigned i=0;i<GpuSegments;++i) out+=",\""+std::string(names[i])+"\":"+Stats(gpuSegment_[i]);
        out+="},\"gpuPercent\":{\"samples\":"+std::to_string(gpu_.Count())+",\"p50\":"+Number(gpu_.Percentile(.5));
        out+=",\"p95\":"+Number(gpu_.Percentile(.95))+"}";
        out+=",\"limit\":\"";out+=LimitName(Estimate());
        const auto slow=SlowWindows();
        out+="\",\"dips\":{\"threshold\":"+Number(DipThreshold)+",\"slowWindows\":"+std::to_string(slow);
        out+=",\"share\":"+(fps_.Count()?Number(double(slow)/double(fps_.Count())):std::string("null"));
        out+=",\"gpu\":"+std::to_string(slow_[size_t(Limit::Gpu)])+",\"cpu\":"+std::to_string(slow_[size_t(Limit::Cpu)]);
        out+=",\"mixed\":"+std::to_string(slow_[size_t(Limit::Mixed)])+",\"limit\":\""+DipLimit()+"\"}";
        out+=",\"timeline\":{\"bucketSeconds\":"+std::to_string(BucketMs/1000)+",\"truncated\":"+(timelineTruncated_?"true":"false");
        out+=",\"columns\":[\"t\",\"windows\",\"fps\",\"low\",\"cpuMs\",\"gpuPercent\"],\"buckets\":[";
        for (unsigned i=0;i<buckets_;++i) {
            const auto& b=timeline_[i];
            out+=(i?",[":"[")+std::to_string(b.index*BucketMs/1000)+","+std::to_string(b.windows)+","+Number(b.fps/b.windows)+","+Number(b.low);
            out+=","+(b.cpuWindows?Number(b.cpu/b.cpuWindows):std::string("null"))+","+(b.gpuSamples?Number(b.gpu/b.gpuSamples):std::string("null"))+"]";
        }
        out+="]},\"limits\":\"Limit is an estimate from medians of stereo frames only; dips classify each second below 95% of the headset rate. GPU segments are timestamp intervals and include idle time when the CPU is the limit. GPU percent is the adapter-wide busiest engine, including other processes.\"}\n";
        return out;
    }
private:
    static constexpr unsigned TimelineCapacity=360,BucketMs=10000; // one hour of 10 s buckets
    struct Bucket {
        uint32_t index{},windows{},cpuWindows{},gpuSamples{};
        double fps{},low{},cpu{},gpu{};
    };
    void StartWindow(double nowMs) {windowStart_=nowMs;windowFrames_=0;windowBusy_=windowBlocked_=0;windowIntervals_=0;}
    // Keeps the first hour, so a race start is never displaced by later driving.
    void AddTimeline(double nowMs,double fps,double busy,double gpu) {
        const auto index=static_cast<uint32_t>((nowMs-sessionStart_)/BucketMs);
        if (!buckets_ || timeline_[buckets_-1].index!=index) {
            if (buckets_==TimelineCapacity) {timelineTruncated_=true;return;}
            timeline_[buckets_++]=Bucket{index,0,0,0,0,fps};
        }
        auto& b=timeline_[buckets_-1];
        ++b.windows;b.fps+=fps;b.low=std::min(b.low,fps);
        if (busy>=0) {b.cpu+=busy;++b.cpuWindows;}
        if (gpu>=0) {b.gpu+=gpu;++b.gpuSamples;}
    }
    static std::string Number(double value) {
        if (!std::isfinite(value) || value<0) return "null";
        char text[32]{};snprintf(text,sizeof(text),"%.2f",value);return text;
    }
    template<class H> static std::string Stats(const H& h) {
        return "{\"count\":"+std::to_string(h.Count())+",\"p50\":"+Number(h.Percentile(.5))+",\"p95\":"+Number(h.Percentile(.95))+
            ",\"p99\":"+Number(h.Percentile(.99))+",\"max\":"+Number(h.Max())+"}";
    }
    static std::string Escape(const std::string& text) {
        std::string out;
        for (const char c:text) {
            if (c=='"' || c=='\\') {out+='\\';out+=c;}
            else if (static_cast<unsigned char>(c)<0x20) out+=' ';
            else out+=c;
        }
        return out;
    }
    Milliseconds frame_,cpu_,blocked_,wait_,present_;
    std::array<Milliseconds,3> cpuView_;
    std::array<Milliseconds,GpuSegments> gpuSegment_;
    FramesPerSecond fps_;
    Percent gpu_;
    uint64_t stereoFrames_{},screenFrames_{},gpuFrames_{},gpuDropped_{},gpuDisjoint_{},windowFrames_{},windowIntervals_{};
    std::array<uint64_t,5> slow_{}; // indexed by Limit
    std::array<Bucket,TimelineCapacity> timeline_{};
    unsigned buckets_{};
    bool timelineTruncated_{};
    double periodMs_{-1},waitMs_{},lastBoundary_{-1},windowStart_{-1},sessionStart_{-1},windowBusy_{},windowBlocked_{};
    bool frameStereo_{},frameScreen_{},previousStereo_{};
};
}
