#pragma once
#include <filesystem>

namespace grid2vr {
// Explicit experimental path only. Recorded-pose and live-XR modes must never mix.
inline bool HeadsetVisibilityCandidate(const std::filesystem::path& folder) {
    std::error_code error;
    for(const auto* name:{L"headset-visibility-candidate.request",L"runtime-session.request",
        L"tracked-visibility.request",L"expand-visibility.request",L"xr-session.request",L"xr-projection.request",
        L"instance-capacity-observe.request",L"instance-capacity-expanded.request",L"flicker-diagnostic.request",
        L"flicker-compact.request",L"flicker-visibility-only.request"})
        if(!std::filesystem::is_regular_file(folder/name,error) || error)return false;
    for(const auto* name:{L"recorded-views.request",L"flicker-unattended.request",
        L"native-visibility-control.request",L"flicker-sign-target.request",L"flicker-observation.request"})
        if(std::filesystem::exists(folder/name,error) || error)return false;
    return true;
}
}
