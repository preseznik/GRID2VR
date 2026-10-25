#include "startup_movies.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace grid2vr::startupmovies;
namespace fs=std::filesystem;
namespace {
void Check(bool value,const char* what){if(!value)throw std::runtime_error(what);}
struct Worker {
    using Stop=std::int32_t(__stdcall*)(std::uint32_t);
    Stop request{},join{};bool started{};
    void Finish(){if(started){request(0);join(0);started=false;}}
    ~Worker(){Finish();}
};
std::string Hash(const fs::path& path){
    std::ifstream file(path,std::ios::binary);BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    if(!file||BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return {};
    if(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0){BCryptCloseAlgorithmProvider(algorithm,0);return {};}
    std::array<char,16384> block{};bool valid=true;
    while(file){file.read(block.data(),block.size());if(BCryptHashData(hash,reinterpret_cast<PUCHAR>(block.data()),static_cast<ULONG>(file.gcount()),0)<0)valid=false;}
    std::array<unsigned char,32> digest{};valid=valid&&!file.bad()&&BCryptFinishHash(hash,digest.data(),32,0)>=0;
    BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);char text[65]{};
    for(std::size_t i=0;i<32;++i)sprintf_s(text+i*2,3,"%02x",digest[i]);return valid?text:"";
}
}
// Optional standalone decoder verification. It never loads GRID 2, creates a
// window, configures sound, or changes a game file. Four videos, no captures.
int wmain(int argc,wchar_t** argv){try{
    Check(argc==2||(argc==3&&!wcscmp(argv[2],L"--async")),"expected installed game directory and optional --async");
    const bool useAsync=argc==3;const auto root=fs::absolute(argv[1]).lexically_normal();
    const auto dll=root/L"bink2w32.dll";
    Check(Hash(dll)=="03f9c695d4c45186d7b43ab64926fcb0933ca9a02eed43fd67cd8775ffac81e9","exact decoder fingerprint required");
    HMODULE module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    Check(module!=nullptr,"load verified decoder");
    Check(ValidBinkLayout(reinterpret_cast<const std::uint8_t*>(module),0x69000),"decoder instruction guards");
    const auto open=reinterpret_cast<Open>(GetProcAddress(module,"_BinkOpen@8"));
    const auto close=reinterpret_cast<Close>(GetProcAddress(module,"_BinkClose@4"));
    const auto goTo=reinterpret_cast<GoTo>(GetProcAddress(module,"_BinkGoto@12"));
    const auto async=reinterpret_cast<AsyncWait>(GetProcAddress(module,"_BinkDoFrameAsyncWait@8"));
    const auto doFrame=reinterpret_cast<std::int32_t(__stdcall*)(void*)>(GetProcAddress(module,"_BinkDoFrame@4"));
    Check(open&&close&&goTo&&async&&doFrame,"expected decoder exports");
    const auto start=reinterpret_cast<std::int32_t(__stdcall*)(std::uint32_t,std::uint32_t)>(GetProcAddress(module,"_BinkStartAsyncThread@8"));
    const auto doAsync=reinterpret_cast<std::int32_t(__stdcall*)(void*,std::uint32_t,std::uint32_t)>(GetProcAddress(module,"_BinkDoFrameAsync@12"));
    Worker worker{reinterpret_cast<Worker::Stop>(GetProcAddress(module,"_BinkRequestStopAsyncThread@4")),
                  reinterpret_cast<Worker::Stop>(GetProcAddress(module,"_BinkWaitStopAsyncThread@4"))};
    Check(start&&doAsync&&worker.request&&worker.join,"expected async exports");
    // The game starts each worker with (index,0) and passes two worker indices
    // to DoFrameAsync. The DLL explicitly supports equal indices (one worker).
    if(useAsync){Check(start(0,0)!=0,"start decoder worker");worker.started=true;}
    std::cout<<"{\"schema\":1,\"gameLaunched\":false,\"windowsCreated\":false,\"async\":"<<(useAsync?"true":"false")<<",\"movies\":[";
    bool first=true;
    for(const char* name:{"sting.bk2","intel_sting.bk2","attract.bk2","attract_jpn.bk2"}){
        const auto file=root/"video"/name;const auto path=file.string();void* handle=open(path.c_str(),0);Check(handle!=nullptr,"open startup movie");
        std::array<std::uint32_t,4> header{};SIZE_T bytes{};
        Check(ReadProcessMemory(GetCurrentProcess(),handle,header.data(),sizeof(header),&bytes)&&bytes==sizeof(header),"read decoder header");
        const auto before=header[3];const auto started=GetTickCount64();
        std::array<std::uint32_t,2> pending{};
        if(useAsync){
            Check(doAsync(handle,0,0)!=0,"dispatch real async decode");
            Check(ReadProcessMemory(GetCurrentProcess(),static_cast<const std::uint8_t*>(handle)+0x110,pending.data(),sizeof(pending),&bytes)&&bytes==sizeof(pending),"read real job flags");
        }else doFrame(handle);
        const bool sought=SeekToLastFrame(handle,{goTo,async});
        // Decode the final frame normally too; this catches a seek which only
        // changes the frame counter but leaves decoder references invalid.
        if(sought)doFrame(handle);
        Check(ReadProcessMemory(GetCurrentProcess(),handle,header.data(),sizeof(header),&bytes)&&bytes==sizeof(header),"read final header");
        const auto elapsed=GetTickCount64()-started;close(handle);
        Check(sought&&header[3]==header[2],"normal Bink seek reaches final frame");
        if(!first)std::cout<<',';first=false;
        std::cout<<"{\"name\":\""<<name<<"\",\"frames\":"<<header[2]<<",\"before\":"<<before<<",\"after\":"<<header[3]<<",\"pendingJob0\":"<<pending[0]<<",\"pendingJob1\":"<<pending[1]<<",\"elapsedMs\":"<<elapsed<<"}";
    }
    std::cout<<"]}\n";worker.Finish();FreeLibrary(module);return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
