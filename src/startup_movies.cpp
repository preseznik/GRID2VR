#include "startup_movies.h"
#include "game_executable.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <initializer_list>

namespace grid2vr::startupmovies {
namespace {
struct Layout {
    std::uint32_t pe, stamp, imageBytes, descriptor, originalThunk, dllName, firstThunk;
    std::uint32_t openIat, waitIat, closeIat, calls[4];
    const char* hash;
};
constexpr Layout avx{0x158,0x524b8944,0x100e000,0xd89498,0xd89c4c,0xd8b540,0xb1673c,
    0xb1674c,0xb16784,0xb16774,{0x98ff6c,0x997ced,0x99779f,0x9975c5},
    "519cd994d2ff92a2a7a2a784856854183f53e359b83f8b9563cc09a0ae91e502"};
constexpr Layout regular{0x148,0x524b87ac,0xffd000,0xd77680,0xd77e38,0xd7973c,0xb04740,
    0xb04750,0xb04788,0xb04778,{0x98170c,0x9890ed,0x988b9f,0x9889c5},
    "f92c2e1134b5d6d61404131224b41c182089bf563d28731eef0faa7d8096ab4c"};
// These exact standalone files retain the audited import/call map. Keep their
// header layouts separate so lazy file validation also checks the same family.
constexpr Layout standaloneAvx=[]{auto layout=avx;layout.pe=0x50;
    layout.hash=gameexe::avxStandalone;return layout;}();
constexpr Layout standaloneRegular=[]{auto layout=regular;layout.pe=0x50;
    layout.hash=gameexe::regularStandalone;return layout;}();
constexpr char binkHash[]="03f9c695d4c45186d7b43ab64926fcb0933ca9a02eed43fd67cd8775ffac81e9";
constexpr std::uint32_t binkBytes=0x69000;
constexpr std::uint32_t openRva=0x117c0,waitRva=0x134f0,closeRva=0x12fd0,gotoRva=0x12ca0,asyncWaitRva=0x18140;
std::uint32_t U32(const std::uint8_t* p){std::uint32_t n{};std::memcpy(&n,p,4);return n;}
const Layout* HeaderLayout(const std::uint8_t* image,std::size_t bytes){
    if(!image||bytes<0x200||image[0]!='M'||image[1]!='Z')return nullptr;
    for(const auto* layout:{&avx,&regular,&standaloneAvx,&standaloneRegular}){
        if(bytes>=layout->imageBytes&&U32(image+0x3c)==layout->pe&&
           U32(image+layout->pe)==IMAGE_NT_SIGNATURE&&
           U32(image+layout->pe+8)==layout->stamp&&
           U32(image+layout->pe+0x50)==layout->imageBytes)return layout;
    }return nullptr;
}
char Fold(char c){if(c=='/')return '\\';return c>='A'&&c<='Z'?static_cast<char>(c+('a'-'A')):c;}
bool EqualPath(std::string_view a,std::string_view b){
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i)if(Fold(a[i])!=Fold(b[i]))return false;
    return true;
}
bool Exchange(void** at,void* expected,void* replacement){
    DWORD previous{};if(!VirtualProtect(at,sizeof(void*),PAGE_READWRITE,&previous))return false;
    const bool changed=InterlockedCompareExchangePointer(at,replacement,expected)==expected;
    DWORD ignored{};VirtualProtect(at,sizeof(void*),previous,&ignored);return changed;
}
bool ReadMemory(const void* at,void* out,std::size_t bytes){
    SIZE_T read{};return at&&ReadProcessMemory(GetCurrentProcess(),at,out,bytes,&read)&&read==bytes;
}
}
bool ParseRequest(std::string_view text){return text=="GRID2VR_STARTUP_SKIP_V1\n"||text=="GRID2VR_STARTUP_SKIP_V1\r\n";}
Movie MovieForPath(std::string_view path,std::string_view gameDirectory){
    if(path.empty()||path.size()>1023)return Movie::None;
    if(path.size()>gameDirectory.size()&&!gameDirectory.empty()&&
       EqualPath(path.substr(0,gameDirectory.size()),gameDirectory)&&Fold(path[gameDirectory.size()])=='\\')
        path.remove_prefix(gameDirectory.size()+1);
    constexpr const char* names[]{"video\\sting.bk2","video\\intel_sting.bk2","video\\attract.bk2","video\\attract_jpn.bk2"};
    // states.bin uses logical .bik names; EGO's file provider resolves the
    // installed .bk2 files. Accept either exact spelling, never other movies.
    constexpr const char* logical[]{"video\\sting.bik","video\\intel_sting.bik","video\\attract.bik","video\\attract_jpn.bik"};
    for(std::size_t i=0;i<4;++i)if(EqualPath(path,names[i])||EqualPath(path,logical[i]))return static_cast<Movie>(i+1);
    return Movie::None;
}
bool ValidGameLayout(const std::uint8_t* image,std::size_t bytes,std::uintptr_t base){
    const auto* layout=HeaderLayout(image,bytes);if(!layout||base>UINT32_MAX-layout->imageBytes)return false;
    const auto d=layout->descriptor;
    if(U32(image+d)!=layout->originalThunk||U32(image+d+12)!=layout->dllName||U32(image+d+16)!=layout->firstThunk||
       std::memcmp(image+layout->dllName,"bink2w32.dll",13))return false;
    const std::uint32_t slots[]{layout->openIat,layout->openIat,layout->waitIat,layout->closeIat};
    for(std::size_t i=0;i<4;++i){const auto at=layout->calls[i];
        if(image[at]!=0xff||image[at+1]!=0x15||U32(image+at+2)!=base+slots[i])return false;
    }return true;
}
bool ValidBinkLayout(const std::uint8_t* image,std::size_t bytes){
    if(!image||bytes<binkBytes||image[0]!='M'||image[1]!='Z'||U32(image+0x3c)!=0xe0||
       U32(image+0xe0)!=IMAGE_NT_SIGNATURE||U32(image+0xe8)!=0x5163a808||U32(image+0x130)!=binkBytes)return false;
    const std::uint8_t open[]{0x55,0x8b,0xec,0x83,0xe4,0xf8,0x83,0xec,0x28};
    const std::uint8_t wait[]{0x55,0x8b,0xec,0x83,0xec,8,0x56,0x8b,0x75,8,0x85,0xf6};
    const std::uint8_t close[]{0x55,0x8b,0xec,0x83,0xe4,0xf8,0x51,0x53,0x8b,0x5d,8,0x56};
    const std::uint8_t goTo[]{0x55,0x8b,0xec,0x83,0xe4,0xf8,0x83,0xec,0x0c,0x53,0x56,0x57};
    const std::uint8_t asyncWait[]{0x55,0x8b,0xec,0x83,0xe4,0xf8,0x8b,0x45,8,0x83,0xec,0x14};
    return !std::memcmp(image+openRva,open,sizeof(open))&&!std::memcmp(image+waitRva,wait,sizeof(wait))&&
        !std::memcmp(image+closeRva,close,sizeof(close))&&!std::memcmp(image+gotoRva,goTo,sizeof(goTo))&&
        !std::memcmp(image+asyncWaitRva,asyncWait,sizeof(asyncWait));
}
bool InstallImportRoutes(std::uint8_t* image,std::size_t bytes,std::uintptr_t base,
    void* expectedOpen,void* expectedWait,void* expectedClose,void* replacementOpen,void* replacementWait,void* replacementClose){
    if(!ValidGameLayout(image,bytes,base)||!expectedOpen||!expectedWait||!expectedClose||
       !replacementOpen||!replacementWait||!replacementClose)return false;
    const auto* layout=HeaderLayout(image,bytes);
    void** slots[]{reinterpret_cast<void**>(image+layout->openIat),reinterpret_cast<void**>(image+layout->waitIat),reinterpret_cast<void**>(image+layout->closeIat)};
    void* originals[]{expectedOpen,expectedWait,expectedClose};void* replacements[]{replacementOpen,replacementWait,replacementClose};
    for(std::size_t i=0;i<3;++i)if(*slots[i]!=originals[i])return false;
    std::size_t done{};
    for(;done<3;++done)if(!Exchange(slots[done],originals[done],replacements[done]))break;
    if(done==3)return true;
    while(done){--done;Exchange(slots[done],replacements[done],originals[done]);}return false;
}
bool SeekToLastFrame(void* handle,const SeekApi& api){
    if(!api.goTo||!api.asyncWait)return false;
    struct Header{std::uint32_t width,height,frames,frame;} header{};
    if(!ReadMemory(handle,&header,sizeof(header))||!header.width||!header.height||header.width>8192||header.height>8192||
       !header.frames||header.frames>108000||!header.frame||header.frame>header.frames)return false;
    // GRID 2 may already have dispatched its first async decode by BinkWait.
    // BinkGoto explicitly refuses pending jobs at +110/+114; the original
    // blocking API completes them before the normal keyframe-aware seek.
    api.asyncWait(handle,-1);
    std::uint32_t jobs[2]{};
    if(!ReadMemory(static_cast<const std::uint8_t*>(handle)+0x110,jobs,sizeof(jobs))||jobs[0]||jobs[1])return false;
    api.goTo(handle,header.frames,0);
    Header after{};return ReadMemory(handle,&after,sizeof(after))&&after.frames==header.frames&&after.frame==header.frames;
}

