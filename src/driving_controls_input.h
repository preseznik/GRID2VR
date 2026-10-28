#pragma once
#include "driving_controls_model.h"
#include <memory>

namespace grid2vr::drivingcontrols {
struct BindingSample {
    bool supported{},connected{};State state{};
    const char* status="legacy_family";
    int32_t error{}; // DirectInput HRESULT or XInput result, unchanged.
    float raw{},calibrated{};
    const char* driverFiltering="not_applicable";
};

// Single-owner input-update thread. Never injects input or changes game maps.
// Poll once per native input update, then use Samples for every map/action.
class BindingInput {
public:
    BindingInput();
    ~BindingInput();
    BindingInput(const BindingInput&)=delete;
    BindingInput& operator=(const BindingInput&)=delete;
    void Configure(const std::vector<Binding>& bindings);
    void Poll();
    // Call on the normal stop path, outside DllMain, under the poller's lock.
    void Release();
    const std::vector<BindingSample>& Samples() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
