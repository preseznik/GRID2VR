#pragma once
#include <cstdint>
namespace grid2vr {
class ReplaySequence {
public:
    enum class State { Rendering, Suspended, Stopped, Failed };
    bool Configure(unsigned count,bool compactStereo=false) {
        if (attempts_ || continuous_ || state_!=State::Rendering || !count || count>(compactStereo?900u:120u)) return false;
        limit_=count; return true;
    }
    // Session lifetime is explicit, never a very large diagnostic frame count.
    // It starts suspended; the owner must reset CPU/GPU histories before Resume.
    bool ConfigureSession() {
        if (attempts_ || continuous_ || state_!=State::Rendering) return false;
        continuous_=true; limit_=0; state_=State::Suspended; return true;
    }
    bool Available() const { return state_==State::Rendering && !pending_ && (continuous_ || attempts_<limit_); }
    // GPU controls must finish restoring an in-flight pair, then stop on failure.
    bool Observing() const { return pending_ || Available(); }
    bool Begin(uint64_t frame) {
        if (!Available()) return false;
        if (!frame || frame<=lastFrame_ || (epochCompleted_ && frame!=lastFrame_+1)) { state_=State::Failed; return false; }
        lastFrame_=frame; ++attempts_; pending_=true; return true;
    }
    void Finish(bool success) {
        if (!pending_ || !success) state_=State::Failed;
        if (pending_ && success && state_==State::Rendering) { ++completed_; ++epochCompleted_; }
        pending_=false;
    }
    // Abandon a continuous pair refused before any eye or game write. Not a
    // success: the owner must then suspend and start a fresh epoch. It is not an
    // attempted pair; its frame remains the monotonic floor.
    bool Cancel() {
        if (!continuous_ || !pending_ || state_!=State::Rendering || !attempts_) return false;
        pending_=false; --attempts_; return true;
    }
    // Refuse suspension/stop mid-pair: the owner must finish restoration first.
    bool Suspend() {
        if (!continuous_ || pending_ || state_!=State::Rendering) return false;
        state_=State::Suspended; return true;
    }
    bool Resume() {
        if (!continuous_ || pending_ || state_!=State::Suspended) return false;
        ++epoch_; epochCompleted_=0; state_=State::Rendering; return true;
    }
    bool Stop() {
        if (!continuous_ || pending_ || state_==State::Failed) return false;
        state_=State::Stopped; return true;
    }
    unsigned Limit() const { return limit_; }
    uint64_t Attempts() const { return attempts_; }
    uint64_t Completed() const { return completed_; }
    uint64_t EpochCompleted() const { return epochCompleted_; }
    uint64_t Epoch() const { return epoch_; }
    bool Continuous() const { return continuous_; }
    uint64_t LastFrame() const { return lastFrame_; }
    void Fail() { state_=State::Failed; }
    State Status() const { return state_; }
private:
    unsigned limit_{1};
    uint64_t attempts_{},completed_{},epochCompleted_{},epoch_{},lastFrame_{};
    bool pending_{},continuous_{};
    State state_{State::Rendering};
};
}
