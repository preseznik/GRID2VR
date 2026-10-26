#include "profile_storage.h"
#include "game_executable.h"
#include "startup_movies.h"
#include "profile_setup.h"
#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

namespace grid2vr::profilestorage {
namespace {
constexpr std::uint64_t maxTotal=64ull*1024*1024;
constexpr std::uint32_t maxFile=32u*1024*1024;
constexpr std::size_t maxFiles=64;
struct Layout {
    std::uint32_t pe,timestamp,imageBytes,descriptor,originalThunk,dllName,firstThunk,storageIat,userIat,folderCall,storageFolderCall;
    std::array<std::uintptr_t,6> storageCalls;
    const char* hash;
};
constexpr Layout avxLayout{0x158,0x524b8944,0x100e000,0xd89394,0xd89d28,0xd89ff0,0xb16818,0xb16824,0xb1681c,0x146c99,0x7f4b77,
    {0x48b9a,0x22d435,0x50763b,0x56aae0,0x6316fc,0x7d948c},
    "519cd994d2ff92a2a7a2a784856854183f53e359b83f8b9563cc09a0ae91e502"};
constexpr Layout regularLayout{0x148,0x524b87ac,0xffd000,0xd7757c,0xd77f14,0xd781dc,0xb0481c,0xb04828,0xb04820,0x604249,0x174af7,
    {0x1845fa,0x1d102b,0x2206c0,0x2573ec,0x27eb25,0x5ff05c},
    "f92c2e1134b5d6d61404131224b41c182089bf563d28731eef0faa7d8096ab4c"};
bool Temporary(std::string_view name) {return name.starts_with(".grid2vr-write-") && name.ends_with(".tmp");}
bool Guid(std::string_view value) {
    return value.size()==32 && std::all_of(value.begin(),value.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
std::uint32_t Word(const std::uint8_t* at) {std::uint32_t result{};std::memcpy(&result,at,4);return result;}
const Layout* HeaderLayout(const std::uint8_t* image,std::size_t bytes) {
    if(!image||bytes<0x200||image[0]!='M'||image[1]!='Z')return nullptr;
    const auto pe=Word(image+0x3c);const auto layout=pe==avxLayout.pe?&avxLayout:(pe==regularLayout.pe?&regularLayout:nullptr);
    if(!layout||bytes<layout->imageBytes||Word(image+pe)!=IMAGE_NT_SIGNATURE||
       Word(image+pe+8)!=layout->timestamp||Word(image+pe+24+56)!=layout->imageBytes)return nullptr;
    return layout;
}
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    Handle()=default;
    explicit Handle(HANDLE v):value(v){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    Handle(Handle&& other) noexcept:value(other.value){other.value=INVALID_HANDLE_VALUE;}
    bool Valid()const{return value!=INVALID_HANDLE_VALUE;}
};
bool Info(HANDLE handle, BY_HANDLE_FILE_INFORMATION& info, bool directory) {
    return GetFileInformationByHandle(handle,&info) &&
        !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) &&
        bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)==directory &&
        (directory || info.nNumberOfLinks==1);
}
Handle OpenFile(const std::filesystem::path& path,DWORD access) {
    return Handle(CreateFileW(path.c_str(),access,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
}
bool Scan(const std::filesystem::path& path, unsigned depth, std::size_t& files,
          std::uint64_t& bytes,unsigned& directories,bool remote) {
    if(depth>4 || ++directories>16)return false;
    for(const auto& entry:std::filesystem::directory_iterator(path)) {
        const auto attrs=GetFileAttributesW(entry.path().c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES || (attrs&FILE_ATTRIBUTE_REPARSE_POINT))return false;
        if(attrs&FILE_ATTRIBUTE_DIRECTORY) {
            if(remote || !Scan(entry.path(),depth+1,files,bytes,directories,false))return false;
        } else {
            if(remote && !ValidFileName(entry.path().filename().string()))return false;
            auto file=OpenFile(entry.path(),FILE_READ_ATTRIBUTES);BY_HANDLE_FILE_INFORMATION info{};
            if(!file.Valid() || !Info(file.value,info,false) || ++files>maxFiles || info.nFileSizeHigh)return false;
            bytes+=info.nFileSizeLow;if(bytes>maxTotal)return false;
        }
    }
    return true;
}
}
bool ValidFileName(std::string_view value) {
    if(value.empty() || value.size()>180 || value=="." || value==".." || value.back()=='.' || value.back()==' ')return false;
    for(const unsigned char c:value)if(c<32 || c>126 || std::strchr("\\/:*?\"<>|",c))return false;
    auto stem=std::string(value.substr(0,value.find('.')));
    for(auto& c:stem)if(c>='a'&&c<='z')c-=32;
    if(stem=="CON"||stem=="PRN"||stem=="AUX"||stem=="NUL")return false;
    if(stem.size()==4 && (stem.starts_with("COM")||stem.starts_with("LPT")) && stem[3]>='1' && stem[3]<='9')return false;
    return true;
}
bool ParseRequest(std::string_view text,Request& result) {
    if(text.size()>8192 || text.find('\0')!=std::string_view::npos)return false;
    std::array<std::string_view,4> lines{};
    for(auto& line:lines){const auto end=text.find('\n');line=text.substr(0,end);if(line.ends_with('\r'))line.remove_suffix(1);
        if(end==std::string_view::npos)text={};else text.remove_prefix(end+1);}
    if(!text.empty() || lines[0]!="GRID2VR_PROFILE_STORAGE_V1" || !Guid(lines[2]))return false;
    std::uint32_t account{};const auto parsed=std::from_chars(lines[3].data(),lines[3].data()+lines[3].size(),account);
    if(parsed.ec!=std::errc{} || parsed.ptr!=lines[3].data()+lines[3].size() || !account)return false;
    try {
        const auto raw=std::filesystem::path(std::u8string(lines[1].begin(),lines[1].end()));
        const auto root=raw.lexically_normal();const auto value=root.native();
        if(value.size()<4 || value.size()>220 || value[1]!=L':' || value[2]!=L'\\' || !root.is_absolute() ||
           root!=raw || root.filename()!=L"remote" || root.parent_path().filename().string()!=lines[2] ||
           root.parent_path().parent_path().filename()!=L"profiles" ||
           root.parent_path().parent_path().parent_path().filename()!=L"artifacts")return false;
        result={root,std::string(lines[2]),account};return true;
    }catch(...){return false;}
}
bool UsesManagedUserFolder(std::uintptr_t callerRva){
    return callerRva==avxLayout.folderCall+6||callerRva==avxLayout.storageFolderCall+6||
        callerRva==regularLayout.folderCall+6||callerRva==regularLayout.storageFolderCall+6;
}
bool ValidImageLayout(const std::uint8_t* image,std::size_t bytes,std::uintptr_t relocatedBase) {
    const auto layout=HeaderLayout(image,bytes);if(!layout||relocatedBase>UINT32_MAX-layout->imageBytes)return false;
    // Import descriptor, names and all six storage call instructions are fixed
    // to the selected shipped executable. File hashes are checked before returning an object.
    if(Word(image+layout->descriptor)!=layout->originalThunk || Word(image+layout->descriptor+12)!=layout->dllName ||
       Word(image+layout->descriptor+16)!=layout->firstThunk || std::memcmp(image+layout->dllName,"steam_api.dll",13))return false;
    for(const auto at:layout->storageCalls)if(image[at]!=0xff || image[at+1]!=0x15 || Word(image+at+2)!=relocatedBase+layout->storageIat)return false;
    constexpr std::uint8_t tail[]{0x85,0xc0,0x74,0x6a,0x8b,0x10,0x8b,0x52,0x18,0x68,0x04,0x01,0,0};
    const auto at=layout->folderCall;
    if(image[at]!=0xff || image[at+1]!=0x15 || Word(image+at+2)!=relocatedBase+layout->userIat ||
        std::memcmp(image+at+6,tail,sizeof(tail)))return false;
    // EGO's cloud-key constructor must use the same local root as the save
    // directory builder. It strips this prefix before encoding '\\' as '@'.
    constexpr std::uint8_t constructorTail[]{0x8b,0x10,0x8b,0x52,0x18,0x68,0x04,0x01,0,0,0x8d,0x4e,0x18,0x51,0x8b,0xc8,0xff,0xd2};
    const auto constructor=layout->storageFolderCall;
    return image[constructor]==0xff && image[constructor+1]==0x15 && Word(image+constructor+2)==relocatedBase+layout->userIat &&
        !std::memcmp(image+constructor+6,constructorTail,sizeof(constructorTail));
}
namespace {
bool Replace(void** slot,void* expected,void* replacement) {
    DWORD old{};if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old))return false;
    const bool replaced=InterlockedCompareExchangePointer(slot,replacement,expected)==expected;
    DWORD ignored{};return VirtualProtect(slot,sizeof(void*),old,&ignored)&&replaced;
}
}
bool InstallImportRoutes(std::uint8_t* image,std::size_t bytes,std::uintptr_t relocatedBase,
    void* expectedStorage,void* expectedUser,void* storageReplacement,void* userReplacement) {
    if(!ValidImageLayout(image,bytes,relocatedBase)||!expectedStorage||!expectedUser||!storageReplacement||!userReplacement)return false;
    const auto layout=HeaderLayout(image,bytes);
    auto remoteSlot=reinterpret_cast<void**>(image+layout->storageIat),userSlot=reinterpret_cast<void**>(image+layout->userIat);
    if(*remoteSlot!=expectedStorage||*userSlot!=expectedUser)return false;
    if(!Replace(remoteSlot,expectedStorage,storageReplacement))return false;
    if(Replace(userSlot,expectedUser,userReplacement))return true;
    Replace(remoteSlot,storageReplacement,expectedStorage);return false;
}
struct Store::State {
    Request request;
    std::filesystem::path local;
    std::vector<Handle> leases;
    std::mutex mutex;
    bool ready{};
    unsigned serial{};
    std::string enumeratedName;
    WriteFailure writeFailure;
    bool FailWrite(const char* name,std::int32_t bytes,const char* category,DWORD error=0) {
        if(writeFailure.count<UINT32_MAX)++writeFailure.count;
        strcpy_s(writeFailure.category,category);writeFailure.requestedBytes=bytes;writeFailure.win32Error=error;
        constexpr char digits[]="0123456789abcdef";
        const auto length=name?strnlen_s(name,261):0;const auto copied=std::min<std::size_t>(length,260);
        writeFailure.keyTruncated=length>260;
        for(std::size_t i=0;i<copied;++i){const auto value=static_cast<unsigned char>(name[i]);
            writeFailure.keyHex[i*2]=digits[value>>4];writeFailure.keyHex[i*2+1]=digits[value&15];}
        writeFailure.keyHex[copied*2]=0;return false;
    }
    bool Pin(const std::filesystem::path& directory) {
        Handle handle(CreateFileW(directory.c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
            OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if(!handle.Valid() || !Info(handle.value,info,true))return false;
        leases.push_back(std::move(handle));return true;
    }
    bool Totals(std::size_t& files,std::uint64_t& bytes) {
        unsigned remoteDirs{},localDirs{};return Scan(request.remote,0,files,bytes,remoteDirs,true) && Scan(local,0,files,bytes,localDirs,false);
    }
    std::filesystem::path Path(const char* name) const {
        if(!name || strnlen_s(name,181)>180 || !ValidFileName(name) || Temporary(name))return {};
        return request.remote/name;
    }
    std::vector<std::pair<std::string,std::int32_t>> Files() {
        std::vector<std::pair<std::string,std::int32_t>> result;
        for(const auto& entry:std::filesystem::directory_iterator(request.remote)) {
            const auto name=entry.path().filename().string();if(Temporary(name))continue;
            if(!ValidFileName(name))throw std::runtime_error("profile filename");
            auto file=OpenFile(entry.path(),FILE_READ_ATTRIBUTES);BY_HANDLE_FILE_INFORMATION info{};
            if(!file.Valid() || !Info(file.value,info,false) || info.nFileSizeHigh || info.nFileSizeLow>maxFile || result.size()>=maxFiles)
                throw std::runtime_error("profile entry");
            result.emplace_back(name,static_cast<std::int32_t>(info.nFileSizeLow));
        }
        std::sort(result.begin(),result.end());return result;
    }
};
Store::Store(const Request& request):state(std::make_unique<State>()) {
    try {
        Request parsed;const auto utf8=request.remote.u8string();
        const std::string text="GRID2VR_PROFILE_STORAGE_V1\n"+std::string(utf8.begin(),utf8.end())+"\n"+request.id+"\n"+std::to_string(request.account)+"\n";
        if(!ParseRequest(text,parsed))return;
        state->request=parsed;state->local=parsed.remote.parent_path()/L"local";
        std::filesystem::path prefix=parsed.remote.root_path();if(!state->Pin(prefix))return;
        for(const auto& component:parsed.remote.relative_path()){prefix/=component;if(!state->Pin(prefix))return;}
        std::filesystem::create_directory(state->local);
        if(!state->Pin(state->local))return;
        std::filesystem::create_directory(state->local/L"savegame");
        if(!state->Pin(state->local/L"savegame"))return;
        std::size_t files{};std::uint64_t bytes{};state->ready=state->Totals(files,bytes);
    }catch(...){state->ready=false;}
}
Store::~Store()=default;
bool Store::Ready()const{return state->ready;}
bool Store::Write(const char* name,const void* data,std::int32_t bytes) {
    std::lock_guard lock(state->mutex);
    if(!Ready())return state->FailWrite(name,bytes,"store_unavailable");
    if(bytes<0 || bytes>maxFile || (!data&&bytes))return state->FailWrite(name,bytes,"invalid_write_arguments");
    try {
        const auto path=state->Path(name);if(path.empty())return state->FailWrite(name,bytes,"invalid_filename");
        std::size_t files{};std::uint64_t used{};if(!state->Totals(files,used))return state->FailWrite(name,bytes,"unsafe_profile_tree");
        const auto attrs=GetFileAttributesW(path.c_str());std::uint64_t previous{};
        if(attrs!=INVALID_FILE_ATTRIBUTES){auto file=OpenFile(path,FILE_READ_ATTRIBUTES);BY_HANDLE_FILE_INFORMATION info{};
            if(!file.Valid())return state->FailWrite(name,bytes,"target_open_failed",GetLastError());
            if(!Info(file.value,info,false)||info.nFileSizeHigh)return state->FailWrite(name,bytes,"unsafe_target");previous=info.nFileSizeLow;}
        else {const auto error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND)return state->FailWrite(name,bytes,"target_attributes_failed",error);
            if(files>=maxFiles)return state->FailWrite(name,bytes,"file_count_limit");}
        // Include the temporary file so even the atomic replacement peak is bounded.
        if(used-previous+bytes>maxTotal || used+bytes>maxTotal)return state->FailWrite(name,bytes,"byte_limit");
        const auto temp=state->request.remote/(L".grid2vr-write-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++state->serial)+L".tmp");
        const char* failure{};DWORD error{};
        {Handle file(CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr));
            DWORD written{};
            if(!file.Valid())failure="temporary_create_failed";
            else if(!WriteFile(file.value,data,static_cast<DWORD>(bytes),&written,nullptr))failure="temporary_write_failed";
            else if(written!=static_cast<DWORD>(bytes)){failure="short_write";SetLastError(ERROR_WRITE_FAULT);}
            else if(!FlushFileBuffers(file.value))failure="temporary_flush_failed";
            if(failure)error=GetLastError();}
        if(!failure && !MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
            failure="atomic_replace_failed";error=GetLastError();}
        if(failure){DeleteFileW(temp.c_str());return state->FailWrite(name,bytes,failure,error);}return true;
    }catch(const std::filesystem::filesystem_error& error){return state->FailWrite(name,bytes,"filesystem_exception",static_cast<DWORD>(error.code().value()));}
    catch(...){return state->FailWrite(name,bytes,"write_exception");}
}
WriteFailure Store::LastWriteFailure()const {std::lock_guard lock(state->mutex);return state->writeFailure;}
std::int32_t Store::Read(const char* name,void* data,std::int32_t bytes) {
    if(!Ready()||bytes<0||bytes>maxFile||(!data&&bytes))return 0;
    std::lock_guard lock(state->mutex);
    try{const auto path=state->Path(name);if(path.empty())return 0;
        auto file=OpenFile(path,GENERIC_READ);BY_HANDLE_FILE_INFORMATION info{};DWORD read{};
        if(!file.Valid()||!Info(file.value,info,false)||info.nFileSizeHigh||info.nFileSizeLow>maxFile)return 0;
        return ReadFile(file.value,data,bytes,&read,nullptr)?static_cast<std::int32_t>(read):0;
    }catch(...){return 0;}
}
bool Store::Delete(const char* name) {
    if(!Ready())return false;std::lock_guard lock(state->mutex);
    try{const auto path=state->Path(name);if(path.empty())return false;
        auto file=OpenFile(path,DELETE|FILE_READ_ATTRIBUTES);BY_HANDLE_FILE_INFORMATION info{};FILE_DISPOSITION_INFO dispose{TRUE};
        return file.Valid()&&Info(file.value,info,false)&&SetFileInformationByHandle(file.value,FileDispositionInfo,&dispose,sizeof(dispose));
    }catch(...){return false;}
}
std::int32_t Store::Size(const char* name) {
    if(!Ready())return 0;std::lock_guard lock(state->mutex);
    try{const auto path=state->Path(name);if(path.empty())return 0;auto file=OpenFile(path,FILE_READ_ATTRIBUTES);BY_HANDLE_FILE_INFORMATION info{};
        return file.Valid()&&Info(file.value,info,false)&&!info.nFileSizeHigh&&info.nFileSizeLow<=maxFile?static_cast<std::int32_t>(info.nFileSizeLow):0;
    }catch(...){return 0;}
}
bool Store::Exists(const char* name) {
    if(!Ready())return false;std::lock_guard lock(state->mutex);
    try{const auto path=state->Path(name);if(path.empty())return false;auto file=OpenFile(path,FILE_READ_ATTRIBUTES);BY_HANDLE_FILE_INFORMATION info{};
        return file.Valid()&&Info(file.value,info,false)&&!info.nFileSizeHigh&&info.nFileSizeLow<=maxFile;
    }catch(...){return false;}
}
std::int32_t Store::Count() {
    if(!Ready())return 0;std::lock_guard lock(state->mutex);
    try{return static_cast<std::int32_t>(state->Files().size());}catch(...){return 0;}
}
const char* Store::Name(std::int32_t index,std::int32_t* size) {
    if(size)*size=0;if(!Ready()||index<0)return nullptr;std::lock_guard lock(state->mutex);
    try{const auto files=state->Files();if(static_cast<std::size_t>(index)>=files.size())return nullptr;
        state->enumeratedName=files[index].first;if(size)*size=files[index].second;return state->enumeratedName.c_str();
    }catch(...){return nullptr;}
}
bool Store::UserFolder(char* output,std::int32_t capacity) {
    if(!Ready()||!output||capacity<1)return false;
    BOOL substituted{};const auto path=state->local.native();
    return WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,path.c_str(),-1,output,capacity,nullptr,&substituted)>0&&!substituted;
}
}

