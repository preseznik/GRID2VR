#include "sign_visibility.h"
#include "scene_views.h"
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace sv=grid2vr::signvisibility;
namespace {
void Require(bool value,const char* why) {if(!value)throw std::runtime_error(why);}
template<size_t N> void Put(std::array<uint8_t,N>& bytes,size_t offset,uint32_t value) {
    Require(offset+4<=N,"fixture offset");memcpy(bytes.data()+offset,&value,4);
}
template<class T> uintptr_t Address(T& value) {return reinterpret_cast<uintptr_t>(value.data());}
std::string Read(const std::filesystem::path& folder) {
    std::ifstream in(folder/L"sign-visibility.jsonl",std::ios::binary);
    return {std::istreambuf_iterator<char>(in),{}};
}
size_t Count(const std::string& value,const std::string& match) {
    size_t count{},at{};
    while((at=value.find(match,at))!=std::string::npos) {++count;at+=match.size();}
    return count;
}
void Contains(const std::string& value,const std::string& match) {Require(value.find(match)!=std::string::npos,match.c_str());}
struct Fixture {
    std::array<uint8_t,256> drawable{},properties{},transform{},packet{};
    std::array<uint8_t,0x710> main{};
    std::array<uint8_t,0x2a20> builder{};
    std::array<uint8_t,256> manager{};
    std::array<uint8_t,0x18> allocator{};
    std::array<std::array<uint8_t,16>,17> lanes{};
    std::array<std::array<uint8_t,8>,3> trees{};
    std::array<std::array<uint8_t,64>,7> nodes{};
    Fixture() {
        Put(drawable,0,0x12345678);Put(drawable,0x2c,static_cast<uint32_t>(Address(transform)));
        Put(drawable,0x30,static_cast<uint32_t>(Address(properties)));Put(drawable,0x44,7);
        Put(main,0xe4,static_cast<uint32_t>(Address(builder)));
        for(unsigned i=0;i<3;++i) {Put(builder,0x34+4*i,static_cast<uint32_t>(Address(trees[i])));Put(trees[i],4,101+i);}
        builder[0x2a18]=1;builder[0x2a1c]=2;Put(builder,0x320,0x13572468);
        Put(main,0xd4,static_cast<uint32_t>(Address(manager)));Put(manager,0xf8,static_cast<uint32_t>(Address(allocator)));
        allocator[0]=1;Put(allocator,0xc,40);Put(allocator,0x10,static_cast<uint32_t>(Address(lanes)));Put(allocator,0x14,2);
        for(unsigned i=0;i<lanes.size();++i) {
            // Deliberately invalid resource/mapped addresses must never be followed.
            Put(lanes[i],0,1);Put(lanes[i],4,UINT32_MAX);Put(lanes[i],8,100+i);Put(lanes[i],12,10+i);
        }
        Put(main,0x188,17);Put(main,0x18c,1);Put(main,0x190,32);
        Put(main,0x230,29);Put(main,0x234,UINT32_MAX);Put(main,0x238,64);
        constexpr std::array<size_t,7> offsets{0x80,0x84,0x88,0xe4,0xe0,0x8c,0x10};
        for(unsigned i=0;i<offsets.size();++i) {
            Put(packet,offsets[i],static_cast<uint32_t>(Address(nodes[i])));
            Put(nodes[i],0xc,static_cast<uint32_t>(Address(drawable)));
        }
    }
    void Remember() {sv::Remember(0,Address(drawable),Address(properties));}
    void Snapshot(uint64_t frame=1,const char* phase="native_complete") {sv::Snapshot(frame,frame*10,0,phase,Address(main),Address(packet));}
};
}
int main(int argc,char** argv) {
    static_assert(sizeof(uintptr_t)==4,"game witness is Win32");
    const auto base=argc>1?std::filesystem::path(argv[1]):std::filesystem::temp_directory_path()/L"grid2vr-sign-visibility-test";
    const auto root=base/(L"case-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    try {
        Require(std::filesystem::create_directories(root),"unique fixture folder");
        const auto folder=[&](const wchar_t* name) {auto p=root/name;std::filesystem::create_directory(p);return p;};
        Fixture f;
        const auto valid=folder(L"valid");sv::Configure(valid,true);f.Remember();sv::Start();
        const auto before=f;f.Snapshot();
        Require(memcmp(&before,&f,sizeof(f))==0,"witness modified fixture");
        Put(f.drawable,0x44,9);f.properties[3]=42;f.Snapshot(2);
        for(uint64_t frame=3;frame<=8;++frame)f.Snapshot(frame);
        sv::Stop();
        const auto good=Read(valid);
        Require(Count(good,"\"type\":\"snapshot\"")==8,"eight snapshots");
        Contains(good,"\"reason\":\"complete\"");Contains(good,"\"selectionFlags\":7");
        Contains(good,"\"selectionFlags\":9");Contains(good,"\"matchingNodes\":7");
        Contains(good,"\"lazyStateComplete\":true");Contains(good,"\"rootKnown\":true,\"root\":103");
        Contains(good,"\"offset\":10776,\"bytes\":1,\"known\":true,\"value\":1");
        Contains(good,"\"offset\":10780,\"bytes\":1,\"known\":true,\"value\":2");
        Contains(good,"\"offset\":800,\"bytes\":4,\"known\":true,\"value\":324478056");
        Contains(good,"\"enabled\":1,\"stride\":40");
        Contains(good,"\"resource\":1,\"mappedBase\":4294967295,\"capacity\":100,\"used\":10");
        Contains(good,"\"observedLanes\":2,\"laneLimit\":16,\"laneCountStatus\":\"ok\",\"laneRecordsTruncated\":false,\"lanesComplete\":true");
        Contains(good,"\"offset\":392,\"status\":\"ok\",\"count\":17,\"records\":1,\"capacity\":32");
        Contains(good,"\"offset\":560,\"status\":\"ok\",\"count\":29,\"records\":4294967295,\"capacity\":64");
        for(unsigned offset:{128u,132u,136u,228u,224u,140u,16u})Contains(good,"\"rootOffset\":"+std::to_string(offset));
        sv::Start();sv::Stop();Require(Read(valid)==good,"second start overwrote finished witness");

        const auto poseFolder=folder(L"frozen-views");sv::Configure(poseFolder,true);sv::Start();
        grid2vr::SceneViews pair{};pair.frusta={grid2vr::diagnosticFrustum,grid2vr::diagnosticFrustum};
        pair.poses[0].position[0]=-.032f;pair.poses[1].position[0]=.032f;
        sv::RememberViews(1,pair);f.Snapshot(1,"visibility-before");
        f.Snapshot(2,"visibility-before"); // Stale pair must not be attached.
        auto invalidPair=pair;invalidPair.poses[1].orientation[0]=NAN;
        sv::RememberViews(3,invalidPair);f.Snapshot(3,"visibility-before");
        std::thread foreign([&] {sv::RememberViews(4,pair);});foreign.join();
        f.Snapshot(4,"visibility-before");sv::Stop();
        const auto poseLog=Read(poseFolder);
        Require(Count(poseLog,"\"views\":")==1,"stale/invalid/foreign frozen pose leaked");
        Contains(poseLog,"\"views\":{\"frame\":1,\"eyes\":[{\"eye\":0,\"qx\":0");
        Contains(poseLog,"{\"eye\":1,\"qx\":0");

        const auto reuse=folder(L"reuse");sv::Configure(reuse,true);f.Remember();sv::Start();
        Put(f.drawable,0x2c,1);f.Remember();f.Snapshot();sv::Stop();
        const auto reused=Read(reuse);
        Contains(reused,"\"status\":\"identity_reused\"");Contains(reused,"\"selectionFlags\":null");
        Contains(reused,"\"transformRaw\":{\"base\":1,\"offset\":0,\"bytes\":256,\"status\":\"identity_reused\"}");

        const auto invalid=folder(L"invalid");sv::Configure(invalid,true);sv::Remember(0,1,1);sv::Start();
        sv::Snapshot(1,1,0,"native",1,1);sv::Snapshot(1,1,0,"bad\"json",0,0);
        sv::Snapshot(1,1,0,reinterpret_cast<const char*>(1),0,0);sv::Stop();
        const auto bad=Read(invalid);Contains(bad,"\"status\":\"identity_unknown\"");
        Contains(bad,"\"rejectedPhases\":2");Contains(bad,"\"lazyStateComplete\":false");
        Contains(bad,"\"rootKnown\":false,\"root\":null");
        Contains(bad,"\"laneCount\":null,\"lanes\":[]");

        Fixture laneFixture;
        const auto laneBounds=folder(L"lane-bounds");sv::Configure(laneBounds,true);sv::Start();
        Put(laneFixture.allocator,0x14,0x7fffffff);
        const auto laneBefore=laneFixture;laneFixture.Snapshot();
        Require(memcmp(&laneBefore,&laneFixture,sizeof(laneFixture))==0,"allocator witness modified fixture");
        Put(laneFixture.allocator,0x14,UINT32_MAX);laneFixture.Snapshot();
        Put(laneFixture.allocator,0x14,2);Put(laneFixture.allocator,0x10,1);laneFixture.Snapshot();
        Put(laneFixture.manager,0xf8,1);laneFixture.Snapshot();
        Put(laneFixture.main,0xd4,1);laneFixture.Snapshot();sv::Stop();
        const auto laneLog=Read(laneBounds);
        Contains(laneLog,"\"observedLanes\":16,\"laneLimit\":16,\"laneCountStatus\":\"bounded\",\"laneRecordsTruncated\":true,\"lanesComplete\":false");
        Require(laneLog.find("\"index\":16")==std::string::npos,"allocator lane cap");
        Contains(laneLog,"\"laneCount\":-1,\"lanes\":[]");
        Contains(laneLog,"\"laneCountStatus\":\"invalid\"");
        Contains(laneLog,"\"index\":0,\"status\":\"unreadable\",\"resource\":null,\"mappedBase\":null,\"capacity\":null,\"used\":null");
        Contains(laneLog,"\"address\":1,\"status\":\"unreadable\",\"rawBytes\":24");
        Contains(laneLog,"\"manager\":1,\"addressKnown\":false,\"address\":null,\"status\":\"unknown\"");

        Fixture cycle;
        Put(cycle.nodes[0],0,static_cast<uint32_t>(Address(cycle.nodes[0])));
        Put(cycle.builder,0x38,1);
        const auto partial=folder(L"partial");sv::Configure(partial,true);cycle.Remember();sv::Start();cycle.Snapshot();sv::Stop();
        Contains(Read(partial),"\"status\":\"partial\",\"complete\":false");
        Contains(Read(partial),"\"lazyStateComplete\":false");

        const auto bounds=folder(L"bounds");sv::Configure(bounds,true);
        for(unsigned i=0;i<9;++i)sv::Remember(i,0,0);
        sv::Start();for(unsigned i=0;i<257;++i)sv::Snapshot(1,1,0,"native",0,0);sv::Stop();
        const auto capped=Read(bounds);Require(Count(capped,"\"type\":\"snapshot\"")==256,"snapshot cap");
        Require(Count(capped,"\"type\":\"target\"")==8,"target cap");
        Contains(capped,"\"reason\":\"snapshot_limit\"");Contains(capped,"\"targetLimitReached\":true");

        const auto frame=folder(L"frames");sv::Configure(frame,true);sv::Start();
        for(unsigned i=1;i<=9;++i)sv::Snapshot(i,i,0,"native",0,0);sv::Stop();
        Require(Count(Read(frame),"\"type\":\"snapshot\"")==8,"frame cap");Contains(Read(frame),"\"reason\":\"frame_limit\"");

        const auto thread=folder(L"thread");sv::Configure(thread,true);sv::Start();
        std::thread other([]{sv::Remember(0,0,0);sv::Snapshot(1,1,0,"native",0,0);sv::Stop();});other.join();
        sv::Snapshot(1,1,0,"native",0,0);sv::Stop();
        Require(Count(Read(thread),"\"type\":\"snapshot\"")==1,"owner thread guard");
        Require(Count(Read(thread),"\"type\":\"target\"")==0,"owner remember guard");

        Fixture many;
        std::vector<std::array<uint8_t,64>> nodes(4097);
        for(size_t i=0;i<nodes.size();++i) {
            Put(nodes[i],0,i+1<nodes.size()?static_cast<uint32_t>(Address(nodes[i+1])):0);
            Put(nodes[i],0xc,static_cast<uint32_t>(Address(many.drawable)));
        }
        many.packet={};Put(many.packet,0x84,static_cast<uint32_t>(Address(nodes[0])));
        const auto budget=folder(L"budget");sv::Configure(budget,true);
        for(unsigned i=0;i<8;++i)sv::Remember(i,Address(many.drawable),Address(many.properties));
        sv::Start();for(unsigned i=0;i<256;++i)many.Snapshot();sv::Stop();
        const auto large=Read(budget);Contains(large,"\"observedNodes\":4096");
        Contains(large,"\"matchingDetailsTruncated\":true");Contains(large,"\"reason\":\"log_budget\"");
        Require(large.size()<=4*1024*1024,"log byte cap");Contains(large,"\"truncated\":true");

        const auto disabled=folder(L"disabled");sv::Configure(disabled,false);f.Remember();sv::Start();f.Snapshot();sv::Stop();
        Require(!std::filesystem::exists(disabled/L"sign-visibility.jsonl"),"disabled output");
        sv::Configure(root,false);
        // root was created uniquely above, under the explicit fixture output.
        std::filesystem::remove_all(root);
        puts("Sign visibility: immutable guarded reads, identity reuse, seven roots, builder gates, thread and storage bounds passed.");
        return 0;
    } catch(const std::exception& e) {
        sv::Stop();fprintf(stderr,"Sign visibility test: %s (fixtures: %s)\n",e.what(),root.string().c_str());return 1;
    }
}
