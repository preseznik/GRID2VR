#include "chase_observer.h"
#include "chase_observer_guard.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <span>
#include <vector>

using namespace grid2vr::chaseobserver;
namespace {
void* expectedCamera{};void* expectedArgument{};
uint32_t expectedFlag{};unsigned forwarded[7]{};bool argumentsValid=true;
void* expectedProvider{};void* expectedProviderOutput{};
std::array<uint32_t,4> providerPosition{0x3f800000,0x40000000,0x40400000,0x3f800000};
uintptr_t providerReturnOffset{};
uintptr_t exportInterleaveCamera{};
bool providerBlocked{};
enum class ProviderAction { Normal,WrongCaller,HelperCaller,ForeignThread,NestedStage,Duplicate,InterleavedUpdate };
ProviderAction providerAction{};
thread_local unsigned stageDepth{};
constexpr uintptr_t stageResult=0x17395bdf;
constexpr uint32_t beforeProviderWord=0x41200000,afterProviderWord=0x41a00000;
HANDLE entered{},release{},finished{};
bool completedUnderLock{};
uintptr_t __fastcall Update(void* self,void*,void* argument) {
    argumentsValid&=self==expectedCamera && argument==expectedArgument;++forwarded[0];
    ++reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(self)+0xb0)[3];return 0x12345678;
}
uintptr_t __fastcall Alternate(void* self,void*,void* argument,uint32_t flag) {
    argumentsValid&=self==expectedCamera && argument==expectedArgument && flag==expectedFlag;++forwarded[1];
    ++reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(self)+0xb0)[4];return 0xfedcba98;
}
uintptr_t __fastcall Copy(void* self,void*,void* argument) {
    argumentsValid&=self==expectedCamera && argument==expectedArgument;++forwarded[2];
    std::memcpy(argument,static_cast<uint8_t*>(self)+0xb0,recordBytes);return reinterpret_cast<uintptr_t>(argument);
}
uintptr_t __fastcall Stage(void* self,void*,void* argument) {
    argumentsValid&=self==expectedCamera && argument==expectedArgument;++forwarded[3];
    ++reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(self)+0xb0)[5];return 0x87654321;
}
uintptr_t __fastcall Getter(void* self,void*) {
    if(exportInterleaveCamera) {
        const auto previous=expectedCamera;expectedCamera=reinterpret_cast<void*>(exportInterleaveCamera);
        TestCall(0,expectedCamera,expectedArgument);expectedCamera=previous;
    }
    argumentsValid&=self==expectedCamera;++forwarded[4];return reinterpret_cast<uintptr_t>(self)+0xb0;
}
uintptr_t __fastcall Render(void* self,void*,uintptr_t context,uintptr_t view,uintptr_t packet) {
    argumentsValid&=self==expectedCamera && context==reinterpret_cast<uintptr_t>(expectedArgument) &&
        view==0x12345678 && packet==0xfedcba98;++forwarded[5];return 0xabcdef01;
}
void Hold() { SetEvent(entered);WaitForSingleObject(release,5000); }
uintptr_t __fastcall Provider(void* self,void*,void* output) {
    argumentsValid&=self==expectedProvider && output==expectedProviderOutput;++forwarded[6];
    if(providerBlocked)Hold();
    std::memcpy(output,providerPosition.data(),sizeof(providerPosition));
    return reinterpret_cast<uintptr_t>(output)+providerReturnOffset;
}
uintptr_t __fastcall PreQueryStage(void* self,void*,void* timing) {
    argumentsValid&=self==expectedCamera && timing==expectedArgument;++forwarded[3];
    auto* record=reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(self)+0xb0);
    record[4]=beforeProviderWord;
    if(providerAction==ProviderAction::NestedStage && !stageDepth) {
        ++stageDepth;argumentsValid&=TestCall(3,self,timing)==stageResult;--stageDepth;
    } else {
        alignas(16) std::array<uint32_t,8> output{};output.fill(0x76543210);
        expectedProviderOutput=output.data();
        const uintptr_t caller=providerAction==ProviderAction::WrongCaller?0x1f78ec:
            providerAction==ProviderAction::HelperCaller?0x18f8d2:0x1f78ed;
        uintptr_t returned{};
        const auto invoke=[&]{returned=TestProvider(expectedProvider,output.data(),caller);};
        if(providerAction==ProviderAction::InterleavedUpdate)TestCall(0,self,timing);
        if(providerAction==ProviderAction::ForeignThread) { std::thread other(invoke);other.join(); }
        else invoke();
        if(providerAction==ProviderAction::Duplicate)invoke();
        argumentsValid&=returned==reinterpret_cast<uintptr_t>(output.data())+providerReturnOffset &&
            !std::memcmp(output.data(),providerPosition.data(),sizeof(providerPosition));
        for(size_t i=4;i<output.size();++i)argumentsValid&=output[i]==0x76543210;
    }
    record[5]=afterProviderWord;return stageResult;
}
void FinishUnderLock() { SetEvent(release);completedUnderLock=WaitForSingleObject(finished,2000)==WAIT_OBJECT_0; }
uintptr_t __fastcall Inflight(void* self,void*,void* argument) {
    Hold();return Update(self,nullptr,argument);
}
uintptr_t nestedCamera{};unsigned nestedRemaining{};
uintptr_t __fastcall ExhaustLedger(void*,void*,void* argument) {
    if(nestedRemaining) {
        --nestedRemaining;nestedCamera+=0x3000;
        TestCall(0,reinterpret_cast<void*>(nestedCamera),argument);
    }
    return 0x12345678;
}
std::string Receipt(const std::filesystem::path& folder) {
    std::ifstream in(folder/L"chase-camera-observer.json");return {std::istreambuf_iterator<char>(in),{}};
}
size_t Occurrences(const std::string& text,const std::string& value) {
    size_t count=0,at=0;while((at=text.find(value,at))!=std::string::npos){++count;at+=value.size();}return count;
}
// The receipt has no escaped strings or braces in values. Keep fixture parsing
// local and distinguish nested provenance objects from top-level samples.
std::string ObjectAt(const std::string& json,size_t at) {
    if(at==std::string::npos || json[at]!='{')return {};
    unsigned depth=0;
    for(size_t end=at;end<json.size();++end) {
        if(json[end]=='{')++depth;
        else if(json[end]=='}' && !--depth)return json.substr(at,end-at+1);
    }
    return {};
}
std::string Object(const std::string& json,const char* key) {
    const auto at=json.find(std::string("\"")+key+"\":{");
    return at==std::string::npos?std::string{}:ObjectAt(json,json.find('{',at));
}
uint64_t Number(const std::string& json,const char* key) {
    const std::string prefix=std::string("\"")+key+"\":";
    const auto at=json.find(prefix);
    return at==std::string::npos?UINT64_MAX:std::stoull(json.substr(at+prefix.size()));
}
bool Reason(const std::string& json,const char* value) {
    return json.find(std::string("\"reason\":\"")+value+'"')!=std::string::npos;
}
std::vector<std::string> Samples(const std::string& json) {
    std::vector<std::string> result;auto at=json.find("\"samples\":[");
    if(at==std::string::npos)return result;at+=11;
    while(at<json.size() && json[at]=='{') {
        const auto sample=ObjectAt(json,at);if(sample.empty())break;
        result.push_back(sample);at+=sample.size();if(json[at]==',')++at;
    }
    return result;
}
void FakeOriginals(void* update=reinterpret_cast<void*>(Update),void* stage=reinterpret_cast<void*>(Stage)) {
    TestOriginals(update,reinterpret_cast<void*>(Alternate),reinterpret_cast<void*>(Copy),stage,reinterpret_cast<void*>(Getter),reinterpret_cast<void*>(Render),reinterpret_cast<void*>(Provider));
}
struct Correlation {
    uintptr_t base,controller,manager,camera,farCamera,main,renderer,context,packet;
    alignas(16) std::array<uintptr_t,28> ownerStorage{};
    std::span<uintptr_t,25> owner{ownerStorage.data()+3,25};
    std::array<uintptr_t,15> scene{};
    static void Word(uintptr_t at,uintptr_t value) { *reinterpret_cast<uintptr_t*>(at)=value; }
    explicit Correlation(uintptr_t image):base(image),controller(base+0x8000),manager(base+0xc000),camera(base+0x10000),
        farCamera(base+0x30000),main(base+0x14000),renderer(base+0x18000),context(base+0x20000),packet(base+0x24000) {
        const auto root=base+0x1000,state=base+0x4000;
        Word(base+0xdef810,root);Word(root+0x10,state);Word(state+0x14fc,controller);
        Word(controller+0x33c,manager);Word(manager+0x748,camera);
        Word(main,base+mainTableRva);Word(main+0x14,0x400);Word(main+0x18,1);Word(main+0x94,1);
        Word(main+0x28,renderer);Word(renderer,base+0xd4a5a0);Word(main+0x5e4,context);
        owner[0]=base+0x8047a0;Parent(0x1185af);owner[22]=controller+0x1f0;
        scene[0]=base+0x3be09e;scene[1]=scene[12]=context;scene[2]=scene[11]=main+0x680;
        scene[3]=scene[9]=packet;scene[8]=base+0x7a7cd2;scene[10]=main+0x5f0;
        for(const auto [at,name]:{std::pair{camera,"chase_close"},std::pair{farCamera,"chase_far"}}) {
            Word(at,base+chaseTableRva);std::memcpy(reinterpret_cast<void*>(at+0x19),name,std::strlen(name)+1);
            uint32_t hash=5381;for(const auto* ch=name;*ch;++ch)hash=hash*33+static_cast<uint8_t>(*ch);Word(at+0x98,hash);
        }
    }
    void Parent(uintptr_t rva) { owner[18]=owner[21]=base+rva; }
    bool Export(uintptr_t selected) {
        Word(manager+0x748,selected);expectedCamera=reinterpret_cast<void*>(selected);
        return TestExport(expectedCamera,owner.data())==selected+0xb0;
    }
    void RenderCapture() { TestRenderCapture(reinterpret_cast<void*>(main),context,main+0x680,packet,scene.data()); }
};
struct PreQueryFixture:Correlation {
    uintptr_t provider,entity;
    alignas(16) std::array<uint32_t,4> timing{0x3c888889,2,3,4};
    explicit PreQueryFixture(uintptr_t image):Correlation(image),provider(base+0x40000),entity(base+0x44000) {
        Word(provider,base+providerTableRva);Word(provider+8,entity);Word(provider+0xc,0x2468);
        std::memcpy(reinterpret_cast<void*>(entity+0x330),providerPosition.data(),sizeof(providerPosition));
        Word(entity+0x300,0x3f800000);Word(entity+0x314,0x3f800000);Word(entity+0x328,0x3f800000);
        Word(entity+0x11a8,7);
        for(auto selected:{camera,farCamera}) {
            Word(selected+0xc,1);Word(selected+0x60,provider);
            Word(selected+0x24c0,base+0x4c000);Word(selected+0x24c4,base+0x4d000);Word(selected+0x24d8,base+0x4e000);
            Word(selected+0x253c,11);Word(selected+0x2540,12);Word(selected+0x2544,13);Word(selected+0x254c,14);
        }
        Select(camera);
    }
    void Select(uintptr_t selected) {
        expectedCamera=reinterpret_cast<void*>(selected);expectedArgument=timing.data();expectedProvider=reinterpret_cast<void*>(provider);
        Word(manager+0x748,selected);std::memset(reinterpret_cast<void*>(selected+0xb0),0,recordBytes);
    }
    bool Run(unsigned next,ProviderAction action=ProviderAction::Normal) {
        providerAction=action;Present(next,next%2?5:4);
        return TestCall(3,expectedCamera,timing.data())==stageResult && argumentsValid;
    }
};
}
int main(int argc,char** argv) {
    if(argc!=3)return 1;
    const std::filesystem::path folder=std::filesystem::path(argv[1])/argv[2];std::filesystem::create_directories(folder);
    const std::string mode=argv[2];
    constexpr size_t imageBytes=0xe70000;
    auto* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,imageBytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!image)return 2;const auto base=reinterpret_cast<uintptr_t>(image);
    const auto put=[&](uintptr_t at,const auto& bytes){std::memcpy(image+at,bytes,sizeof(bytes));};
    put(routines[0],updateEntry);put(routines[0]+0x2d,updateCall);put(routines[1],alternateBody);put(routines[2],copyBody);
    put(routines[3],obstructionBody);put(routines[4],recordPointerBody);
    put(0x804780,viewConverterCall);put(0x118581,selectedViewA);put(0x21a5f3,selectedViewB);
    put(0x2e3930,auxiliaryFrameEntry);put(0x2e394b,auxiliaryFrameAllocation);put(0x2e3c16,auxiliaryViewCall);
    put(routines[5],mainSceneEntry);put(0x3be050,completeSceneEntry);put(0x3be087,mainSceneCall);put(0x7a7cb3,completeSceneCall);
    const auto providerGuard=ProviderPositionGuard(base);
    std::memcpy(image+routines[6],providerGuard.data(),providerGuard.size());
    put(0x1f78a0,preQueryEntry);put(0x61eb91,preQueryStage);put(providerBasisRva,providerBasisBody);
    for(size_t i=0;i<slots.size();++i)*reinterpret_cast<uintptr_t*>(image+SlotRva(i))=base+routines[i];
    *reinterpret_cast<uintptr_t*>(image+providerBasisSlotRva)=base+providerBasisRva;
    if(!GuardsMatch(base) || GuardsMatch(0))return 3;
    if(!ValidRequest("observe-v1\n"))return 4;
    for(auto bad:{"","observe-v1","observe-v1\r\n","observe-v1\nextra","apply-v1\n","observe-v2\n"})if(ValidRequest(bad))return 5;
    for(const auto [at,count]:std::array<std::pair<uintptr_t,size_t>,20>{{{routines[0],sizeof(updateEntry)},
        {routines[0]+0x2d,sizeof(updateCall)},{routines[1],sizeof(alternateBody)},{routines[2],sizeof(copyBody)},{routines[3],sizeof(obstructionBody)},
        {routines[4],sizeof(recordPointerBody)},{0x804780,sizeof(viewConverterCall)},{0x118581,sizeof(selectedViewA)},{0x21a5f3,sizeof(selectedViewB)},
        {routines[5],sizeof(mainSceneEntry)},{0x3be050,sizeof(completeSceneEntry)},{0x3be087,sizeof(mainSceneCall)},{0x7a7cb3,sizeof(completeSceneCall)},
        {routines[6],providerGuard.size()},{0x1f78a0,sizeof(preQueryEntry)},{0x61eb91,sizeof(preQueryStage)},{providerBasisRva,sizeof(providerBasisBody)},
        {0x2e3930,sizeof(auxiliaryFrameEntry)},{0x2e394b,sizeof(auxiliaryFrameAllocation)},{0x2e3c16,sizeof(auxiliaryViewCall)}}}) {
        for(size_t i=0;i<count;++i) { image[at+i]^=1;if(GuardsMatch(base))return 6;image[at+i]^=1; }
    }
    for(size_t route=0;route<slots.size();++route) { image[SlotRva(route)]^=1;if(GuardsMatch(base))return 7;image[SlotRva(route)]^=1; }
    image[providerBasisSlotRva]^=1;if(GuardsMatch(base))return 69;image[providerBasisSlotRva]^=1;
    std::vector<uint8_t> untouched(image,image+imageBytes);
    { std::ofstream request(folder/L"chase-camera-observer.request",std::ios::binary);
      request<<(mode=="refusal"?std::string("observe-v1\n")+std::string(4096,'x'):"observe-v1\n"); }
    Prepare(base,folder);
    if(mode.starts_with("correlation-")) {
        PreQueryFixture graph(base);FakeOriginals(reinterpret_cast<void*>(Update),reinterpret_cast<void*>(PreQueryStage));
        alignas(16) std::array<uintptr_t,160> auxiliary{};
        auto* auxiliaryStack=auxiliary.data()+3;const auto slot=reinterpret_cast<uintptr_t>(auxiliaryStack);
        auxiliaryStack[0]=base+0x8047a0;auxiliaryStack[17]=slot+0x1c4;
        auxiliaryStack[18]=auxiliaryStack[23]=base+0x2e3c40;auxiliaryStack[24]=slot+0x74;
        const auto auxiliaryExport=[&]{return TestExport(expectedCamera,auxiliaryStack)==graph.camera+0xb0;};
        if(!graph.Run(1))return 201;
        std::array<uint32_t,108> stageRecord{};
        std::memcpy(stageRecord.data(),reinterpret_cast<void*>(graph.camera+0xb0),sizeof(stageRecord));
        const auto update=[&](uintptr_t camera) {
            expectedCamera=reinterpret_cast<void*>(camera);TestCall(0,expectedCamera,graph.timing.data());
            expectedCamera=reinterpret_cast<void*>(graph.camera);
        };
        bool expectedChain=true;
        if(mode=="correlation-six-cameras") {
            for(unsigned i=0;i<5;++i)update(base+0x60000+i*0x3000);
        } else if(mode=="correlation-cache-retention") {
            TestCall(3,expectedCamera,graph.timing.data()); // Cache a stage that is not the pinned first stage.
            for(unsigned i=0;i<120;++i) { Present(i+2,i%2?5:4);update(graph.farCamera); }
        } else if(mode=="correlation-burst") {
            // The second native stage is inside the one-second log quota, but
            // refreshes the current selected-camera snapshot during the burst.
            TestCall(3,expectedCamera,graph.timing.data());
        } else if(mode=="correlation-burst-limit") {
            for(unsigned i=1;i<129;++i)TestCall(3,expectedCamera,graph.timing.data());
            expectedChain=false;
        } else if(mode=="correlation-same-camera-aba") {
            update(graph.camera);std::memcpy(reinterpret_cast<void*>(graph.camera+0xb0),stageRecord.data(),sizeof(stageRecord));
            expectedChain=false;
        } else if(mode=="correlation-contention") {
            TestWithLock([] { std::thread other([] { TestCall(0,expectedCamera,expectedArgument); });other.join(); });
            std::memcpy(reinterpret_cast<void*>(graph.camera+0xb0),stageRecord.data(),sizeof(stageRecord));expectedChain=false;
        } else if(mode=="correlation-ledger-full") {
            nestedCamera=base+0x60000;nestedRemaining=8;
            FakeOriginals(reinterpret_cast<void*>(ExhaustLedger),reinterpret_cast<void*>(PreQueryStage));
            TestCall(0,reinterpret_cast<void*>(nestedCamera),graph.timing.data());expectedChain=false;
        } else if(mode=="correlation-identity") {
            Correlation::Word(graph.provider+0xc,99); // Identity below deliberately changes entity, not just record values.
            Correlation::Word(graph.provider+8,graph.entity+0x100);expectedChain=false;
        } else if(mode=="correlation-eviction") {
            for(unsigned i=0;i<9;++i)update(base+0x60000+i*0x3000);expectedChain=false;
        } else if(mode=="correlation-overlap") {
            entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            providerBlocked=true;
            std::thread other([&]{TestCall(3,expectedCamera,graph.timing.data());});
            const bool started=WaitForSingleObject(entered,2000)==WAIT_OBJECT_0;
            update(graph.camera);SetEvent(release);other.join();CloseHandle(entered);CloseHandle(release);
            if(!started)return 202;expectedChain=false;
        } else if(mode=="correlation-before-selection") {
            Correlation::Word(graph.manager+0x748,graph.farCamera);
            TestExport(expectedCamera,graph.owner.data()); // Native selected pointer has not been committed yet.
            Stop();const auto json=Receipt(folder);
            if(!Reason(json,"owner_camera_mismatch") || Reason(json,"matched_stage_record"))return 203;
            return 0;
        }
        if(mode=="correlation-during-export")exportInterleaveCamera=graph.farCamera;
        if(mode=="correlation-auxiliary-throttled" && !auxiliaryExport())return 210;
        if(mode=="correlation-shared-output-reverse")graph.Parent(0x21a621);
        if(!graph.Export(graph.camera))return 204;
        if((mode=="correlation-auxiliary-sampled" || mode=="correlation-auxiliary-throttled") && !auxiliaryExport())return 211;
        if(mode=="correlation-other-before-render")update(graph.farCamera);
        if(mode=="correlation-foreign-render") {
            std::thread other([&]{graph.RenderCapture();});other.join();
        }
        bool expectedRender=true;
        if(mode=="correlation-shared-output" || mode=="correlation-shared-output-reverse") {
            graph.Parent(mode=="correlation-shared-output"?0x21a621:0x1185af);
            std::thread other([&]{graph.Export(graph.camera);});other.join();expectedRender=false;
        } else if(mode=="correlation-busy-output") {
            *reinterpret_cast<uint8_t*>(graph.controller+0x392)=1;
            graph.Export(graph.camera);*reinterpret_cast<uint8_t*>(graph.controller+0x392)=0;expectedRender=false;
        } else if(mode=="correlation-unknown-output") {
            graph.Parent(0x123456);graph.Export(graph.camera);expectedRender=false;
        } else if(mode=="correlation-unreadable-output") {
            if(TestExport(expectedCamera,reinterpret_cast<void*>(1))!=graph.camera+0xb0)return 212;
            expectedRender=false;
        } else if(mode=="correlation-overlapping-output") {
            graph.owner[22]+=0x10;graph.Export(graph.camera);expectedRender=false;
        }
        graph.RenderCapture();graph.RenderCapture(); // Successful entries are consumed once.
        Stop();const auto json=Receipt(folder);const auto results=Object(json,"renderResults");
        if(!argumentsValid || Samples(json).size()>sampleLimit ||
           Number(results,"captured_main_scene")!=(expectedRender?1:0))return 205;
        unsigned chains=0;
        for(const auto& sample:Samples(json))if(Reason(Object(sample,"stageAssociation"),"matched_stage_record") &&
            Reason(Object(sample,"renderObservation"),"captured_main_scene")) {
            ++chains;
            if((mode=="correlation-cache-retention" || mode=="correlation-burst") &&
                Number(Object(Object(sample,"stageAssociation"),"preObstruction"),"sequence")!=2)return 213;
        }
        if(chains!=(expectedChain && expectedRender?1:0))return 206;
        if((mode=="correlation-six-cameras" || mode=="correlation-during-export") &&
            json.find("\"legacyReason\":\"stage_superseded\"")==std::string::npos)return 207;
        if(mode=="correlation-burst-limit" && Number(Object(json,"preQueryResults"),"prequery_unsampled_stage")!=1)return 208;
        if(mode=="correlation-foreign-render" && Number(results,"export_thread_changed")!=1)return 209;
        if(mode=="correlation-ledger-full" && Number(json,"trackingTaint")!=1)return 214;
    } else if(mode=="forwarding") {
        FakeOriginals();
        std::array<uint8_t,0x2570> camera{},expected{};std::array<uint32_t,4> timing{0x3c888889,2,3,4};
        std::array<uint8_t,recordBytes> output{};camera.fill(0x7c);expected=camera;
        std::array<uint8_t,16> provider{};alignas(16) std::array<uint32_t,8> providerOutput{};providerOutput.fill(0x76543210);
        expectedProvider=provider.data();expectedProviderOutput=providerOutput.data();
        // Vary observation state, exercising ring overflow and the real compiled
        // thiscall/fastcall boundary without running any executable game code.
        for(unsigned i=0;i<200;++i) {
            Present(i+1,i%2?4:5);expectedCamera=camera.data();expectedArgument=timing.data();expectedFlag=0xa0000000+i;
            ++reinterpret_cast<uint32_t*>(expected.data()+0xb0)[3];
            if(TestCall(0,camera.data(),timing.data())!=0x12345678 || camera!=expected)return 8;
            ++reinterpret_cast<uint32_t*>(expected.data()+0xb0)[4];
            if(TestCall(1,camera.data(),timing.data(),expectedFlag)!=0xfedcba98 || camera!=expected)return 9;
            expectedArgument=output.data();
            if(TestCall(2,camera.data(),output.data())!=reinterpret_cast<uintptr_t>(output.data()) || camera!=expected ||
               std::memcmp(output.data(),camera.data()+0xb0,recordBytes))return 10;
            expectedArgument=timing.data();++reinterpret_cast<uint32_t*>(expected.data()+0xb0)[5];
            if(TestCall(3,camera.data(),timing.data())!=0x87654321 || camera!=expected)return 25;
            if(TestCall(4,camera.data(),nullptr)!=reinterpret_cast<uintptr_t>(camera.data())+0xb0 || camera!=expected)return 39;
            if(TestRender(camera.data(),reinterpret_cast<uintptr_t>(timing.data()),0x12345678,0xfedcba98)!=0xabcdef01 || camera!=expected)return 41;
            if(TestCall(6,provider.data(),providerOutput.data())!=reinterpret_cast<uintptr_t>(providerOutput.data()) ||
               std::memcmp(providerOutput.data(),providerPosition.data(),sizeof(providerPosition)) || camera!=expected)return 65;
            for(size_t word=4;word<providerOutput.size();++word)if(providerOutput[word]!=0x76543210)return 66;
        }
        // Repeated evaluation in the same frame must still forward, unchanged.
        expectedArgument=timing.data();++reinterpret_cast<uint32_t*>(expected.data()+0xb0)[3];
        if(TestCall(0,camera.data(),timing.data())!=0x12345678 || camera!=expected)return 11;
        ++reinterpret_cast<uint32_t*>(expected.data()+0xb0)[5];
        if(TestCall(3,camera.data(),timing.data())!=0x87654321 || camera!=expected)return 26;
        if(TestCall(4,camera.data(),nullptr)!=reinterpret_cast<uintptr_t>(camera.data())+0xb0 || camera!=expected)return 40;
        if(!argumentsValid || forwarded[0]!=201 || forwarded[1]!=200 || forwarded[2]!=200 || forwarded[3]!=201 || forwarded[4]!=201 || forwarded[5]!=200 || forwarded[6]!=200)return 12;
        Stop();const auto json=Receipt(folder);
        // Non-converter getter calls forward but must not pollute the export samples.
        if(json.find("\"calls\":[201,200,200,201,0,0,0]")==std::string::npos || json.find("\"captured\":800")==std::string::npos || json.size()>400000)return 13;
        if(Samples(json).size()!=sampleLimit-6)return 14; // Six slots preserve the first close/far evidence chains.
    } else if(mode=="provider-stage") {
        PreQueryFixture graph(base);FakeOriginals(reinterpret_cast<void*>(Update),reinterpret_cast<void*>(PreQueryStage));
        // Fill transition retention, then pin distinct ordinary and complete
        // close/far chains. The final overflow must retain all four exports.
        for(unsigned i=0;i<32;++i) {Present(i+1,i%2?4:5);TestCall(0,expectedCamera,graph.timing.data());}
        Present(33,5);if(!graph.Export(graph.camera))return 70;graph.RenderCapture();
        Present(34,4);if(!graph.Export(graph.farCamera))return 71;graph.RenderCapture();
        unsigned next=35;
        for(auto selected:{graph.camera,graph.farCamera}) {
            graph.Select(selected);
            std::array<uint8_t,0x2570> expected{};std::memcpy(expected.data(),expectedCamera,expected.size());
            reinterpret_cast<uint32_t*>(expected.data()+0xb0)[4]=beforeProviderWord;
            reinterpret_cast<uint32_t*>(expected.data()+0xb0)[5]=afterProviderWord;
            const auto timing=graph.timing;
            if(!graph.Run(next++) || std::memcmp(expectedCamera,expected.data(),expected.size()) || graph.timing!=timing)return 72;
            if(!graph.Export(selected))return 73;graph.RenderCapture();
        }
        graph.Select(graph.camera);
        for(unsigned i=0;i<200;++i) {Present(i+37,i%2?4:5);TestCall(0,expectedCamera,graph.timing.data());}
        Stop();const auto json=Receipt(folder);const auto samples=Samples(json);
        if(!argumentsValid || forwarded[3]!=2 || forwarded[6]!=2 || samples.size()!=sampleLimit ||
           json.find("\"firstSuccessfulPairs\":{\"chase_close\":33,\"chase_far\":34}")==std::string::npos ||
           json.find("\"firstPreQueryRenderPairs\":{\"chase_close\":36,\"chase_far\":38}")==std::string::npos ||
           json.find("\"firstPreQueryStages\":{\"chase_close\":35,\"chase_far\":37}")==std::string::npos)return 74;
        unsigned chains=0;
        for(const auto& sample:samples) {
            const auto association=Object(sample,"stageAssociation");
            if(!Reason(association,"matched_stage_record"))continue;
            ++chains;const auto pre=Object(association,"preObstruction"),snapshot=Object(pre,"snapshot");
            const auto sequence=Number(sample,"sequence"),selected=Number(sample,"camera");
            if((sequence!=36 && sequence!=38) || !Reason(pre,"captured_prequery") ||
               Number(pre,"sequence")!=sequence-1 || Number(pre,"evaluationSerial")!=(sequence==36?33:34) ||
               Number(pre,"threadId")!=GetCurrentThreadId() || Number(pre,"camera")!=selected ||
               Number(pre,"timing")!=reinterpret_cast<uintptr_t>(graph.timing.data()) || Number(pre,"callerRva")!=0x1f78ed ||
               Number(pre,"calls")!=1 || Number(snapshot,"provider")!=graph.provider || Number(snapshot,"entity")!=graph.entity ||
               Number(snapshot,"record")!=selected+0xb0 || Number(snapshot,"returned")!=Number(snapshot,"output") ||
               snapshot.find("\"recordWords\":[0,0,0,0,"+std::to_string(beforeProviderWord)+",0,")==std::string::npos ||
               association.find("\"beforeWords\":[0,0,0,0,0,0,")==std::string::npos ||
               association.find("\"afterWords\":[0,0,0,0,"+std::to_string(beforeProviderWord)+','+std::to_string(afterProviderWord)+',')==std::string::npos ||
               !Reason(Object(sample,"renderObservation"),"captured_main_scene"))return 75;
        }
        if(chains!=2 || Number(Object(json,"preQueryResults"),"captured_prequery")!=2)return 76;
    } else if(mode=="provider-context") {
        PreQueryFixture graph(base);FakeOriginals(reinterpret_cast<void*>(Update),reinterpret_cast<void*>(PreQueryStage));
        alignas(16) std::array<uint32_t,4> output{};expectedProviderOutput=output.data();
        if(TestProvider(expectedProvider,output.data())!=reinterpret_cast<uintptr_t>(output.data()) || output!=providerPosition)return 77;
        unsigned next=1;
        for(auto action:{ProviderAction::WrongCaller,ProviderAction::HelperCaller,ProviderAction::ForeignThread,
            ProviderAction::NestedStage,ProviderAction::Duplicate,ProviderAction::InterleavedUpdate}) {
            graph.Select(graph.camera);if(!graph.Run(next++,action))return 78;
        }
        graph.Select(graph.camera);Correlation::Word(graph.camera+0xc,0);
        if(!graph.Run(next++))return 79;Correlation::Word(graph.camera+0xc,1);
        graph.Select(graph.camera);Correlation::Word(graph.camera+0x60,graph.entity);
        if(!graph.Run(next++))return 80;Correlation::Word(graph.camera+0x60,graph.provider);
        graph.Select(graph.camera);providerReturnOffset=4;
        if(!graph.Run(next++))return 81;providerReturnOffset=0;
        Stop();const auto json=Receipt(folder),results=Object(json,"preQueryResults");
        for(const char* refused:{"prequery_not_called","prequery_nested_stage","prequery_duplicate","prequery_interleaved",
            "prequery_camera_gate","prequery_provider_mismatch","prequery_return_mismatch"})
            if(!Reason(json,refused))return 82;
        if(Reason(json,"captured_prequery") || Number(results,"prequery_without_stage")!=2 || Number(results,"prequery_caller")!=2 ||
           Number(results,"prequery_unsampled_stage")!=1 || !argumentsValid || forwarded[3]!=10 || forwarded[6]!=11 ||
           json.find("\"calls\":[1,0,0,10,0,0,7]")==std::string::npos ||
           json.find("\"entryCalls\":[1,0,0,10,0,0,11]")==std::string::npos ||
           json.find("\"firstPreQueryRenderPairs\":{\"chase_close\":0,\"chase_far\":0}")==std::string::npos ||
           json.find("\"firstPreQueryStages\":{\"chase_close\":0,\"chase_far\":0}")==std::string::npos)return 83;
    } else if(mode=="provider-chain") {
        PreQueryFixture graph(base);FakeOriginals(reinterpret_cast<void*>(Update),reinterpret_cast<void*>(PreQueryStage));
        Present(1,5);if(!graph.Export(graph.camera))return 84;
        // A valid stage forces one fresh export through the normal quota.
        if(!graph.Run(1) || !graph.Export(graph.camera))return 85;graph.RenderCapture();
        if(!graph.Export(graph.camera))return 86; // Consumed stages cannot force another sample.
        graph.Select(graph.camera);Present(2,4);TestCall(0,expectedCamera,graph.timing.data());
        if(!graph.Run(2))return 87;
        TestCall(0,expectedCamera,graph.timing.data()); // Throttled update still invalidates the stage.
        if(!graph.Export(graph.camera))return 88;
        graph.Select(graph.camera);if(!graph.Run(3))return 89;
        TestCall(3,expectedCamera,graph.timing.data()); // Throttled stage also advances the evaluation generation.
        if(!graph.Export(graph.camera))return 90;
        graph.Select(graph.camera);if(!graph.Run(4))return 91;
        ++reinterpret_cast<uint32_t*>(graph.camera+0xb0)[6];if(!graph.Export(graph.camera))return 92;
        graph.Select(graph.camera);if(!graph.Run(5) || !graph.Export(graph.farCamera))return 93;
        graph.Select(graph.camera);if(!graph.Run(6))return 94;
        const auto owner=graph.owner[22];graph.owner[22]=0x1234;
        if(!graph.Export(graph.camera))return 95;
        // A failed ownership check gets one forced attempt for this stage;
        // repeated native exports must not defeat the ordinary sampling quota.
        if(!graph.Export(graph.camera) || !graph.Export(graph.camera))return 107;
        graph.owner[22]=owner;
        graph.Select(graph.camera);if(!graph.Run(7) || !graph.Export(graph.camera))return 96;
        TestCall(0,expectedCamera,graph.timing.data());graph.RenderCapture();
        // A quiescent stage can transfer to another thread, but the export and
        // render still share their original stricter provenance requirements.
        graph.Select(graph.camera);bool completed=false;DWORD stageThread{};
        std::thread other([&]{stageThread=GetCurrentThreadId();completed=graph.Run(8);});other.join();
        if(!completed || !graph.Export(graph.camera))return 97;graph.RenderCapture();
        // The other guarded native parent can export first without consuming
        // the later render parent's one allowed association with this stage.
        graph.Select(graph.camera);if(!graph.Run(9))return 104;
        graph.Parent(0x21a621);if(!graph.Export(graph.camera))return 105;
        graph.Parent(0x1185af);if(!graph.Export(graph.camera))return 106;graph.RenderCapture();
        Stop();const auto json=Receipt(folder);const auto samples=Samples(json);
        for(const char* refused:{"stage_superseded","stage_record_changed","stage_not_available","stage_export_unqualified","stage_superseded_before_render"})
            if(!Reason(json,refused))return 98;
        unsigned matched=0,refusedOwnerAttempts=0;bool crossThread=false,firstParent=false,renderParent=false;
        for(const auto& sample:samples) {
            if(Number(sample,"presentFrame")==6 && Reason(Object(sample,"stageAssociation"),"stage_export_unqualified"))++refusedOwnerAttempts;
            const auto association=Object(sample,"stageAssociation");if(!Reason(association,"matched_stage_record"))continue;
            ++matched;const auto pre=Object(association,"preObstruction");
            if(Number(pre,"threadId")==stageThread && Number(sample,"threadId")==GetCurrentThreadId() &&
               Reason(Object(sample,"renderObservation"),"captured_main_scene"))crossThread=true;
            if(Number(sample,"presentFrame")==9) {
                firstParent|=Number(sample,"nativeParentRva")==0x21a621 && Object(sample,"renderObservation").empty();
                renderParent|=Number(sample,"nativeParentRva")==0x1185af && Reason(Object(sample,"renderObservation"),"captured_main_scene");
            }
        }
        if(matched!=4 || refusedOwnerAttempts!=1 || !crossThread || !firstParent || !renderParent || !argumentsValid || Number(Object(json,"preQueryResults"),"prequery_unsampled_stage")!=1 ||
           json.find("\"firstPreQueryRenderPairs\":{\"chase_close\":3,\"chase_far\":0}")==std::string::npos)return 99;
    } else if(mode=="provider-stop") {
        Install();PreQueryFixture graph(base);FakeOriginals(reinterpret_cast<void*>(Update),reinterpret_cast<void*>(PreQueryStage));
        entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!entered || !release)return 100;providerBlocked=true;
        uintptr_t result{};Present(1,5);
        std::thread native([&]{result=TestCall(3,expectedCamera,graph.timing.data());});
        const bool started=WaitForSingleObject(entered,2000)==WAIT_OBJECT_0;
        const auto began=GetTickCount64();const auto stopped=StopForExit();const auto elapsed=GetTickCount64()-began;
        const std::array<void*,7> targets{reinterpret_cast<void*>(Update),reinterpret_cast<void*>(Alternate),reinterpret_cast<void*>(Copy),
            reinterpret_cast<void*>(PreQueryStage),reinterpret_cast<void*>(Getter),reinterpret_cast<void*>(Render),reinterpret_cast<void*>(Provider)};
        bool restored=true;for(size_t i=0;i<slots.size();++i)restored&=*reinterpret_cast<void**>(image+SlotRva(i))==targets[i];
        SetEvent(release);native.join();CloseHandle(entered);CloseHandle(release);
        if(!started || !restored || std::strcmp(stopped,"observer_restored") || elapsed>2000 || result!=stageResult ||
           !argumentsValid || forwarded[3]!=1 || forwarded[6]!=1)return 101;
        // A fresh stop snapshot includes the returning callback's refusal,
        // while its native result and sixteen output bytes remain unchanged.
        if(std::strcmp(StopForExit(),"observer_restored"))return 102;
        const auto json=Receipt(folder);
        if(Number(json,"captured")!=0 || Number(Object(json,"preQueryResults"),"observer_stopped")!=1 ||
           Reason(json,"captured_prequery") || json.find("\"stopped\":true,\"restored\":true")==std::string::npos)return 103;
    } else if(mode=="provenance") {
        // Synthetic stack words exercise the real sampling/retention/receipt
        // path. The separate forwarding case tests the compiled native ABI.
        const uintptr_t root=base+0x1000,state=base+0x4000,controller=base+0x8000,manager=base+0xc000;
        const uintptr_t camera=base+0x10000,main=base+0x14000,renderer=base+0x18000,context=base+0x20000,packet=base+0x24000;
        const auto word=[&](uintptr_t at,uintptr_t value){*reinterpret_cast<uintptr_t*>(at)=value;};
        word(base+0xdef810,root);word(root+0x10,state);word(state+0x14fc,controller);
        word(controller+0x33c,manager);word(manager+0x748,camera);word(camera,base+chaseTableRva);
        word(main,base+mainTableRva);word(main+0x14,0x400);word(main+0x18,1);word(main+0x94,1);
        word(main+0x28,renderer);word(renderer,base+0xd4a5a0);word(main+0x5e4,context);
        alignas(16) std::array<uintptr_t,28> ownerStorage{};std::span<uintptr_t,25> owner{ownerStorage.data()+3,25};
        owner[0]=base+0x8047a0;owner[18]=owner[21]=base+0x1185af;owner[22]=controller+0x1f0;
        std::array<uintptr_t,15> scene{};scene[0]=base+0x3be09e;scene[1]=scene[12]=context;
        scene[2]=scene[11]=main+0x680;scene[3]=scene[9]=packet;scene[8]=base+0x7a7cd2;scene[10]=main+0x5f0;
        FakeOriginals();
        expectedCamera=reinterpret_cast<void*>(camera);
        const auto observe=[&]{TestRenderCapture(reinterpret_cast<void*>(main),context,main+0x680,packet,scene.data());};
        const auto source=[&](unsigned next){Present(next,next);return TestExport(expectedCamera,owner.data())==camera+0xb0;};
        if(!source(1))return 44;
        observe();scene[0]=0;observe();scene[0]=base+0x3be09e; // A duplicate cannot replace the captured boundary.
        if(!source(2) || TestExport(expectedCamera,owner.data())!=camera+0xb0)return 45;
        observe(); // Unsampled duplicate export invalidates the old source.
        if(!source(3))return 46;
        Present(4,4);observe();
        if(!source(5))return 47;
        std::thread foreign(observe);foreign.join();
        if(!source(6))return 48;
        scene[0]=0;observe();
        Stop();const auto json=Receipt(folder);
        for(const char* expectedReason:{"matched_export_provenance","captured_main_scene","export_superseded","export_present_changed","export_thread_changed","render_scene_stack"})
            if(json.find(std::string("\"reason\":\"")+expectedReason+'"')==std::string::npos)return 49;
        if(Occurrences(json,"\"reason\":\"captured_main_scene\"")!=1 ||
           json.find("\"captured\":5")==std::string::npos || !argumentsValid || forwarded[4]!=6)return 50;
    } else if(mode=="export-quotas") {
        Correlation graph(base);FakeOriginals();Present(1,5);
        if(!graph.Export(graph.camera))return 51;
        graph.Parent(0x21a621);
        if(!graph.Export(graph.camera))return 52; // Same camera/thread, different native parent.
        graph.Parent(0x1185af);
        bool exported{};std::thread other([&]{exported=graph.Export(graph.camera);});other.join();
        if(!exported || !graph.Export(graph.camera))return 53; // The same parent on another thread has its own quota.
        graph.RenderCapture(); // The unsampled main-thread duplicate still invalidates shared export storage.
        Stop();const auto json=Receipt(folder);
        if(json.find("\"captured\":3")==std::string::npos || json.find("\"throttled\":[0,0,0,0,1,0,0]")==std::string::npos ||
           json.find("\"reason\":\"export_superseded\"")==std::string::npos || !argumentsValid || forwarded[4]!=4)return 54;
    } else if(mode=="sampling-retry") {
        FakeOriginals(reinterpret_cast<void*>(Inflight));
        std::array<uint8_t,0x2570> camera{};std::array<uint32_t,4> timing{0x3c888889,2,3,4};
        expectedCamera=camera.data();expectedArgument=timing.data();Present(1,5);
        entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        finished=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!entered || !release || !finished)return 55;
        uintptr_t result{};std::thread native([&]{result=TestCall(0,camera.data(),timing.data());SetEvent(finished);});
        const bool started=WaitForSingleObject(entered,2000)==WAIT_OBJECT_0;
        TestWithLock(FinishUnderLock);SetEvent(release);native.join();
        if(!started || !completedUnderLock || result!=0x12345678)return 56;
        // Same state and millisecond window retries the abandoned reservation.
        if(TestCall(0,camera.data(),timing.data())!=0x12345678)return 57;
        TestWithLock([]{std::thread contender([]{TestCall(0,expectedCamera,expectedArgument);});contender.join();});
        CloseHandle(entered);CloseHandle(release);CloseHandle(finished);
        Stop();const auto json=Receipt(folder);
        if(json.find("\"captured\":1")==std::string::npos || json.find("\"entryCalls\":[3,0,0,0,0,0,0]")==std::string::npos ||
           json.find("\"calls\":[2,0,0,0,0,0,0]")==std::string::npos || json.find("\"beginDrops\":[1,0,0,0,0,0,0]")==std::string::npos ||
           json.find("\"endDrops\":[1,0,0,0,0,0,0]")==std::string::npos || !argumentsValid || forwarded[0]!=3)return 58;
    } else if(mode=="pair-retention") {
        Correlation graph(base);FakeOriginals();
        std::array<uint32_t,4> timing{0x3c888889,2,3,4};expectedCamera=reinterpret_cast<void*>(graph.camera);expectedArgument=timing.data();
        // Exhaust initial-transition retention before either successful pair.
        for(unsigned i=0;i<32;++i) {Present(i+1,i%2?4:5);TestCall(0,expectedCamera,timing.data());}
        Present(33,5);
        if(!graph.Export(graph.camera))return 59;graph.RenderCapture();
        Present(34,5);if(!graph.Export(graph.farCamera))return 60;graph.RenderCapture();
        expectedCamera=reinterpret_cast<void*>(graph.camera);
        for(unsigned i=0;i<200;++i) {Present(i+35,i%2?4:5);TestCall(0,expectedCamera,timing.data());}
        Stop();const auto json=Receipt(folder);
        if(json.find("\"firstSuccessfulPairs\":{\"chase_close\":33,\"chase_far\":34}")==std::string::npos ||
           Occurrences(json,"\"reason\":\"captured_main_scene\"")!=2 || json.find("\"captured_main_scene\":2")==std::string::npos ||
           json.find("\"captured\":234")==std::string::npos || Samples(json).size()>sampleLimit)return 61;
    } else if(mode=="writer-order") {
        Install();FakeOriginals();
        std::array<uint8_t,0x2570> camera{};std::array<uint32_t,4> timing{0x3c888889,2,3,4};
        expectedCamera=camera.data();expectedArgument=timing.data();
        entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!entered || !release)return 62;
        TestBeforeWrite(Hold);std::thread writer([]{Present(120,5);});
        const bool writing=WaitForSingleObject(entered,2000)==WAIT_OBJECT_0;
        TestCall(0,camera.data(),timing.data()); // Receipt serialization must not hold the native callback mutex.
        const auto started=GetTickCount64();const auto result=StopForExit();const auto elapsed=GetTickCount64()-started;
        const std::array<void*,7> targets{reinterpret_cast<void*>(Update),reinterpret_cast<void*>(Alternate),reinterpret_cast<void*>(Copy),
            reinterpret_cast<void*>(Stage),reinterpret_cast<void*>(Getter),reinterpret_cast<void*>(Render),reinterpret_cast<void*>(Provider)};
        bool restored=true;
        for(size_t i=0;i<slots.size();++i)restored&=*reinterpret_cast<void**>(image+SlotRva(i))==targets[i];
        SetEvent(release);writer.join();CloseHandle(entered);CloseHandle(release);
        if(!writing || !restored || std::strcmp(result,"observer_restored_receipt_pending") || elapsed>2000)return 63;
        const auto json=Receipt(folder);
        if(json.find("\"stopped\":true,\"restored\":true")==std::string::npos || json.find("\"captured\":1")==std::string::npos ||
           json.find("\"beginDrops\":[0,0,0,0,0,0,0]")==std::string::npos || std::filesystem::exists(folder/L"chase-camera-observer.json.tmp"))return 64;
    } else if(mode=="busy") {
        // A stop that cannot acquire the observer mutex must remain bounded and
        // prevent installation even before it can set the locked stopped flag.
        entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!entered || !release)return 27;
        std::thread holder([]{TestWithLock(Hold);});
        const bool locked=WaitForSingleObject(entered,2000)==WAIT_OBJECT_0;
        const auto started=GetTickCount64();const auto result=StopForExit();const auto elapsed=GetTickCount64()-started;
        SetEvent(release);holder.join();CloseHandle(entered);CloseHandle(release);
        if(!locked || std::strcmp(result,"observer_busy") || elapsed>2000)return 28;
        Install();
        if(std::memcmp(image,untouched.data(),imageBytes) || Receipt(folder).find("\"installed\":false")==std::string::npos)return 29;
        if(std::strcmp(StopForExit(),"observer_not_installed"))return 30;
    } else if(mode=="inflight") {
        Install();
        const std::array<void*,7> targets{reinterpret_cast<void*>(Inflight),reinterpret_cast<void*>(Alternate),reinterpret_cast<void*>(Copy),reinterpret_cast<void*>(Stage),reinterpret_cast<void*>(Getter),reinterpret_cast<void*>(Render),reinterpret_cast<void*>(Provider)};
        TestOriginals(targets[0],targets[1],targets[2],targets[3],targets[4],targets[5],targets[6]);
        std::array<uint8_t,0x2570> camera{},expected{};std::array<uint32_t,4> timing{0x3c888889,2,3,4};
        const auto timingBefore=timing;
        expectedCamera=camera.data();expectedArgument=timing.data();
        ++reinterpret_cast<uint32_t*>(expected.data()+0xb0)[3];
        entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!entered || !release)return 31;
        uintptr_t returned{};
        std::thread native([&]{returned=TestCall(0,camera.data(),timing.data());});
        const bool blocked=WaitForSingleObject(entered,2000)==WAIT_OBJECT_0;
        const auto started=GetTickCount64();const auto result=StopForExit();const auto elapsed=GetTickCount64()-started;
        bool restored=true;
        for(size_t i=0;i<slots.size();++i)
            restored&=*reinterpret_cast<void**>(image+SlotRva(i))==targets[i];
        SetEvent(release);native.join();CloseHandle(entered);CloseHandle(release);
        if(!blocked || std::strcmp(result,"observer_restored") || elapsed>2000 || !restored)return 32;
        if(returned!=0x12345678 || !argumentsValid || forwarded[0]!=1 || camera!=expected || timing!=timingBefore)return 33;
        Present(120,5);
        const auto receipt=Receipt(folder);
        if(receipt.find("\"calls\":[1,0,0,0,0,0,0]")==std::string::npos || receipt.find("\"captured\":0")==std::string::npos ||
           receipt.find("\"stopped\":true,\"restored\":true")==std::string::npos)return 34;
    } else {
        DWORD protection{};
        if(!VirtualProtect(image+chaseTableRva,0x1000,PAGE_READONLY,&protection))return 20;
        if(!VirtualProtect(image+mainTableRva,0x1000,PAGE_READONLY,&protection))return 42;
        if(!VirtualProtect(image+providerTableRva,0x1000,PAGE_READONLY,&protection))return 67;
        Install();
        MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQuery(image+chaseTableRva,&region,sizeof(region)) || region.Protect!=PAGE_READONLY)return 21;
        if(mode=="refusal") {
            if(Receipt(folder).find("invalid_request")==std::string::npos || std::memcmp(image,untouched.data(),imageBytes))return 15;
        } else {
            if(Receipt(folder).find("\"installed\":true")==std::string::npos)return 16;
            // Only the seven guarded table words may change; code is identical.
            for(size_t i=0;i<imageBytes;++i) {
                bool slotByte=false;for(size_t route=0;route<slots.size();++route)slotByte|=i>=SlotRva(route) && i<SlotRva(route)+4;
                if(!slotByte && image[i]!=untouched[i])return 17;
            }
            const bool foreign=mode=="foreign" || mode=="exit-foreign";
            if(foreign)for(size_t route:{1,3,5,6}) {
                auto** slot=reinterpret_cast<void**>(image+SlotRva(route));
                if(!grid2vr::SwapGuardedPointer(slot,*slot,reinterpret_cast<void*>(0x12345678)))return 22;
            }
            if(mode=="receipt-failure") {
                const auto receipt=folder/L"chase-camera-observer.json";
                const auto file=CreateFileW(receipt.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
                if(file==INVALID_HANDLE_VALUE)return 35;
                const auto result=StopForExit();CloseHandle(file);
                if(std::strcmp(result,"observer_receipt_failed") || std::memcmp(image,untouched.data(),imageBytes))return 36;
                // Pointer recovery survives a failed receipt and its bounded
                // status write can be retried without reinstalling the hooks.
                if(std::strcmp(StopForExit(),"observer_restored") ||
                   Receipt(folder).find("\"stopped\":true,\"restored\":true")==std::string::npos)return 37;
            } else if(mode=="exit-foreign") {
                if(std::strcmp(StopForExit(),"observer_restore_failed"))return 38;
            } else Stop();
            if(!VirtualQuery(image+chaseTableRva,&region,sizeof(region)) || region.Protect!=PAGE_READONLY)return 23;
            if(!VirtualQuery(image+mainTableRva,&region,sizeof(region)) || region.Protect!=PAGE_READONLY)return 43;
            if(!VirtualQuery(image+providerTableRva,&region,sizeof(region)) || region.Protect!=PAGE_READONLY)return 68;
            if(foreign)for(size_t route:{1,3,5,6}) {
                if(*reinterpret_cast<uintptr_t*>(image+SlotRva(route))!=0x12345678 ||
                   Receipt(folder).find("pointer_restore_failed")==std::string::npos)return 18;
                if(!grid2vr::SwapGuardedPointer(reinterpret_cast<void**>(image+SlotRva(route)),
                    reinterpret_cast<void*>(0x12345678),reinterpret_cast<void*>(base+routines[route])))return 24;
            }
            if(std::memcmp(image,untouched.data(),imageBytes))return 19;
        }
    }
    VirtualFree(image,0,MEM_RELEASE);
    std::cout<<"Chase observer "<<mode<<": passed; synthetic fixtures only.\n";return 0;
}
