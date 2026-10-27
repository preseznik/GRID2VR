#include "game_executable.h"
#include "chase_camera_guard.h"
#include "chase_camera_input_guard.h"
#include "chase_camera_state.h"
#include "chase_camera_region_guard.h"
#include "chase_camera_playback_state.h"
#include "chase_camera_transaction.h"
#include "chase_camera_collision.h"
#include "chase_camera_replay.h"
#include "chase_observer_guard.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

// Map a verified file as inert data, including PE relocations. Never load its
// imports, start the game, execute a native routine or change the installed file.
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 1;
    const auto hash=grid2vr::gameexe::Validate(argv[1],true);
    if(hash.empty()) {std::cerr<<"Executable fingerprint refused\n";return 2;}
    std::ifstream file(std::filesystem::path(argv[1]),std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
    const auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    const auto pe=reinterpret_cast<const IMAGE_NT_HEADERS32*>(bytes.data()+dos->e_lfanew);
    auto* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,pe->OptionalHeader.SizeOfImage,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!image)return 3;
    std::memcpy(image,bytes.data(),pe->OptionalHeader.SizeOfHeaders);
    const auto sections=IMAGE_FIRST_SECTION(pe);
    for(unsigned i=0;i<pe->FileHeader.NumberOfSections;++i)
        std::memcpy(image+sections[i].VirtualAddress,bytes.data()+sections[i].PointerToRawData,sections[i].SizeOfRawData);
    const auto base=reinterpret_cast<uintptr_t>(image);
    const auto delta=static_cast<uint32_t>(base-pe->OptionalHeader.ImageBase);
    const auto reloc=pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    for(size_t offset=0;offset<reloc.Size;) {
        const auto block=reinterpret_cast<IMAGE_BASE_RELOCATION*>(image+reloc.VirtualAddress+offset);
        if(block->SizeOfBlock<sizeof(*block) || block->SizeOfBlock>reloc.Size-offset)return 4;
        const auto entries=reinterpret_cast<uint16_t*>(block+1);
        for(size_t i=0;i<(block->SizeOfBlock-sizeof(*block))/2;++i) {
            const auto type=entries[i]>>12;
            if(type==IMAGE_REL_BASED_HIGHLOW) {
                auto* word=reinterpret_cast<uint32_t*>(image+block->VirtualAddress+(entries[i]&0xfff));*word+=delta;
            } else if(type!=IMAGE_REL_BASED_ABSOLUTE)return 5;
        }
        offset+=block->SizeOfBlock;
    }
    using namespace grid2vr::chasecamera;
    const auto read=[](uintptr_t at,void* out,size_t count) {return grid2vr::ReadMemory(reinterpret_cast<void*>(at),out,count);};
    const bool valid=grid2vr::chaseobserver::GuardsMatch(base) && AdapterGuardsMatch(base) && LookGuardsMatch(base) && PauseGuardsMatch(base) &&
        RegionGuardsMatch(base) && ReplayStateGuardsMatch(base,read) && TransactionGuardsMatch(base,read) && CollisionGuardsMatch(base) && RecordedReplayGuardsMatch(base);
    unsigned rejected{};
    if(valid) {
        for(const auto& guard:PlaybackClockGuards(base)) {
            image[guard.rva]^=1;rejected+=!RecordedReplayGuardsMatch(base);image[guard.rva]^=1;
        }
        for(const auto& guard:ReplayOwnerInputGuards(base)) {
            image[guard.rva]^=1;rejected+=!RecordedReplayGuardsMatch(base);image[guard.rva]^=1;
        }
        for(const auto& guard:ReplayDirectorThreeGuards(base)) {
            image[guard.rva]^=1;rejected+=!RecordedReplayGuardsMatch(base);image[guard.rva]^=1;
        }
        for(const auto& guard:ReplayShuttleGuards(base)) {
            image[guard.rva]^=1;rejected+=!RecordedReplayGuardsMatch(base);image[guard.rva]^=1;
        }
        for(const auto& guard:ReplayMenuScopeGuards(base)) {
            image[guard.rva]^=1;rejected+=!RecordedReplayGuardsMatch(base);image[guard.rva]^=1;
        }
        for(const auto& guard:collisionGuards) {
            image[guard.rva]^=1;rejected+=!CollisionGuardsMatch(base);image[guard.rva]^=1;
        }
        for(const auto& guard:LookGuards(base)) {
            image[guard.rva]^=1;rejected+=!LookGuardsMatch(base);image[guard.rva]^=1;
        }
        for(const auto& guard:PauseGuards(base)) {
            image[guard.rva]^=1;rejected+=!PauseGuardsMatch(base);image[guard.rva]^=1;
        }
        for(const auto& guard:RegionGuards()) {
            image[guard.rva]^=1;rejected+=!RegionGuardsMatch(base);image[guard.rva]^=1;
        }
        image[0xd09998]^=1;rejected+=!RegionGuardsMatch(base);image[0xd09998]^=1;
        for(const auto& guard:ReplayStateGuards(base)) {
            image[guard.rva]^=1;rejected+=!ReplayStateGuardsMatch(base,read);image[guard.rva]^=1;
        }
        for(const auto rva:{0x70ec10,0x5afbe0,0x410c60,0x1ea430,0xd0996c,0xd09970,0xd099a8,0xd099ac}) {
            image[rva]^=1;rejected+=!TransactionGuardsMatch(base,read);image[rva]^=1;
        }
    }
    VirtualFree(image,0,MEM_RELEASE);
    std::cout<<"{\"sha256\":\""<<hash<<"\",\"allGuardsMatched\":"<<(valid?"true":"false")
        <<",\"mutatedGuardSitesRefused\":"<<rejected<<",\"gameExecuted\":false}\n";
    return valid && rejected==LookGuards(0x400000).size()+PauseGuards(0x400000).size()+RegionGuards().size()+1+ReplayStateGuards(0x400000).size()+8+collisionGuards.size()+PlaybackClockGuards(0x400000).size()+ReplayOwnerInputGuards(0x400000).size()+ReplayDirectorThreeGuards(0x400000).size()+ReplayShuttleGuards(0x400000).size()+ReplayMenuScopeGuards(0x400000).size()?0:6;
}