#ifndef GRID2VR_PROFILE_STORAGE_TEST
namespace {
using namespace grid2vr::profilestorage;
// POD only: attach runs before the game entry point, under the loader lock.
wchar_t outputFolder[1024]{},requestFile[1200]{};
std::uintptr_t hostBase{},steamBase{};
const Layout* hostLayout{};
void* originalUser{};
Store* localStore{};
void* storageSlots[64]{};void* userSlots[32]{};
struct Object{void** slots;};Object storageObject{storageSlots},userObject{userSlots};
INIT_ONCE once=INIT_ONCE_STATIC_INIT;
volatile LONG writes{},reads{},deletes{};
volatile LONG counts{},names{},folders{};
char managedId[33]{};
std::uint32_t observedPe{},observedTimestamp{},observedImageSize{},observedRemote{},observedUser{};
char hostName[96]{};
void ObserveAttach() {
    wchar_t path[1024]{};GetModuleFileNameW(nullptr,path,1024);const auto slash=wcsrchr(path,L'\\');const auto name=slash?slash+1:path;
    unsigned i{};for(;name[i]&&i<sizeof(hostName)-1;++i)hostName[i]=(name[i]>=32&&name[i]<127&&name[i]!='"'&&name[i]!='\\')?static_cast<char>(name[i]):'_';
    hostName[i]=0;
    const auto read=[](std::uintptr_t at,std::uint32_t& value){SIZE_T bytes{};ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(at),&value,4,&bytes);};
    if(hostBase){read(hostBase+0x3c,observedPe);if(observedPe<0x1000){read(hostBase+observedPe+8,observedTimestamp);read(hostBase+observedPe+24+56,observedImageSize);}
        if(hostLayout){read(hostBase+hostLayout->storageIat,observedRemote);read(hostBase+hostLayout->userIat,observedUser);}}
}
void Receipt(const char* phase,const char* reason="") {
    if(!outputFolder[0])return;
    wchar_t path[1200]{};swprintf_s(path,L"%s\\profile-storage.json",outputFolder);
    const auto failure=localStore?localStore->LastWriteFailure():WriteFailure{};
    if(!reason[0]&&failure.count)reason="write_failed";
    char text[2048]{};const int size=sprintf_s(text,"{\"schema\":1,\"phase\":\"%s\",\"reason\":\"%s\",\"profileId\":\"%s\",\"writes\":%ld,\"reads\":%ld,\"deletes\":%ld,\"countCalls\":%ld,\"enumerationCalls\":%ld,\"localFolderCalls\":%ld,\"steamStorageCalls\":0,\"writeFailures\":%u,\"lastWriteFailure\":{\"category\":\"%s\",\"keyHex\":\"%s\",\"keyTruncated\":%s,\"requestedBytes\":%d,\"win32Error\":%u},\"attach\":{\"host\":\"%s\",\"hostBase\":%u,\"steamBase\":%u,\"peOffset\":%u,\"timestamp\":%u,\"imageSize\":%u,\"storageImport\":%u,\"userImport\":%u}}\n",phase,reason,managedId,writes,reads,deletes,counts,names,folders,failure.count,failure.category,failure.keyHex,failure.keyTruncated?"true":"false",failure.requestedBytes,failure.win32Error,hostName,static_cast<unsigned>(hostBase),static_cast<unsigned>(steamBase),observedPe,observedTimestamp,observedImageSize,observedRemote,observedUser);
    HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE){DWORD done{};WriteFile(file,text,size,&done,nullptr);CloseHandle(file);}
}
[[noreturn]] void ExitRefused(){TerminateProcess(GetCurrentProcess(),0x47525046);__fastfail(FAST_FAIL_FATAL_APP_EXIT);}
[[noreturn]] void Refuse(const char* reason){Receipt("refused",reason);ExitRefused();}
bool ReadSmall(const wchar_t* path,char* buffer,DWORD capacity,DWORD& bytes) {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    BY_HANDLE_FILE_INFORMATION info{};const bool valid=GetFileInformationByHandle(file,&info)&&
        !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&info.nNumberOfLinks==1&&
        !info.nFileSizeHigh&&info.nFileSizeLow<capacity&&ReadFile(file,buffer,capacity-1,&bytes,nullptr)&&bytes==info.nFileSizeLow;
    CloseHandle(file);if(valid)buffer[bytes]=0;return valid;
}
std::string HashFile(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    if(!input||BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return {};
    if(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0){BCryptCloseAlgorithmProvider(algorithm,0);return {};}
    std::array<char,65536> block{};bool valid=true;
    while(input){input.read(block.data(),block.size());if(BCryptHashData(hash,reinterpret_cast<PUCHAR>(block.data()),static_cast<ULONG>(input.gcount()),0)<0)valid=false;}
    std::array<unsigned char,32> digest{};valid=valid&&!input.bad()&&BCryptFinishHash(hash,digest.data(),32,0)>=0;
    BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);char text[65]{};
    if(valid)for(unsigned i=0;i<32;++i)sprintf_s(text+i*2,3,"%02x",digest[i]);return text;
}
std::uint32_t Account() {
    void* user=reinterpret_cast<void*(__cdecl*)()>(originalUser)();if(!user)return 0;
    auto slots=*static_cast<void***>(user);std::uint64_t id{};
    // The game's own call at RVA137711 supplies this hidden return buffer.
    const auto getId=reinterpret_cast<std::uint64_t*(__thiscall*)(void*,std::uint64_t*)>(slots[2]);
    if(getId(user,&id)!=&id)return 0;return static_cast<std::uint32_t>(id);
}
bool __fastcall Write(void*,void*,const char* name,const void* data,std::int32_t bytes){const bool ok=localStore->Write(name,data,bytes);if(ok)InterlockedIncrement(&writes);Receipt("active",ok?"":"write_failed");return ok;}
std::int32_t __fastcall Read(void*,void*,const char* name,void* data,std::int32_t bytes){const auto result=localStore->Read(name,data,bytes);InterlockedIncrement(&reads);Receipt("active");return result;}
bool __fastcall Delete(void*,void*,const char* name){const bool ok=localStore->Delete(name);if(ok)InterlockedIncrement(&deletes);Receipt("active");return ok;}
bool __fastcall Exists(void*,void*,const char* name){return localStore->Exists(name);}
std::int32_t __fastcall Size(void*,void*,const char* name){return localStore->Size(name);}
std::int32_t __fastcall Count(void*,void*){const auto result=localStore->Count();InterlockedIncrement(&counts);Receipt("active");return result;}
const char* __fastcall Name(void*,void*,std::int32_t index,std::int32_t* size){const auto result=localStore->Name(index,size);InterlockedIncrement(&names);Receipt("active");return result;}
bool __fastcall Folder(void*,void*,char* output,std::int32_t size){if(!localStore->UserFolder(output,size))Refuse("local_folder_unrepresentable");InterlockedIncrement(&folders);Receipt("active");return true;}
[[noreturn]] void __fastcall Unsupported(void*,void*){Refuse("unsupported_storage_method");}
BOOL CALLBACK Initialize(PINIT_ONCE,void*,void**) {
    try{
        wchar_t host[32768]{},steam[32768]{};GetModuleFileNameW(nullptr,host,32768);GetModuleFileNameW(reinterpret_cast<HMODULE>(steamBase),steam,32768);
        if(!hostLayout || grid2vr::gameexe::Validate(host,hostLayout==&avxLayout).empty() ||
           HashFile(steam)!="abfedd473b3f4a9597bbdc90d20f4b6f696bb2ebb937a03177461df695430ad6")Refuse("unsupported_binary");
        char text[8193]{};DWORD size{};Request request;
        if(!ReadSmall(requestFile,text,sizeof(text),size)||!ParseRequest(std::string_view(text,size),request))Refuse("invalid_request");
        strcpy_s(managedId,request.id.c_str());
        const auto expectedArtifacts=std::filesystem::path(outputFolder).lexically_normal().parent_path();
        const auto actualArtifacts=request.remote.parent_path().parent_path().parent_path();
        if(_wcsicmp(expectedArtifacts.c_str(),actualArtifacts.c_str()))Refuse("profile_workspace_mismatch");
        if(Account()!=request.account)Refuse("steam_account_mismatch");
        localStore=new Store(request);if(!localStore->Ready())Refuse("unsafe_profile_folder");
        char localPath[260]{};if(!localStore->UserFolder(localPath,sizeof(localPath)))Refuse("local_folder_unrepresentable");
        for(auto& slot:storageSlots)slot=reinterpret_cast<void*>(Unsupported);
        for(auto& slot:userSlots)slot=reinterpret_cast<void*>(Unsupported);
        storageSlots[0]=reinterpret_cast<void*>(Write);storageSlots[1]=reinterpret_cast<void*>(Read);
        storageSlots[3]=reinterpret_cast<void*>(Delete);storageSlots[10]=reinterpret_cast<void*>(Exists);
        storageSlots[12]=reinterpret_cast<void*>(Size);storageSlots[15]=reinterpret_cast<void*>(Count);storageSlots[16]=reinterpret_cast<void*>(Name);
        userSlots[6]=reinterpret_cast<void*>(Folder);
        // Diagnostic observation is separately requested; normal managed
        // careers and all current-career launches leave native states untouched.
        wchar_t auditPath[1200]{};swprintf_s(auditPath,L"%s\\profile-setup.request",outputFolder);
        char audit[128]{};DWORD auditBytes{};
        if(hostLayout==&avxLayout&&ReadSmall(auditPath,audit,sizeof(audit),auditBytes)&&
           (std::string_view(audit,auditBytes)=="GRID2VR_PROFILE_SETUP_AUDIT_V1\n"||
            std::string_view(audit,auditBytes)=="GRID2VR_PROFILE_SETUP_AUDIT_V1\r\n"))
            grid2vr::profilesetup::Install(reinterpret_cast<std::uint8_t*>(hostBase),hostLayout->imageBytes,outputFolder,managedId);
        Receipt("active");return TRUE;
    }catch(...){Refuse("initialization_exception");}
}
void Ensure(){if(!InitOnceExecuteOnce(&once,Initialize,nullptr,nullptr))Refuse("initialization_failed");}
void* __cdecl StorageGetter(){Ensure();return &storageObject;}
void* __cdecl UserGetter(){
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress())-hostBase;
    if(!hostLayout || (caller!=hostLayout->folderCall+6&&caller!=hostLayout->storageFolderCall+6))return reinterpret_cast<void*(__cdecl*)()>(originalUser)();
    Ensure();return &userObject;
}
bool AttachStorage() {
    bool markerFound{};
    {
        wchar_t host[1024]{};if(!GetModuleFileNameW(nullptr,host,1024))return true;
        auto slash=wcsrchr(host,L'\\');if(!slash)return true;slash[1]=0;
        wchar_t marker[1200]{};swprintf_s(marker,L"%sGRID2VR.diagnostic-session",host);
        char text[4096]{};DWORD bytes{};
        if(ReadSmall(marker,text,sizeof(text),bytes)){
            const char header[]="GRID2VR_DIAGNOSTIC_V1";const auto line=strchr(text,'\n');
            if(line&&!strncmp(text,header,sizeof(header)-1)&&
               (line-text==sizeof(header)-1 || (line-text==sizeof(header)&&line[-1]=='\r'))){
                auto end=strchr(line+1,'\n');if(end)*end=0;auto last=strchr(line+1,'\r');if(last)*last=0;
                markerFound=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,line+1,-1,outputFolder,1024)>0;
            }
        }
    }
    // The transaction's adjacent marker wins over inherited diagnostic variables.
    if(!markerFound){
        wchar_t flag[8]{};const auto marked=GetEnvironmentVariableW(L"GRID2VR_DIAGNOSTICS",flag,8);
        if(!marked||marked>=8||wcscmp(flag,L"1"))return true;
        const auto count=GetEnvironmentVariableW(L"GRID2VR_OUTPUT",outputFolder,1024);if(!count||count>=1024)return true;
    }
    if(outputFolder[1]!=L':' || outputFolder[2]!=L'\\')return true;
    swprintf_s(requestFile,L"%s\\profile-storage.request",outputFolder);
    const auto attrs=GetFileAttributesW(requestFile);
    const auto requestError=GetLastError();
    if(attrs==INVALID_FILE_ATTRIBUTES && (requestError==ERROR_FILE_NOT_FOUND || requestError==ERROR_PATH_NOT_FOUND))return true;
    if(attrs==INVALID_FILE_ATTRIBUTES || (attrs&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){Receipt("refused","invalid_request_file");return false;}
    hostBase=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));steamBase=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"steam_api.dll"));
    const auto image=reinterpret_cast<const std::uint8_t*>(hostBase);
    hostLayout=HeaderLayout(image,0x100e000);
    ObserveAttach();
    if(!hostBase)Refuse("host_module_unavailable");
    if(!steamBase)Refuse("steam_module_unavailable_at_attach");
    if(!ValidImageLayout(image,0x100e000,hostBase))Refuse("unsupported_import_layout");
    const auto user=reinterpret_cast<void*>(steamBase+0x2ba0),remote=reinterpret_cast<void*>(steamBase+0x2c20);
    auto userSlot=reinterpret_cast<void**>(hostBase+hostLayout->userIat),remoteSlot=reinterpret_cast<void**>(hostBase+hostLayout->storageIat);
    if(*userSlot!=user||*remoteSlot!=remote){Receipt("refused","foreign_storage_hook");return false;}
    originalUser=user;
    if(!InstallImportRoutes(reinterpret_cast<std::uint8_t*>(hostBase),0x100e000,hostBase,remote,user,
        reinterpret_cast<void*>(StorageGetter),reinterpret_cast<void*>(UserGetter))){Receipt("refused","storage_import_patch_failed");return false;}
    Receipt("armed");return true;
}
}
BOOL WINAPI DllMain(HINSTANCE,DWORD reason,void*) {
    if(reason==DLL_PROCESS_ATTACH && (!AttachStorage()||!grid2vr::startupmovies::Attach()))ExitRefused();return TRUE;
}
#endif
