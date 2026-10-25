#include "headset_visibility_candidate.h"
#include <fstream>
#include <cstdio>

#define CHECK(x) do {if(!(x)){std::printf("line %d: %s\n",__LINE__,#x);return 1;}} while(false)
int main(int argc,char** argv) {
    CHECK(argc==2);
    const auto folder=std::filesystem::absolute(argv[1]);
    CHECK(!std::filesystem::exists(folder));std::filesystem::create_directories(folder);
    const wchar_t* required[]{L"headset-visibility-candidate.request",L"runtime-session.request",
        L"tracked-visibility.request",L"expand-visibility.request",L"xr-session.request",L"xr-projection.request",
        L"instance-capacity-observe.request",L"instance-capacity-expanded.request",L"flicker-diagnostic.request",
        L"flicker-compact.request",L"flicker-visibility-only.request"};
    CHECK(!grid2vr::HeadsetVisibilityCandidate(folder));
    for(auto* name:required)std::ofstream(folder/name)<<"fixture";
    CHECK(grid2vr::HeadsetVisibilityCandidate(folder));
    for(auto* name:required) {
        std::filesystem::remove(folder/name);CHECK(!grid2vr::HeadsetVisibilityCandidate(folder));
        std::filesystem::create_directory(folder/name);CHECK(!grid2vr::HeadsetVisibilityCandidate(folder));
        std::filesystem::remove(folder/name);std::ofstream(folder/name)<<"fixture";
    }
    for(auto* name:{L"recorded-views.request",L"flicker-unattended.request",L"native-visibility-control.request",L"flicker-sign-target.request",L"flicker-observation.request"}) {
        std::ofstream(folder/name)<<"fixture";CHECK(!grid2vr::HeadsetVisibilityCandidate(folder));
        std::filesystem::remove(folder/name);
    }
    CHECK(grid2vr::HeadsetVisibilityCandidate(folder));
    for(auto* name:required)std::filesystem::remove(folder/name);
    std::filesystem::remove(folder);return 0;
}
