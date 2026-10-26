#include "dlc_cockpit.h"
#include "game_executable.h"
#include "guarded_hooks.h"
#include "solo_race.h"
#include <atomic>
#include <fstream>
#include <mutex>
#include <vector>

namespace grid2vr::dlccockpit {
namespace {

uintptr_t base{};
std::filesystem::path folder;
std::mutex mutex;
Request request;
std::atomic<bool> stopped{},enabled{};
bool prepared{},requested{},failed{},finished{},nativeLaunch{};
const char* reason="not_requested";
std::array<uint64_t,25> applied{},rejected{};
struct Guard {uint32_t rva,length;const char* hash;std::vector<uint32_t> relocations;};
// Complete native consumers/parsers, including all field offsets, are pinned.
// Normalize only recorded PE HIGHLOW relocation operands for ASLR.
const std::array<Guard,30> guards{{
    {0x2f9e0,0x131,"18da29f82a8aa7b92ff7759cc87a7d9152517e5c02e8efc0ccd2a5279e51ecab",{0x8,0x27}},
    {0xfb38f,0x3a,"772c73998f620bbb076cab486a088d0b2425ddec6651829b9ce94a2f70c264c5",{0x2,0x20}},
    {0x34a4f0,0x94,"49d7d4b0e7027943dfb8947dca187f46b714cab4b4034c19fdcbfa18c6544d6c",{0x31}},
    {0x92e30,0x8e,"874e33973ed249d371fe715ba2f1ce03c85d2185a5717653f5158c923f1ef97b",{}},
    {0x476ab0,0x3cf,"ecbbce05ff9dab39c4e5f78aa4c82b5d4dfe9152f308bc269d53401ae89edd5a",{0x22,0x46,0x6c,0x90,0xb9,0xf3,0x11b,0x144,0x153,0x178,0x1a3,0x1ea,0x204,0x23a,0x341}},
    {0x242a30,0x68,"cedefc9a8a3527098e5e116dcf9e2a42b2893e5226d5b069f709ed45f0dc4aed",{0x6}},
    {0x241500,0x29,"e6b6ed9246d6b2cab4f6ecbc09bef0088995d068931a5ac17fd6f947f36b6240",{}},
    {0x706320,0x76d,"739f771c9bbcb48dd4c2f215ab937dbe5fd9d3598e2cff95807fd565a5ca405f",{0x11,0x1b,0x23,0x33,0x43,0x82,0x8e,0xba,0xef,0xf7,0xff,0x109,0x185,0x195,0x1ad,0x1c5,0x1d5,0x1e5,0x1f5,0x208,0x212,0x242,0x264,0x26e,0x276,0x286,0x2ae,0x2be,0x2ce,0x313,0x31b,0x323,0x32b,0x333,0x33b,0x343,0x356,0x360,0x3f2,0x3fc,0x414,0x524,0x52e,0x587,0x5af,0x612,0x61c,0x644,0x654,0x68d,0x6d2,0x708,0x718}},
    {0x131c10,0x6e,"4f6bad6104ec9d5458389c22a8452debfa5b146eab0700dbee7717fe57c5d2ab",{}},
    {0x3feaf0,0x4e,"2024d3d22fcb5a2c7e38579d682bd2f0d6745ed77c3746f92257bbe4e9d7b635",{0x14}},
    {0x4b3830,0x35e,"97816814c3b8871751dddbe1f65bdd6edd0909c684e93d8999a9621ab0927a86",{0x7c,0x93,0xa5,0xbb,0xc3,0xf8,0x104,0x134,0x140,0x165,0x17a,0x18f,0x1a4,0x1b9,0x1ce,0x1e3,0x1f8,0x20d,0x222,0x237,0x24c,0x261}},
    {0xd8e70,0xce,"de111736a42d697a212ef18ee47792f0ddce30cedc6fb53f1c12fd6ed875675b",{0x20,0x35,0x4a,0x5f,0x74,0x83,0x9b,0xad,0xbf}},
    {0x144a00,0x128,"ebe347ec0cc0b656075f705c1d92a2fb5e9825975d7386a26cf67ead13a50322",{0x10,0x22,0x34,0x46,0x58,0x6a,0x7c,0x8e,0xa0,0xb2,0xc4,0xd6,0xe8,0xfa,0x10c,0x11a}},
    {0x18b260,0xe0,"33c3f0cf1353315a08422341e2a536f1cc325189b8a2abee755391963d84326c",{0x10,0x22,0x34,0x46,0x58,0x6a,0x7c,0x8e,0xa0,0xb2,0xc4,0xcf}},
    {0x6a9250,0x195,"3a9265b75c3d693c020ab7099d6d272ebfec6a62633c282df168a847c888341b",{0x10,0x1c,0x24,0x3f,0x64,0x80,0x85,0xa1,0xb3,0xc5,0xd7,0xe9,0xfb,0x10a,0x11f,0x131,0x143,0x155,0x167,0x179,0x187}},
    {0x58b5a0,0x170,"574253f6ced5a968d275e095d595b61364e3eafc09423134b026bac82664768f",{0x10,0x22,0x34,0x46,0x58,0x6a,0x7c,0x8e,0xa0,0xb2,0xc4,0xd6,0xe8,0xfa,0x10c,0x11e,0x130,0x142,0x154,0x162}},
    {0x14b220,0x15e,"c5f6630d65aa3dc639803611b0e48073bb16421d44df4ad8877915e0f4b810d6",{0x10,0x22,0x34,0x46,0x58,0x6a,0x7c,0x8e,0xa0,0xb2,0xc4,0xd6,0xe8,0xfa,0x10c,0x11e,0x130,0x142,0x150}},
    {0x6ea090,0x1b7,"6074ef7bd5f503b5a8b985b2cc46c5612255d283e0740ff6dd8100655399f3fd",{0x11,0x23,0x35,0x47,0x59,0x6b,0x7d,0x8f,0xa1,0xb3,0xc5,0xd7,0xe9,0xfb,0x10d,0x11f,0x131,0x143,0x152,0x164,0x176,0x188,0x19a,0x1a8}},
    {0x66b410,0x215,"c0644ee45cc1ddb430c4b765b29aefe12dc2e92645210d2783daf088fc601253",{0x10,0x25,0x3a,0x4c,0x5e,0x70,0x82,0x97,0xa9,0xbb,0xcd,0xdf,0xf4,0x106,0x118,0x12d,0x142,0x157,0x169,0x17b,0x18d,0x19f,0x1b1,0x1c3,0x1d5,0x1e7,0x1f9,0x207}},
    {0x6443f0,0x180,"31583e659d45e0b49336fbde7782881e4aaa8cb3c6e095c572a1b4a0c755acab",{0xa,0x1f,0x2c,0x39,0x55,0x68,0x7d,0x9e,0xb0,0xc2,0xd4,0xe6,0xf5,0x107,0x119,0x12b,0x13d,0x14f,0x161,0x16f}},
    {0x5edee0,0x168,"1fe761568e740e23ba205e3f379b2568913d83da38a4eb4f4c307bed66a3a748",{0xa,0x2e,0x82,0x94,0xa6,0xb8,0xca,0xd9,0xeb,0xfd,0x10f,0x121,0x133,0x145,0x157}},
    {0x69a3a0,0x10f,"487fa3df951781d2056a81f96f9f7afc8ae55d9fd0191f87117e9bd523fcc89b",{0x4f,0x5d,0x70,0x81,0xc2,0xd2}},
    {0x605e70,0xc1,"ef0c423ce8482f2ef4ea9c6b91bd575bac852369cbb06b4404d89a623e3cc117",{0x9,0x34,0x5d,0x72}},
    {0x7e83e0,0x33,"defb028dea1817f5270a0e9ae6036c4793ef3c09eda70d48471495e402d8f708",{}},
    {0x19d90,0x4f,"37d00e01d3a0d8fce5d34fcaeec408a7154deab7ed35b4e7e10db6026346c73c",{}},
    {0x653b00,0x128,"a8a8bcd82c8720b8ffd8dd7aa8bce02e6eb4a61f5e881010c634f9e355e48665",{0x10,0x1c,0x4a,0x51,0xfc,0x10c,0x124}},
    {0x701b30,0x63,"ba82ee70510b707d251586ae6a6c34dcb871125e237c42f7a54afbef4671fc52",{0x7,0xd,0x14,0x1a,0x47,0x4d}},
    {0x741b90,0x17f,"d6aea0e9d8f7bd2505acf7d21841fc56e933d3133c4524fbc64834b940c8a1b2",{0x8,0x2e,0x58,0x5f,0x65}},
    {0x130c40,0x59,"ea14133aae08b8828e4da5d84991726c0387619d1c6ea0eceb8835d2ce19e92b",{}},
    {0x1151e0,0x46,"cb2ccadd2639f6ef46bdc6dcf4ce8ae658c378e12b3ed9125d09979b43f25bc2",{}},
}};
bool CodeMatches() {
    for(const auto& guard:guards) {
        std::vector<uint8_t> bytes(guard.length);
        if(!ReadMemory(reinterpret_cast<void*>(base+guard.rva),bytes.data(),bytes.size()))return false;
        for(auto at:guard.relocations) {
            uint32_t value{};std::memcpy(&value,bytes.data()+at,4);
            value-=static_cast<uint32_t>(base-0x400000);std::memcpy(bytes.data()+at,&value,4);
        }
        if(gameexe::Digest(bytes.data(),static_cast<ULONG>(bytes.size()))!=guard.hash)return false;
    }
    uint32_t parser{};
    return ReadMemory(reinterpret_cast<void*>(base+mountedTableRva+0x30),&parser,4) && parser==base+0x4b3830;
}
template<class T> bool Read(uintptr_t address,T& value) {
    return ReadMemory(reinterpret_cast<void*>(address),&value,sizeof(value));
}
bool Writable(uintptr_t address,size_t bytes) {
    if(!address || bytes>UINTPTR_MAX-address)return false;
    const auto end=address+bytes;
    while(address<end) {
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
           info.Type!=MEM_PRIVATE || info.Protect!=PAGE_READWRITE)return false;
        const auto next=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
        if(next<=address)return false;address=next;
    }
    return true;
}
struct Graph {
    uintptr_t parent{},car{},owner{},manager{},begin{},end{};
    std::array<uint32_t,2> views{};
    size_t row{};
    bool operator==(const Graph&)const=default;
};
Graph lastRejected{};
bool hasRejected{};
uint64_t appliedPairs{},rejectedPairs{},alreadyPatchedPairs{};
void WriteStatus() {
    if(folder.empty())return;
    std::ofstream out(folder/L"dlc-cockpit.json",std::ios::trunc);
    out<<std::boolalpha<<"{\"schema\":2,\"strategy\":\"loaded_camera_data\",\"requested\":"<<requested
       <<",\"enabled\":"<<enabled.load()<<",\"nativeCodePatches\":0,\"failed\":"<<failed
       <<",\"stopped\":"<<stopped.load()<<",\"reason\":\""<<reason
       <<"\",\"archiveWrites\":0,\"saveWrites\":0,\"headsetValidated\":false,\"appliedPairs\":"<<appliedPairs
       <<",\"rejectedPairs\":"<<rejectedPairs<<",\"alreadyPatchedPairs\":"<<alreadyPatchedPairs<<",\"cars\":[";
    for(size_t i=0;i<request.count;++i) {
        auto index=request.rows[i].car;if(i)out<<',';
        out<<"{\"car\":\""<<cars[index].code<<"\",\"patchedPairs\":"<<applied[index]<<",\"rejectedPairs\":"<<rejected[index]<<'}';
    }
    out<<"]}\n";
}
void Status(const char* value) {
    if(std::string_view(reason)!=value) {reason=value;WriteStatus();}
}
bool Pointer(uintptr_t at,uintptr_t& value) {
    return Read(at,value) && value>=0x10000 && value<=UINTPTR_MAX-0x10000 && value%4==0;
}
bool Word(uintptr_t at,uintptr_t expected) {uint32_t value{};return Read(at,value) && value==expected;}
bool ReadyParent(uintptr_t& parent) {
    if(NormalLocalRaceReady(base) || NormalSoloRaceReady(base))return Pointer(base+0xd96808,parent);
    if(nativeLaunch)return false;
    // Same bounded solo-playback readiness used by diagnostics. Association to
    // the local car and its provider is independently checked below.
    uintptr_t playback{},begin{},end{},car{};
    return Pointer(base+0xe61010,playback) && Pointer(playback+0xa8,parent) && Word(parent,base+0xd1266c) &&
        Pointer(parent+0xa8,begin) && Pointer(parent+0xac,end) && end-begin==4 && Pointer(begin,car);
}
// 530c40 assigns the named loader manager directly to car+148; 5151e0
// binds every view provider to car+12c. These are live views, not a clone.
bool Resolve(Graph& graph) {
    if(!ReadyParent(graph.parent))return false;
    uintptr_t carsBegin{};
    if(!Pointer(graph.parent+0xa8,carsBegin) || !Pointer(carsBegin,graph.car) ||
       !Word(graph.car,base+0xcdfeb4) || !Word(graph.car+0xb0,0) ||
       !Pointer(graph.car+0x148,graph.manager) || !Pointer(base+0xdef814,graph.owner) ||
       !Word(graph.owner,base+0xcdd504) || !Word(graph.owner+0xc,base+0xcdd4f8))return false;
    int32_t count{};if(!Read(graph.owner+0xa64,count) || count<1 || count>64)return false;
    unsigned matches{};std::array<char,4> code{};
    for(int32_t i=0;i<count;++i) {
        std::array<uint8_t,20> entry{};uint32_t manager{};
        if(!Read(graph.owner+0x564+static_cast<uintptr_t>(i)*20,entry))return false;
        std::memcpy(&manager,entry.data()+12,4);
        if(manager==graph.manager) {std::memcpy(code.data(),entry.data(),4);++matches;}
    }
    if(matches!=1 || code[3]!=0)return false;
    graph.row=request.count;
    for(size_t i=0;i<request.count;++i)if(!std::memcmp(code.data(),cars[request.rows[i].car].code,3))graph.row=i;
    if(graph.row==request.count)return false;
    // Reject ambiguous names even if another entry points at a different manager.
    matches=0;
    for(int32_t i=0;i<count;++i) {
        std::array<char,4> name{};if(!Read(graph.owner+0x564+static_cast<uintptr_t>(i)*20,name))return false;
        if(name==code)++matches;
    }
    if(matches!=1 || !Pointer(graph.manager+0x5f0,graph.begin) || !Pointer(graph.manager+0x5f4,graph.end) ||
       graph.end<=graph.begin || (graph.end-graph.begin)%4 || graph.end-graph.begin>64*4)return false;
    std::array<unsigned,2> found{};
    for(auto address=graph.begin;address<graph.end;address+=4) {
        uintptr_t view{};std::array<char,32> name{};
        if(!Pointer(address,view) || !Read(view+0x19,name))return false;
        for(size_t i=0;i<2;++i) {
            const std::string_view wanted=i?"bonnet_reverse":"bonnet";
            if(!std::memcmp(name.data(),wanted.data(),wanted.size()) && !name[wanted.size()]) {
                graph.views[i]=static_cast<uint32_t>(view);++found[i];
            }
        }
    }
    if(found[0]!=1 || found[1]!=1 ||
       (graph.views[0]>graph.views[1]?graph.views[0]-graph.views[1]:graph.views[1]-graph.views[0])<objectBytes)return false;
    for(auto view:graph.views)
        if(!Word(view,base+mountedTableRva) || !Word(view+0x60,graph.car+0x12c) || !Writable(view,objectBytes))return false;
    return true;
}
bool Store(uintptr_t address,const void* value,size_t length) {
    SIZE_T written{};
    return WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),value,length,&written) && written==length;
}
void Arm() {
    if(!CodeMatches()) {failed=true;reason="instruction_guard";WriteStatus();return;}
    enabled=true;reason="waiting_for_world";WriteStatus();
}
}
void Prepare(uintptr_t gameBase,const std::filesystem::path& output) {
    std::lock_guard lock(mutex);if(prepared)return;prepared=true;base=gameBase;folder=output;
    std::error_code error;const auto path=output/L"dlc-cockpit.request";
    if(!std::filesystem::exists(path,error))return;
    requested=true;std::array<char,4097> bytes{};std::ifstream in(path,std::ios::binary);in.read(bytes.data(),bytes.size());
    if(!ParseRequest(std::string_view(bytes.data(),static_cast<size_t>(in.gcount())),request) ||
       !std::filesystem::is_regular_file(output/L"xr-session.request",error) ||
       !std::filesystem::is_regular_file(output/L"xr-projection.request",error)) {
        failed=true;reason="invalid_or_non_vr_request";WriteStatus();return;
    }
    nativeLaunch=std::filesystem::is_regular_file(output/L"race-launch.request",error) ||
        std::filesystem::is_regular_file(output/L"normal-launch.request",error);
    wchar_t exe[32768]{};const auto length=GetModuleFileNameW(nullptr,exe,32768);
    if(!base || length==0 || length>=32768 || gameexe::Validate(exe,true).empty()) {
        failed=true;reason="executable_guard";WriteStatus();return;
    }
    // Startup remains entirely read-only. In particular, do not detour the
    // loader: native relocated-image checks continue after D3D device creation.
    Arm();
}
void Present() {
    if(!enabled || stopped)return;
    std::lock_guard lock(mutex);if(stopped)return;
    uintptr_t parent{};
    if(!ReadyParent(parent)) {hasRejected=false;Status("waiting_for_world");return;}
    Graph graph{};
    if(!Resolve(graph)) {Status("waiting_for_verified_camera_graph");return;}
    const auto& row=request.rows[graph.row];
    std::array<std::array<uint8_t,objectBytes>,2> snapshots{};
    if(!Read(graph.views[0],snapshots[0]) || !Read(graph.views[1],snapshots[1])) {
        Status("camera_snapshot_unreadable");return;
    }
    if(PatchedPair(snapshots[0],snapshots[1],static_cast<uint32_t>(base+mountedTableRva),row,request.hideDriver)) {
        if(reason!=std::string_view("applied_loaded_camera_data") && reason!=std::string_view("already_patched_loaded_camera_data")) {
            ++alreadyPatchedPairs;Status("already_patched_loaded_camera_data");
        }
        return;
    }
    if(!PatchPair(snapshots[0],snapshots[1],static_cast<uint32_t>(base+mountedTableRva),row,request.hideDriver)) {
        if(!hasRejected || !(lastRejected==graph)) {++rejected[row.car];++rejectedPairs;lastRejected=graph;hasRejected=true;}
        Status("camera_original_values_rejected");return;
    }
    // Re-resolve the complete ownership graph immediately before writes. Never
    // retain raw object pointers across loading, restart or car transitions.
    Graph current{};if(!Resolve(current) || !(current==graph)) {Status("camera_graph_changed");return;}
    bool written=true;
    for(size_t i=0;i<2 && written;++i) {
        const auto address=static_cast<uintptr_t>(graph.views[i]);
        for(size_t field=0;field<18 && written;++field)
            written=Store(address+floatOffsets[field],snapshots[i].data()+floatOffsets[field],4);
        if(written)written=Store(address+0xf4,snapshots[i].data()+0xf4,4);
        for(size_t field=0;field<flagOffsets.size() && written;++field)
            if(cars[row.car].original[i].flags[field]>=0)
                written=Store(address+flagOffsets[field],snapshots[i].data()+flagOffsets[field],1);
    }
    if(!written) {failed=true;enabled=false;reason="camera_data_write_failed";WriteStatus();return;}
    ++applied[row.car];++appliedPairs;hasRejected=false;reason="applied_loaded_camera_data";WriteStatus();
}
void Stop() {
    stopped=true;enabled=false;std::lock_guard lock(mutex);if(!requested || finished)return;finished=true;
    // Objects belong to the game and may already have been released. Stop
    // observing; never dereference old identities during process teardown.
    WriteStatus();
}
#ifdef GRID2VR_DLC_COCKPIT_TEST
bool TestCodeMatches(uintptr_t image) {base=image;return CodeMatches();}
void TestSetup(uintptr_t image,const Request& settings,bool normal) {
    base=image;request=settings;nativeLaunch=normal;stopped=false;enabled=false;failed=false;finished=false;requested=true;folder.clear();
    applied={};rejected={};appliedPairs=0;rejectedPairs=0;alreadyPatchedPairs=0;hasRejected=false;Arm();
}
std::array<uint64_t,2> TestCounts(size_t car) {return {applied[car],rejected[car]};}
#endif
}
