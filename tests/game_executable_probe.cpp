#include "game_executable.h"
#include <cstdio>
#include <cwchar>

// Offline validator only. Never loads or starts the supplied executable.
int wmain(int argc,wchar_t** argv) {
    if(argc==3 && !std::wcscmp(argv[1],L"identity")) {
        std::string identity;
        for(const auto* part=argv[2];*part;++part) {
            if(*part>127)return 1;
            identity+=static_cast<char>(*part);
        }
        return grid2vr::gameexe::IsStandalone(identity)?0:1;
    }
    if(argc!=3||(std::wcscmp(argv[1],L"avx")&&std::wcscmp(argv[1],L"regular")))return 2;
    const auto hash=grid2vr::gameexe::Validate(argv[2],!std::wcscmp(argv[1],L"avx"));
    if(hash.empty())return 1;
    std::puts(hash.c_str());return 0;
}
