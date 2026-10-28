#include "frontend_video_resize.h"
#include "scene_resolution.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>
static_assert(sizeof(void*)==4);
using namespace grid2vr;
namespace { constexpr size_t maxBytes=256u*1024*1024;
void Check(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
bool Contains(size_t size,size_t offset,size_t count) {return offset<=size && count<=size-offset;}
template<class T> T Read(std::span<const uint8_t> bytes,size_t offset) {
    Check(Contains(bytes.size(),offset,sizeof(T)),"PE field lies outside its containing buffer");
    T value{};std::memcpy(&value,bytes.data()+offset,sizeof(value));return value;
}
std::vector<uint8_t> ReadFile(const std::filesystem::path& path,size_t limit) {
    const auto length=std::filesystem::file_size(path);
    Check(length && length<=limit,"fixture file is empty or exceeds its size limit");
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    std::ifstream file(path,std::ios::binary);
    Check(bool(file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()))),"fixture file read failed");
    return bytes;
}
struct Image {
    uint8_t* data{};size_t size{},relocations{};uint32_t preferredBase{};
    ~Image(){if(data)VirtualFree(data,0,MEM_RELEASE);}
    Image()=default;Image(const Image&)=delete;Image& operator=(const Image&)=delete;
    std::span<const uint8_t> Bytes() const {return {data,size};}
    void Load(std::span<const uint8_t> file) {
        const auto dos=Read<IMAGE_DOS_HEADER>(file,0);
        Check(dos.e_magic==IMAGE_DOS_SIGNATURE && dos.e_lfanew>=0,"fixture is not an MZ image");
        const auto nt=static_cast<size_t>(dos.e_lfanew);
        Check(Read<uint32_t>(file,nt)==IMAGE_NT_SIGNATURE,"fixture has no PE signature");
        const auto coff=Read<IMAGE_FILE_HEADER>(file,nt+sizeof(uint32_t));
        Check(coff.Machine==IMAGE_FILE_MACHINE_I386 && coff.NumberOfSections>0 && coff.NumberOfSections<=96 &&
            coff.SizeOfOptionalHeader>=sizeof(IMAGE_OPTIONAL_HEADER32),"fixture is not a bounded i386 PE image");
        const auto optionalAt=nt+sizeof(uint32_t)+sizeof(IMAGE_FILE_HEADER);
        Check(Contains(file.size(),optionalAt,coff.SizeOfOptionalHeader),"truncated optional header");
        const auto optional=Read<IMAGE_OPTIONAL_HEADER32>(file,optionalAt);
        const auto sectionAt=optionalAt+coff.SizeOfOptionalHeader;
        Check(optional.Magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC && optional.SizeOfImage && optional.SizeOfImage<=maxBytes &&
            optional.SizeOfHeaders && optional.SizeOfHeaders<=optional.SizeOfImage && optional.SizeOfHeaders<=file.size() &&
            optional.AddressOfEntryPoint<optional.SizeOfImage && optional.ImageBase &&
            Contains(optional.SizeOfHeaders,sectionAt,size_t(coff.NumberOfSections)*sizeof(IMAGE_SECTION_HEADER)),"invalid PE32 image/header sizes");
        Check(!(coff.Characteristics&IMAGE_FILE_RELOCS_STRIPPED) &&
            optional.NumberOfRvaAndSizes>IMAGE_DIRECTORY_ENTRY_BASERELOC,"fixture has no relocation directory");
        size=optional.SizeOfImage;preferredBase=optional.ImageBase;
        data=static_cast<uint8_t*>(VirtualAlloc(nullptr,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        Check(data!=nullptr,"private image allocation failed");
        const auto base=reinterpret_cast<uintptr_t>(data);
        Check(base<=UINT32_MAX-size && base!=preferredBase,"fixture must fit in Win32 and exercise relocation");
        std::memcpy(data,file.data(),optional.SizeOfHeaders);
        std::vector<std::pair<size_t,size_t>> regions{{0,optional.SizeOfHeaders}};
        for(unsigned i=0;i<coff.NumberOfSections;++i) {
            const auto section=Read<IMAGE_SECTION_HEADER>(file,sectionAt+i*sizeof(IMAGE_SECTION_HEADER));
            const size_t start=section.VirtualAddress,extent=std::max(section.Misc.VirtualSize,section.SizeOfRawData);
            Check(Contains(size,start,extent) && Contains(file.size(),section.PointerToRawData,section.SizeOfRawData),"section exceeds image/file bounds");
            for(const auto& region:regions)Check(!extent || start>=region.second || start+extent<=region.first,"overlapping image sections");
            if(extent)regions.emplace_back(start,start+extent);
            if(section.SizeOfRawData)std::memcpy(data+start,file.data()+section.PointerToRawData,section.SizeOfRawData);
        }
        const auto directory=optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        Check(directory.VirtualAddress && directory.Size && Contains(size,directory.VirtualAddress,directory.Size),"relocation directory exceeds image bounds");
        // Parse a stable copy: malformed relocations cannot rewrite the next
        // block header or relocation entry while it is being validated.
        const std::vector<uint8_t> relocationBytes(data+directory.VirtualAddress,data+directory.VirtualAddress+directory.Size);
        const uint32_t delta=static_cast<uint32_t>(base)-preferredBase;
        for(size_t cursor=0;cursor<relocationBytes.size();) {
            const auto block=Read<IMAGE_BASE_RELOCATION>(relocationBytes,cursor);
            Check(block.SizeOfBlock>=sizeof(block) && !(block.SizeOfBlock%sizeof(uint16_t)) &&
                Contains(relocationBytes.size(),cursor,block.SizeOfBlock),"invalid relocation block size");
            const auto end=cursor+block.SizeOfBlock;
            for(size_t entryAt=cursor+sizeof(block);entryAt<end;entryAt+=sizeof(uint16_t)) {
                const auto entry=Read<uint16_t>(relocationBytes,entryAt);
                const auto type=entry>>12;if(type==IMAGE_REL_BASED_ABSOLUTE)continue;
                Check(type==IMAGE_REL_BASED_HIGHLOW,"unsupported PE32 relocation type");
                const uint64_t target=uint64_t(block.VirtualAddress)+(entry&0xfff);
                Check(target<=size && sizeof(uint32_t)<=size-target,"relocation target exceeds image bounds");
                auto value=Read<uint32_t>(Bytes(),static_cast<size_t>(target));value+=delta;
                std::memcpy(data+static_cast<size_t>(target),&value,sizeof(value));++relocations;
            }
            cursor=end;
        }
        Check(relocations && Contains(size,0xcdca08,sizeof(uint32_t)),"fixture lacks relocated game data");
        DWORD previous{};Check(VirtualProtect(data,size,PAGE_READONLY,&previous)!=FALSE,"cannot make private image read-only");
    }
    bool ReadOnly() const {
        const auto end=reinterpret_cast<uintptr_t>(data)+size;
        for(auto at=reinterpret_cast<uintptr_t>(data);at<end;) {
            MEMORY_BASIC_INFORMATION region{};
            if(!VirtualQuery(reinterpret_cast<void*>(at),&region,sizeof(region)) || region.State!=MEM_COMMIT ||
                region.Type!=MEM_PRIVATE || region.Protect!=PAGE_READONLY)return false;
            const auto next=reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize;
            if(next<=at)return false;at=next;
        }
        return true;
    }
};
std::vector<uint8_t> Snapshot(const Image& image) {
    std::vector<uint8_t> bytes(image.size);SIZE_T copied{};
    Check(ReadProcessMemory(GetCurrentProcess(),image.data,bytes.data(),bytes.size(),&copied) && copied==bytes.size(),"cannot snapshot private image");
    return bytes;
}

struct Fixture {
    uint8_t* data{};
    static constexpr size_t registry=0x4000,manager=0x4400,camera=0x6000,target=0x6500,record=0x6700,node=0x6800,event=0x6900;
    explicit Fixture(bool high){data=static_cast<uint8_t*>(VirtualAlloc(high?reinterpret_cast<void*>(0x90000000u):nullptr,0x8000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));Check(data,"fixture allocation");if(high)Check(reinterpret_cast<uintptr_t>(data)>=0x80000000u,"fixture above2GiB");}
    ~Fixture(){VirtualFree(data,0,MEM_RELEASE);}
    uintptr_t At(size_t n=0)const{return reinterpret_cast<uintptr_t>(data+n);}
    void Word(size_t n,uintptr_t v){const auto word=static_cast<uint32_t>(v);memcpy(data+n,&word,4);}
    uint32_t Word(size_t n)const{uint32_t v{};memcpy(&v,data+n,4);return v;}
    void Initialize(uintptr_t base){
        memset(data,0,0x8000);Word(0x13a4,At(registry));Word(0x2cac,At(manager));
        Word(registry,base+0xcffb68);data[registry+0x48]=1;Word(registry+0x180,At(node));Word(registry+0x184,At(node));
        Word(node,At(registry+0x180));Word(node+4,At(registry+0x180));Word(node+8,base+0xbf8914);Word(node+12,At(record));
        Word(record,base+0x3ff4a0);Word(record+4,At());Word(manager+0xfb4,At(camera));Word(manager+0x70,At(target));
        Word(camera,base+0xcdd1f0);Word(camera+0x94,At(target));Word(camera+0x410,2);
        Word(target+0x30,2);Word(target+0x44,1);Word(target+0x4c,1);Word(target+0x50,0x12);Word(target+0x8c,8);
        Word(event,base+0xcdca04);Word(event+8,3024);Word(event+12,3360);Sizes(8192,2048);
    }
    void Sizes(unsigned w,unsigned h){Word(camera+0x408,w);Word(camera+0x40c,h);Word(target+0x3c,w);Word(target+0x40,h);}
};
Fixture* fixture{};unsigned calls{},mode{},nested{};bool expectedCanonical=true,owned=true;uintptr_t wrapper{};
int __fastcall Native(void* self,void*,void* event) {
    Check(reinterpret_cast<uintptr_t>(self)==fixture->At() && reinterpret_cast<uintptr_t>(event)==fixture->At(Fixture::event),"native receiver/event forwarded");
    ++calls;
    if(expectedCanonical)Check(fixture->Word(Fixture::camera+0x408)==2048 && fixture->Word(Fixture::camera+0x40c)==512,"native sees canonical base");
    else Check(fixture->Word(Fixture::camera+0x408)==8192 && fixture->Word(Fixture::camera+0x40c)==2048,"refused scope preserves actual");
    if(mode==1)fixture->Sizes(2048,512); // Successful factor-1 reset must not be restored.
    if(mode==2)fixture->Sizes(4096,1024); // Native updates camera/target normally.
    if(mode==3)fixture->Word(Fixture::manager+0xfb4,0); // Owner changed during call.
    if(mode==4)fixture->Word(0x2cac,0);
    if(mode==5)fixture->Word(Fixture::camera+0x94,0);
    if(mode==6 && !nested){++nested;const auto fn=reinterpret_cast<frontendvideo::TestCallback>(wrapper);Check(fn(self,event)==37,"nested native disposition");--nested;}
    return 37;
}
}
namespace grid2vr::sceneresolution {bool OwnsNativeResize(uintptr_t) noexcept{return owned;}}
int wmain(int argc,wchar_t** argv) {
    using namespace grid2vr;
    try {
        Check(argc==3 || argc==4,"usage: frontend_video_event_test gameexe owned-output [foreign|high]");
        const bool foreign=argc==4 && !wcscmp(argv[3],L"foreign"),high=argc==4 && !wcscmp(argv[3],L"high");
        Check(argc==3 || foreign || high,"unknown fixture mode");
        const std::filesystem::path folder=argv[2];std::filesystem::create_directories(folder);
        const auto file=ReadFile(argv[1],maxBytes);Image image;image.Load(file);const auto pristine=Snapshot(image);
        const auto base=reinterpret_cast<uintptr_t>(image.data);
        Check(frontendvideo::TestEventGuards(base),"all native event and video instruction guards");
        Fixture graph(high);fixture=&graph;graph.Initialize(base);
        // Broken membership cannot authorize a function-pointer write.
        graph.Word(Fixture::node+12,0);Check(!frontendvideo::Install(base,folder),"broken registry rejected");
        Check(graph.Word(Fixture::record)==base+0x3ff4a0 && Snapshot(image)==pristine,"refusal leaves image/record intact");
        graph.Word(Fixture::node+12,graph.At(Fixture::record));
        const std::vector<uint8_t> before(graph.data,graph.data+0x8000);
        Check(frontendvideo::Install(base,folder),"production discovery and attachment");
        wrapper=graph.Word(Fixture::record);Check(wrapper!=base+0x3ff4a0,"private callback replaced");
        for(size_t i=0;i<before.size();++i)if(i<Fixture::record || i>=Fixture::record+4)Check(graph.data[i]==before[i],"attachment changes only record function word");
        Check(image.ReadOnly() && Snapshot(image)==pristine,"full relocated native image unchanged after Install");
        frontendvideo::SetTestCallback(reinterpret_cast<frontendvideo::TestCallback>(Native));
        const auto callback=reinterpret_cast<frontendvideo::TestCallback>(wrapper);
        auto invoke=[&] {const auto prior=calls;Check(callback(reinterpret_cast<void*>(graph.At()),reinterpret_cast<void*>(graph.At(Fixture::event)))==37,"native disposition preserved");Check(calls==prior+(mode==6?2:1),"native called exactly once per event");};
        mode=0;invoke();Check(graph.Word(Fixture::camera+0x408)==8192 && graph.Word(Fixture::camera+0x40c)==2048,"skipped handler restores old actual size");
        mode=1;invoke();Check(graph.Word(Fixture::camera+0x408)==2048 && graph.Word(Fixture::camera+0x40c)==512,"high-to-factor1 native result retained");
        graph.Sizes(8192,2048);mode=2;invoke();Check(graph.Word(Fixture::camera+0x408)==4096 && graph.Word(Fixture::camera+0x40c)==1024,"changed native result retained");
        for(unsigned change:{3u,4u,5u}) {
            graph.Sizes(8192,2048);mode=change;invoke();Check(graph.Word(Fixture::camera+0x408)==2048,"changed route is not restored");
            graph.Word(0x2cac,graph.At(Fixture::manager));graph.Word(Fixture::manager+0xfb4,graph.At(Fixture::camera));graph.Word(Fixture::camera+0x94,graph.At(Fixture::target));
        }
        graph.Sizes(8192,2048);mode=6;invoke();Check(graph.Word(Fixture::camera+0x408)==8192,"nested no-op handler preserves outer restoration");mode=0;
        expectedCanonical=false;owned=false;invoke();owned=true;
        graph.Word(Fixture::event,0);invoke();graph.Word(Fixture::event,base+0xcdca04);
        graph.data[Fixture::registry+0x48]=0;invoke();graph.data[Fixture::registry+0x48]=1;
        std::thread peer([&]{invoke();});peer.join();
        Check(graph.Word(Fixture::camera+0x408)==8192,"off-owner-thread callback leaves dimensions unchanged");
        graph.Word(Fixture::record,base+0x3ff4a0);
        Check(frontendvideo::Install(base,folder) && graph.Word(Fixture::record)==wrapper,"native replacement reattaches after revalidation");
        Check(image.ReadOnly() && Snapshot(image)==pristine,"native image unchanged after all callback paths");
        if(foreign)graph.Word(Fixture::record,0x12345678);
        frontendvideo::Stop();Check(graph.Word(Fixture::record)==(foreign?0x12345678:base+0x3ff4a0),"Stop restores owned record only");
        invoke(); // A cached wrapper is still safe after Stop and forwards once.
        Check(graph.Word(Fixture::camera+0x408)==8192 && Snapshot(image)==pristine && image.ReadOnly(),"Stop and cached callback preserve native image and actual dimensions");
        std::ofstream report(folder/L"verification.json");report<<"{\"pass\":true,\"gameCodeExecuted\":false,\"fullImageBytesCompared\":"<<image.size<<",\"nativeCalls\":"<<calls<<",\"foreignStop\":"<<(foreign?"true":"false")<<",\"highAddressFixture\":"<<(high?"true":"false")<<"}\n";
        std::cout<<"PASS: private callback only, immutable relocated game image, native disposition, scoped camera dimensions, changed owners, reentrancy and owned Stop.\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
