#include "driving_controls.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Runs production Install/Stop against an owned, relocated PE image. Never
// calls a game function, its entry point, or the Windows image loader. Tick
// discovers/attaches owned heap fixtures; production DriverUpdate is exercised
// with owned callbacks and returns before any native query or polling call.
static_assert(sizeof(void*)==4,"This fixture exercises the Win32 game ABI.");
namespace {
constexpr size_t maxBytes=256u*1024*1024;
constexpr size_t driverSlot=0xd0ba08+3*sizeof(uint32_t);
constexpr size_t controllerGlobal=0xd96808;
void* ownedController{};
unsigned ownedStubCalls{};
bool ownedCallbacksValid=true;
void ControllerWord(void* object,size_t offset,uint32_t value) {
    std::memcpy(static_cast<uint8_t*>(object)+offset,&value,4);
}
uint32_t ControllerWord(void* object,size_t offset) {
    uint32_t value{};std::memcpy(&value,static_cast<uint8_t*>(object)+offset,4);return value;
}
bool OwnedUpdate(void* self,uint32_t mode,uint32_t selection) {
    ++ownedStubCalls;
    if(self!=ownedController) {ownedCallbacksValid=false;return false;}
    const auto observed=ControllerWord(self,4);
    ownedCallbacksValid=ownedCallbacksValid && observed==mode && ControllerWord(self,0x44)==selection;
    // Model only the documented native output copy. These are owned fixture
    // bytes, not instructions from the relocated, non-executable image.
    ControllerWord(self,0x34,observed);ControllerWord(self,0x24,9);ControllerWord(self,0x30,0);
    return true;
}
void __fastcall RequestedTransmission(void* self,void*,const double*) {OwnedUpdate(self,1,2);}
void __fastcall NativeTransmissionEdit(void* self,void*,const double*) {
    if(OwnedUpdate(self,1,2)) {ControllerWord(self,4,2);ControllerWord(self,0x44,1);}
}
void __fastcall StoppedTransmission(void* self,void*,const double*) {OwnedUpdate(self,2,1);}
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
        Check(relocations && Contains(size,driverSlot,sizeof(uint32_t)),"fixture lacks relocated game data");
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
struct Fixtures {
    uint8_t* data{};
    ~Fixtures(){if(data)VirtualFree(data,0,MEM_RELEASE);}
    uintptr_t At(size_t offset) const {return reinterpret_cast<uintptr_t>(data)+offset;}
    void Word(size_t offset,uintptr_t value) {const auto word=static_cast<uint32_t>(value);std::memcpy(data+offset,&word,4);}
    uint32_t Word(size_t offset) const {uint32_t value{};std::memcpy(&value,data+offset,4);return value;}
    void Build(Image& image) {
        data=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        Check(data!=nullptr,"cannot allocate owned input/controller graph");
        const auto base=reinterpret_cast<uintptr_t>(image.data);
        // Input2 with a one-node map tree and lock storage. No native library
        // operation is invoked, so no allocator/backend/function fixture exists.
        Word(0,base+0xd0bc84);Word(0xc,base+0xc3f364);Word(0x78,base+0xc56b50);
        for(const auto offset:{0x21c,0x224,0x240})Word(offset,At(0));
        Word(0x278,At(0x84));Word(0x84+0xb8,At(0x440));
        Word(0x84+0x30,At(0x300));Word(0x84+0x34,1);
        for(const auto offset:{0,4,8}) {Word(0x300+offset,At(0x380));Word(0x380+offset,At(0x300));}
        data[0x300+0x75]=1;
        // One local-player route, containing the native controller pair. The
        // first word is the driver pointer; the second native pair word is zero.
        Word(0x500,base+0xd1266c);Word(0x500+0x1a4,At(0x700));Word(0x700+4,At(0x740));
        Word(0x740,At(0x700));Word(0x740+4,At(0x700));Word(0x740+8,At(0x700));
        Word(0x740+12,At(0x780));Word(0x740+16,At(0x880));Word(0x780+0xb0,0);
        Word(0x880+8,At(0x900));Word(0x880+12,At(0x908));Word(0x900,At(0x980));
        Word(0x980,base+0xd0ba08);Word(0x980+4,4);Word(0x980+0x44,1);
        Word(0x980+0x10,At(0));Word(0x980+0x5c,0);data[0x980+0x40]=1;
        Check(Contains(image.size,controllerGlobal,4),"fixture has no controller global");
        DWORD old{},ignored{};
        Check(VirtualProtect(image.data+controllerGlobal,4,PAGE_READWRITE,&old)!=FALSE,"cannot initialize owned controller global");
        const auto parent=static_cast<uint32_t>(At(0x500));std::memcpy(image.data+controllerGlobal,&parent,4);
        Check(VirtualProtect(image.data+controllerGlobal,4,old,&ignored)!=FALSE,"cannot restore controller global protection");
    }
};
bool PrivateShadow(const Image& image,uint32_t table,size_t nativeTable,unsigned changedSlot) {
    if(table<4 || (table>=reinterpret_cast<uintptr_t>(image.data) && table<reinterpret_cast<uintptr_t>(image.data)+image.size))return false;
    std::array<uint32_t,14> copied{};SIZE_T read{};MEMORY_BASIC_INFORMATION region{};
    const auto prefix=uintptr_t(table)-4;
    if(!VirtualQuery(reinterpret_cast<void*>(prefix),&region,sizeof(region)) || region.Type!=MEM_PRIVATE ||
        region.State!=MEM_COMMIT || region.Protect!=PAGE_READONLY ||
        prefix+sizeof(copied)>reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize ||
        !ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(prefix),copied.data(),sizeof(copied),&read) || read!=sizeof(copied))return false;
    for(size_t i=0;i<copied.size();++i) {
        const auto original=Read<uint32_t>(image.Bytes(),nativeTable-4+i*4);
        if(i==changedSlot+1) {if(!copied[i] || copied[i]==original)return false;}
        else if(copied[i]!=original)return false;
    }
    return true;
}
bool Field(const std::string& json,const std::string& key,const std::string& expected) {
    // The production writer emits unique top-level fields used here. Match
    // complete JSON tokens, not a prefix (e.g. 0 must not match 01 or 0.5).
    auto at=json.find('"'+key+'"');if(at==json.npos)return false;at+=key.size()+2;
    const auto space=[&](){while(at<json.size() && std::isspace(static_cast<unsigned char>(json[at])))++at;};
    space();if(at==json.size() || json[at++]!=':')return false;space();
    if(json.compare(at,expected.size(),expected))return false;at+=expected.size();space();
    return at<json.size() && (json[at]==',' || json[at]=='}');
}
std::string Status(const std::filesystem::path& output) {
    const auto bytes=ReadFile(output/L"driving-controls-status.json",65536);return {bytes.begin(),bytes.end()};
}
std::string Claim(const std::filesystem::path& output) {
    const auto bytes=ReadFile(output/L"device-claim.json",65536);return {bytes.begin(),bytes.end()};
}
void Write(const std::filesystem::path& path,const std::string& text) {
    std::ofstream file(path,std::ios::binary|std::ios::trunc);file<<text;file.close();Check(bool(file),"fixture output write failed");
}
std::string Quoted(const std::string& text) {
    constexpr char hex[]="0123456789abcdef";
    std::string result="\"";
    for(const unsigned char c:text) {
        if(c<32) {result+="\\u00";result+=hex[c>>4];result+=hex[c&15];}
        else {if(c=='"' || c=='\\')result+='\\';result+=static_cast<char>(c);}
    }
    return result+'"';
}
struct Session {
    bool started{};
    ~Session(){if(started)try{grid2vr::drivingcontrols::Stop();}catch(...) {}}
    void Stop(){if(started){grid2vr::drivingcontrols::Stop();started=false;}}
};
}
int wmain(int argc,wchar_t** argv) {
    std::filesystem::path output;bool ownOutput{};
    try {
        Check(argc==3,"usage: driving_controls_image_test <game-exe> <new-owned-fixture-output>");
        // No game byte is ever executable, even if a future Install regression
        // accidentally tries to call into the fixture. Avoid a crash-dialog UI.
        SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
        DWORD dep{};BOOL permanent{};
        Check(GetProcessDEPPolicy(GetCurrentProcess(),&dep,&permanent)!=FALSE,"cannot inspect DEP policy");
        if(!(dep&PROCESS_DEP_ENABLE))Check(SetProcessDEPPolicy(PROCESS_DEP_ENABLE|PROCESS_DEP_DISABLE_ATL_THUNK_EMULATION)!=FALSE,"cannot enable DEP");
        const auto file=ReadFile(argv[1],maxBytes);Image image;image.Load(file);
        Check(image.ReadOnly(),"image contains writable or executable pages before Install");
        auto before=Snapshot(image);
        const auto originalDriverSlot=Read<uint32_t>(before,driverSlot);
        output=std::filesystem::absolute(argv[2]).lexically_normal();
        Check(output.has_filename() && std::filesystem::is_directory(output.parent_path()) &&
            std::filesystem::create_directory(output),"fixture output must be a new directory with an existing parent");
        ownOutput=true;
        Write(output/L"driving-controls.request","GRID2VR_DRIVING_CONTROLS_3\nsettings\thpattern_clutch\t0\n"
            "keyboard\tClutch\tvk_code_0x43\tuniDirPos\t0\t1\tkeyboard\n"
            "keyboard\tGear 1\tvk_code_0x31\tuniDirPos\t0\t1\tkeyboard\n");
        Write(output/L"driving-controls.xml","<driving-controls-image-test/>\n");
        // The direct-launch claim shares the private Input2 table. Install must
        // match every claim guard on the relocated image and only arm it.
        Write(output/L"device-claim.request","GRID2VR_DEVICE_CLAIM_1\nmode\tauto\nfamily\t{C24F046D-0000-0000-0000-504944564944}\n");
        Fixtures fixtures;Session session;session.started=true;
        grid2vr::drivingcontrols::Install(reinterpret_cast<uintptr_t>(image.data),output);
        const auto installed=Status(output);
        const auto claimInstalled=Claim(output);
        const bool claimArmed=Field(claimInstalled,"pending","true") && Field(claimInstalled,"reason","\"waiting_for_input_instance\"") &&
            Field(claimInstalled,"nativeCall","false") && Field(claimInstalled,"mode","\"auto\"");
        const auto afterInstall=Snapshot(image);
        const bool installReadOnly=image.ReadOnly();
        const bool installedReady=Field(installed,"failed","false") && Field(installed,"reason","\"waiting_for_input_instance\"") &&
            Field(installed,"installed","false") && Field(installed,"stopped","false") && Field(installed,"bindings","2");
        const bool installUnchanged=before==afterInstall;
        // Do not proceed beyond a failed pristine-install assertion. This is
        // what detects the original shared-image vtable patch regression.
        bool attached{},inputShadow{},driverShadow{},attachUnchanged{};unsigned ticks{};
        std::string attachment=installed;
        if(installedReady && installUnchanged && installReadOnly) {
            fixtures.Build(image);before=Snapshot(image);
            const auto start=GetTickCount64();
            while(ticks<4096 && GetTickCount64()-start<15000) {
                ++ticks;grid2vr::drivingcontrols::Tick();attachment=Status(output);
                if(Field(attachment,"driverHook","true") || Field(attachment,"failed","true"))break;
                Sleep(1);
            }
            attached=Field(attachment,"installed","true") && Field(attachment,"driverHook","true") && Field(attachment,"failed","false");
            inputShadow=PrivateShadow(image,fixtures.Word(0),0xd0bc84,1);
            driverShadow=PrivateShadow(image,fixtures.Word(0x980),0xd0ba08,3);
            attachUnchanged=before==Snapshot(image) && image.ReadOnly();
        }
        bool transmissionRestored{},transmissionOutput{},nativeEditPreserved{},stoppedNative{},updateUnchanged{};
        const bool invoke=attached && inputShadow && driverShadow && attachUnchanged;
        if(invoke) {
            ownedController=reinterpret_cast<void*>(fixtures.At(0x980));
            grid2vr::drivingcontrols::testing::DriverUpdate(ownedController,reinterpret_cast<void*>(RequestedTransmission));
            transmissionRestored=fixtures.Word(0x980+4)==4 && fixtures.Word(0x980+0x44)==1;
            transmissionOutput=fixtures.Word(0x980+0x34)==1;
            updateUnchanged=before==Snapshot(image) && image.ReadOnly();
            grid2vr::drivingcontrols::testing::DriverUpdate(ownedController,reinterpret_cast<void*>(NativeTransmissionEdit));
            nativeEditPreserved=fixtures.Word(0x980+4)==2 && fixtures.Word(0x980+0x44)==1 && fixtures.Word(0x980+0x34)==1;
            updateUnchanged=updateUnchanged && before==Snapshot(image) && image.ReadOnly();
        }
        session.Stop();
        if(invoke) {
            grid2vr::drivingcontrols::testing::DriverUpdate(ownedController,reinterpret_cast<void*>(StoppedTransmission));
            stoppedNative=fixtures.Word(0x980+4)==2 && fixtures.Word(0x980+0x44)==1 && fixtures.Word(0x980+0x34)==2;
            updateUnchanged=updateUnchanged && before==Snapshot(image) && image.ReadOnly();
            // Stop is idempotent and publishes the counters after the stopped
            // wrapper call. No service update or mapped game call has run.
            grid2vr::drivingcontrols::Stop();
        }
        const auto stopped=Status(output);
        const auto claimStopped=Claim(output);
        const bool claimIdle=Field(claimStopped,"nativeCall","false") && Field(claimStopped,"updates","0");
        const auto afterStop=Snapshot(image);
        const bool stoppedReady=Field(stopped,"failed","false") && Field(stopped,"stopped","true");
        bool nativeIdle=true;
        for(const auto* key:{"mapUpdates","stateWrites","driverOutputWrites"})
            nativeIdle=nativeIdle && Field(installed,key,"0") && Field(attachment,key,"0") && Field(stopped,key,"0");
        const bool driverCounters=ownedStubCalls==3 && ownedCallbacksValid && Field(installed,"driverUpdates","0") &&
            Field(attachment,"driverUpdates","0") && Field(stopped,"driverUpdates","3") &&
            Field(stopped,"driverEntered","3") && Field(stopped,"transmissionUpdates","2") &&
            Field(stopped,"driverOutputGate","\"stopped\"") && Field(stopped,"stale_sample","2");
        const bool stopUnchanged=before==afterStop;
        const bool slotUnchanged=originalDriverSlot==Read<uint32_t>(afterInstall,driverSlot) && originalDriverSlot==Read<uint32_t>(afterStop,driverSlot);
        const bool driverRestored=fixtures.data && fixtures.Word(0x980)==reinterpret_cast<uintptr_t>(image.data)+0xd0ba08 &&
            Field(stopped,"driverHook","false");
        const bool readOnly=installReadOnly && image.ReadOnly();
        const bool pass=installedReady && stoppedReady && nativeIdle && installUnchanged && attachUnchanged && stopUnchanged && slotUnchanged &&
            readOnly && attached && inputShadow && driverShadow && driverRestored && driverCounters &&
            transmissionRestored && transmissionOutput && nativeEditPreserved && stoppedNative && updateUnchanged && claimArmed && claimIdle;
        const auto flag=[](bool value){return value?"true":"false";};
        const std::string receipt=std::string("{\"schema\":2,\"pass\":")+flag(pass)+",\"productionInstallReached\":"+flag(installedReady)+
            ",\"productionStopSucceeded\":"+flag(stoppedReady)+",\"installImageUnchanged\":"+flag(installUnchanged)+
            ",\"stopImageUnchanged\":"+flag(stopUnchanged)+",\"driverSlotUnchanged\":"+flag(slotUnchanged)+
            ",\"privateImageReadOnly\":"+flag(readOnly)+",\"nativeInputCountersZero\":"+flag(nativeIdle)+
            ",\"driverUpdateCountersMatch\":"+flag(driverCounters)+",\"ownedStubCalls\":"+std::to_string(ownedStubCalls)+
            ",\"nativeGameCalls\":0,\"transmissionConfigurationRestored\":"+flag(transmissionRestored)+
            ",\"requestedTransmissionOutput\":"+flag(transmissionOutput)+",\"nativeTransmissionEditPreserved\":"+flag(nativeEditPreserved)+
            ",\"stoppedTransmissionNative\":"+flag(stoppedNative)+",\"updateImageUnchanged\":"+flag(updateUnchanged)+
            ",\"deviceClaimGuardsArmed\":"+flag(claimArmed)+",\"deviceClaimNotCalled\":"+flag(claimIdle)+
            ",\"attachmentImageUnchanged\":"+flag(attachUnchanged)+",\"privateInputShadow\":"+flag(inputShadow)+
            ",\"privateDriverShadow\":"+flag(driverShadow)+",\"driverHeapPointerRestored\":"+flag(driverRestored)+
            ",\"gameCodeExecuted\":false,\"imageBytes\":"+std::to_string(image.size)+",\"highLowRelocations\":"+std::to_string(image.relocations)+
            ",\"discoveryTicks\":"+std::to_string(ticks)+"}\n";
        Write(output/L"driving-controls-image-verification.json",receipt);std::cout<<receipt;
        if(!pass)std::cerr<<"Production Install/DriverUpdate/Stop did not preserve the private image or expected transmission behavior.\n";
        return pass?0:1;
    } catch(const std::exception& error) {
        const auto receipt=std::string("{\"schema\":2,\"pass\":false,\"error\":")+Quoted(error.what())+"}\n";
        if(ownOutput)try{Write(output/L"driving-controls-image-verification.json",receipt);}catch(...) {}
        std::cout<<receipt;return 1;
    }
}