namespace {
INIT_ONCE once=INIT_ONCE_STATIC_INIT;
SRWLOCK lock=SRWLOCK_INIT;
Open originalOpen{};Wait originalWait{};Close originalClose{};SeekApi seek{};
const Layout* hostLayout{};
HMODULE binkModule{};
wchar_t outputFolder[1024]{},hostFile[1024]{};
char gameFolder[1024]{};
bool verified{};
char failure[64]{};
std::uint32_t opened{},matched{},seeked{},failures{},closed{};
struct Entry{void* handle{};Movie movie{};bool attempted{};};
Entry entries[8]{};
std::uint32_t movieCounts[4]{};
void Receipt(const char* phase){
    if(!outputFolder[0])return;
    wchar_t path[1200]{},temporary[1200]{};
    swprintf_s(path,L"%s\\startup-skip.json",outputFolder);swprintf_s(temporary,L"%s\\startup-skip.tmp",outputFolder);
    char json[1024]{};const auto bytes=sprintf_s(json,
        "{\"schema\":1,\"phase\":\"%s\",\"pid\":%lu,\"verified\":%s,\"opened\":%u,\"matched\":%u,\"seeked\":%u,\"closed\":%u,\"failures\":%u,\"reason\":\"%s\",\"sting\":%u,\"intelSting\":%u,\"attract\":%u,\"attractJapanese\":%u}\n",
        phase,GetCurrentProcessId(),verified?"true":"false",opened,matched,seeked,closed,failures,failure,
        movieCounts[0],movieCounts[1],movieCounts[2],movieCounts[3]);
    HANDLE file=CreateFileW(temporary,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return;DWORD written{};
    const bool saved=bytes>0&&WriteFile(file,json,static_cast<DWORD>(bytes),&written,nullptr)&&written==static_cast<DWORD>(bytes);
    CloseHandle(file);if(saved)MoveFileExW(temporary,path,MOVEFILE_REPLACE_EXISTING);else DeleteFileW(temporary);
}
void Failed(const char* reason){++failures;if(!failure[0])strcpy_s(failure,reason);}
bool ReadSmall(const wchar_t* path,char* text,DWORD capacity,DWORD& bytes){
    const auto attrs=GetFileAttributesW(path);
    if(attrs==INVALID_FILE_ATTRIBUTES||(attrs&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return false;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER size{};
    bool valid=GetFileSizeEx(file,&size)&&size.QuadPart>0&&size.QuadPart<capacity;
    valid=valid&&ReadFile(file,text,capacity-1,&bytes,nullptr)&&bytes==static_cast<DWORD>(size.QuadPart);
    CloseHandle(file);if(valid)text[bytes]=0;return valid;
}
bool HashEquals(const wchar_t* path,const char* expected){
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    bool valid=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0&&
        BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    unsigned char buffer[16384]{},digest[32]{};DWORD bytes{};
    while(valid){if(!ReadFile(file,buffer,sizeof(buffer),&bytes,nullptr)){valid=false;break;}
        if(!bytes)break;valid=BCryptHashData(hash,buffer,bytes,0)>=0;}
    if(valid)valid=BCryptFinishHash(hash,digest,sizeof(digest),0)>=0;
    if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);CloseHandle(file);
    char hex[65]{};for(std::size_t i=0;i<32;++i)sprintf_s(hex+i*2,3,"%02x",digest[i]);
    return valid&&!std::strcmp(hex,expected);
}
BOOL CALLBACK Verify(PINIT_ONCE,void*,void**){
    wchar_t binkFile[1024]{},gameRoot[1024]{},path[1200]{};
    const auto identity=gameexe::Validate(hostFile,hostLayout->stamp==avx.stamp);
    const bool standalone=hostLayout==&standaloneAvx||hostLayout==&standaloneRegular;
    if(!GetModuleFileNameW(binkModule,binkFile,1024)||identity.empty()||
       standalone!=gameexe::IsStandalone(identity)||(standalone&&identity!=hostLayout->hash)||!HashEquals(binkFile,binkHash)){
        Failed("unsupported_binary");Receipt("refused");return TRUE;}
    wcscpy_s(gameRoot,hostFile);auto slash=wcsrchr(gameRoot,L'\\');if(!slash){Failed("invalid_game_path");Receipt("refused");return TRUE;}*slash=0;
    // Leave save checks intact and refuse to seek when a legacy state-file
    // deployment or another modification is still active.
    swprintf_s(path,L"%s\\system\\states.bin",gameRoot);
    const bool stock=HashEquals(path,"07e593b3891171a7c4caa67b0b2e0ee1a9c166fc2d4bd25814ad695c439d9adc");
    // Practice/Race have their own guarded flow transaction. Requiring the
    // Normal flow here would silently disable startup skipping in those modes.
    // This feature never changes either file or suppresses native save checks.
    BOOL substituted{};
    if(!stock){Failed("stock_states_required");Receipt("refused");return TRUE;}
    if(!WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,gameRoot,-1,gameFolder,1024,nullptr,&substituted)||substituted){
        Failed("unrepresentable_game_path");Receipt("refused");return TRUE;}
    verified=true;Receipt("active");return TRUE;
}
Movie Classify(const char* source,std::uint32_t flags){
    // The native movie player uses this flag for a memory buffer, not a path.
    if(!source||(flags&0x04000000))return Movie::None;
    char path[1024]{};
    for(std::size_t i=0;i<sizeof(path);++i){if(!ReadMemory(source+i,path+i,1))return Movie::None;
        if(!path[i])return MovieForPath(std::string_view(path,i),gameFolder);}
    return Movie::None;
}
void* __stdcall OpenMovie(const char* source,std::uint32_t flags){
    const auto incomingError=GetLastError();
#ifndef GRID2VR_STARTUP_MOVIES_TEST
    InitOnceExecuteOnce(&once,Verify,nullptr,nullptr);
#endif
    const auto movie=verified?Classify(source,flags):Movie::None;SetLastError(incomingError);
    void* handle=originalOpen(source,flags);const auto error=GetLastError();
    AcquireSRWLockExclusive(&lock);++opened;
    if(handle&&movie!=Movie::None){
        ++matched;bool tracked{};
        for(auto& entry:entries)if(!entry.handle){entry={handle,movie,false};tracked=true;break;}
        if(!tracked)Failed("movie_capacity");Receipt(verified?"active":"refused");
    }
    ReleaseSRWLockExclusive(&lock);SetLastError(error);return handle;
}
std::int32_t __stdcall WaitMovie(void* handle){
    const auto error=GetLastError();AcquireSRWLockExclusive(&lock);
    for(auto& entry:entries)if(entry.handle==handle&&!entry.attempted){
        entry.attempted=true;
        if(SeekToLastFrame(handle,seek)){++seeked;++movieCounts[static_cast<std::uint32_t>(entry.movie)-1];}
        else Failed("seek_failed");
        Receipt("active");break;
    }
    ReleaseSRWLockExclusive(&lock);SetLastError(error);return originalWait(handle);
}
void __stdcall CloseMovie(void* handle){
    const auto error=GetLastError();AcquireSRWLockExclusive(&lock);
    for(auto& entry:entries)if(entry.handle==handle){entry={};++closed;Receipt("active");break;}
    ReleaseSRWLockExclusive(&lock);SetLastError(error);originalClose(handle);
}
std::size_t LoadedImageBytes(HMODULE module){
    const auto* image=reinterpret_cast<const std::uint8_t*>(module);
    if(!image||image[0]!='M'||image[1]!='Z')return 0;const auto pe=U32(image+0x3c);
    if(pe>0x400||U32(image+pe)!=IMAGE_NT_SIGNATURE)return 0;return U32(image+pe+0x50);
}
}
bool Attach(){
    if(!GetModuleFileNameW(nullptr,hostFile,1024))return true;
    wchar_t marker[1200]{},root[1024]{};wcscpy_s(root,hostFile);auto slash=wcsrchr(root,L'\\');if(!slash)return true;slash[1]=0;
    swprintf_s(marker,L"%sGRID2VR.diagnostic-session",root);
    char text[4096]{};DWORD bytes{};
    if(!ReadSmall(marker,text,sizeof(text),bytes))return true;
    char* line=std::strchr(text,'\n');if(!line)return true;
    const auto header=std::string_view(text,line-text);
    if(header!="GRID2VR_DIAGNOSTIC_V1"&&header!="GRID2VR_DIAGNOSTIC_V1\r")return true;
    auto end=std::strchr(line+1,'\n');if(end)*end=0;end=std::strchr(line+1,'\r');if(end)*end=0;
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,line+1,-1,outputFolder,1024)||outputFolder[1]!=L':'||outputFolder[2]!=L'\\')return true;
    swprintf_s(marker,L"%s\\startup-skip.request",outputFolder);
    const auto attrs=GetFileAttributesW(marker);const auto error=GetLastError();
    if(attrs==INVALID_FILE_ATTRIBUTES&&(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND))return true;
    if(!ReadSmall(marker,text,sizeof(text),bytes)||!ParseRequest(std::string_view(text,bytes))){Failed("invalid_request");Receipt("refused");return false;}
    const auto host=GetModuleHandleW(nullptr);const auto image=reinterpret_cast<std::uint8_t*>(host);
    const auto imageBytes=LoadedImageBytes(host);hostLayout=HeaderLayout(image,imageBytes);
    binkModule=GetModuleHandleW(L"bink2w32.dll");const auto bink=reinterpret_cast<const std::uint8_t*>(binkModule);
    if(!hostLayout||!ValidGameLayout(image,imageBytes,reinterpret_cast<std::uintptr_t>(host))||!ValidBinkLayout(bink,LoadedImageBytes(binkModule))){
        Failed("unsupported_import_layout");Receipt("refused");return false;}
    originalOpen=reinterpret_cast<Open>(bink+openRva);originalWait=reinterpret_cast<Wait>(bink+waitRva);originalClose=reinterpret_cast<Close>(bink+closeRva);
    seek={reinterpret_cast<GoTo>(bink+gotoRva),reinterpret_cast<AsyncWait>(bink+asyncWaitRva)};
    if(!InstallImportRoutes(image,imageBytes,reinterpret_cast<std::uintptr_t>(host),reinterpret_cast<void*>(originalOpen),
        reinterpret_cast<void*>(originalWait),reinterpret_cast<void*>(originalClose),reinterpret_cast<void*>(OpenMovie),reinterpret_cast<void*>(WaitMovie),reinterpret_cast<void*>(CloseMovie))){
        Failed("foreign_or_unwritable_import");Receipt("refused");return false;}
    Receipt("armed");return true;
}
#ifdef GRID2VR_STARTUP_MOVIES_TEST
void TestConfigure(Open open,Wait wait,Close close,const SeekApi& api,const char* folder){
    originalOpen=open;originalWait=wait;originalClose=close;seek=api;verified=true;
    strcpy_s(gameFolder,folder);std::memset(entries,0,sizeof(entries));std::memset(movieCounts,0,sizeof(movieCounts));
    opened=matched=seeked=closed=failures=0;failure[0]=0;
}
void* TestOpen(const char* path,std::uint32_t flags){return OpenMovie(path,flags);}
std::int32_t TestWait(void* handle){return WaitMovie(handle);}
void TestClose(void* handle){CloseMovie(handle);}
TestCounts TestReceipt(){return {opened,matched,seeked,closed,failures};}
#endif
}
