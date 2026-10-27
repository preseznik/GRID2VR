#include "startup_movies.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace grid2vr::startupmovies;
namespace {
void Check(bool value,const char* what){if(!value)throw std::runtime_error(what);}
void Put(std::vector<std::uint8_t>& image,std::size_t at,std::uint32_t value){std::memcpy(image.data()+at,&value,4);}
void Game(std::vector<std::uint8_t>& image,bool avx,bool standalone=false){
    std::fill(image.begin(),image.end(),0);image[0]='M';image[1]='Z';
    const std::uint32_t pe=standalone?0x50:(avx?0x158:0x148),d=avx?0xd89498:0xd77680;
    Put(image,0x3c,pe);Put(image,pe,IMAGE_NT_SIGNATURE);Put(image,pe+8,avx?0x524b8944:0x524b87ac);Put(image,pe+0x50,avx?0x100e000:0xffd000);
    const auto name=avx?0xd8b540:0xd7973c;Put(image,d,avx?0xd89c4c:0xd77e38);Put(image,d+12,name);Put(image,d+16,avx?0xb1673c:0xb04740);
    std::memcpy(image.data()+name,"bink2w32.dll",13);
    const std::uint32_t callsAvx[]{0x98ff6c,0x997ced,0x99779f,0x9975c5},callsRegular[]{0x98170c,0x9890ed,0x988b9f,0x9889c5};
    const auto* calls=avx?callsAvx:callsRegular;
    const std::uint32_t open=avx?0xb1674c:0xb04750,wait=avx?0xb16784:0xb04788,close=avx?0xb16774:0xb04778;
    const std::uint32_t slots[]{open,open,wait,close};
    for(std::size_t i=0;i<4;++i){image[calls[i]]=0xff;image[calls[i]+1]=0x15;Put(image,calls[i]+2,0x400000+slots[i]);}
    Put(image,open,0x1111);Put(image,wait,0x2222);Put(image,close,0x3333);
}
void Bink(std::vector<std::uint8_t>& image){
    image.assign(0x69000,0);image[0]='M';image[1]='Z';Put(image,0x3c,0xe0);Put(image,0xe0,IMAGE_NT_SIGNATURE);Put(image,0xe8,0x5163a808);Put(image,0x130,0x69000);
    const std::uint8_t open[]{0x55,0x8b,0xec,0x83,0xe4,0xf8,0x83,0xec,0x28};
    const std::uint8_t wait[]{0x55,0x8b,0xec,0x83,0xec,8,0x56,0x8b,0x75,8,0x85,0xf6};
    const std::uint8_t close[]{0x55,0x8b,0xec,0x83,0xe4,0xf8,0x51,0x53,0x8b,0x5d,8,0x56};
    const std::uint8_t goTo[]{0x55,0x8b,0xec,0x83,0xe4,0xf8,0x83,0xec,0x0c,0x53,0x56,0x57};
    const std::uint8_t async[]{0x55,0x8b,0xec,0x83,0xe4,0xf8,0x8b,0x45,8,0x83,0xec,0x14};
    std::memcpy(image.data()+0x117c0,open,sizeof(open));std::memcpy(image.data()+0x134f0,wait,sizeof(wait));
    std::memcpy(image.data()+0x12fd0,close,sizeof(close));std::memcpy(image.data()+0x12ca0,goTo,sizeof(goTo));std::memcpy(image.data()+0x18140,async,sizeof(async));
}
std::array<std::uint32_t,80> handle{};
int asyncCalls{},gotoCalls{},waitCalls{},closeCalls{},openCalls{};
bool clearJobs=true,doSeek=true,failOpen=false;
void Reset(){
    handle={};handle[0]=1920;handle[1]=1080;handle[2]=301;handle[3]=1;handle[0x110/4]=7;
    asyncCalls=gotoCalls=waitCalls=closeCalls=openCalls=0;clearJobs=doSeek=true;failOpen=false;
}
void* __stdcall OpenFake(const char*,std::uint32_t){++openCalls;SetLastError(123);return failOpen?nullptr:handle.data();}
std::int32_t __stdcall WaitFake(void*){++waitCalls;SetLastError(456);return 17;}
void __stdcall CloseFake(void*){++closeCalls;SetLastError(789);}
std::int32_t __stdcall AsyncFake(void* p,std::int32_t timeout){
    Check(p==handle.data()&&timeout==-1,"blocking original async completion");++asyncCalls;
    if(clearJobs){handle[0x110/4]=0;handle[0x114/4]=0;}return 1;
}
void __stdcall GotoFake(void* p,std::uint32_t frame,std::uint32_t flags){
    Check(p==handle.data()&&frame==301&&flags==0&&asyncCalls>gotoCalls,"normal seek follows async completion");++gotoCalls;
    if(doSeek)handle[3]=frame;
}
void Configure(){TestConfigure(OpenFake,WaitFake,CloseFake,{GotoFake,AsyncFake},"E:\\Games\\grid 2");}
}
int main(){try{
    Check(ParseRequest("GRID2VR_STARTUP_SKIP_V1\n")&&ParseRequest("GRID2VR_STARTUP_SKIP_V1\r\n"),"exact marker accepts line endings");
    Check(!ParseRequest("GRID2VR_STARTUP_SKIP_V1")&&!ParseRequest("GRID2VR_STARTUP_SKIP_V1\nextra\n"),"other requests rejected");
    Check(MovieForPath("video/sting.bk2","")==Movie::Sting,"relative startup sting");
    Check(MovieForPath("VIDEO\\INTEL_STING.BK2","")==Movie::Intel,"case and separator normalization");
    Check(MovieForPath("e:/games/grid 2/video/attract.bk2","E:\\Games\\grid 2")==Movie::Attract,"known root absolute path");
    Check(MovieForPath("video/attract_jpn.bk2","")==Movie::AttractJapanese,"localized attract exact match");
    Check(MovieForPath("video/sting.bik","")==Movie::Sting&&MovieForPath("video/attract_jpn.bik","")==Movie::AttractJapanese,"exact EGO logical filenames accepted");
    for(const char* path:{"sting.bk2","video/sting.bik.old","video/sting.bk2.old","video/../video/sting.bk2","other/video/sting.bk2","video/sc01.bk2","video/sc01.bik","video/race_type_race.bk2","F:/Games/grid 2/video/sting.bk2"})
        Check(MovieForPath(path,"E:\\Games\\grid 2")==Movie::None,"other movies and paths unchanged");
    std::vector<std::uint8_t> image(0x100e000);
    for(bool avx:{true,false})for(bool standalone:{false,true}){
        Game(image,avx,standalone);Check(ValidGameLayout(image.data(),image.size(),0x400000),"exact game layout");
        const auto pe=standalone?0x50:(avx?0x158:0x148);
        Put(image,0x3c,0x70);Check(!ValidGameLayout(image.data(),image.size(),0x400000),"unknown PE layout refused");Put(image,0x3c,pe);
        image[pe+8]^=1;Check(!ValidGameLayout(image.data(),image.size(),0x400000),"changed build stamp refused");image[pe+8]^=1;
        Check(!ValidGameLayout(image.data(),100,0x400000)&&!ValidGameLayout(image.data(),image.size(),0x500000),"bounds and relocation guard");
        const auto call=avx?0x98ff6c:0x98170c;image[call]^=1;Check(!ValidGameLayout(image.data(),image.size(),0x400000),"changed call refused");image[call]^=1;
        Check(!InstallImportRoutes(image.data(),image.size(),0x400000,reinterpret_cast<void*>(0x1111),reinterpret_cast<void*>(0x2222),reinterpret_cast<void*>(0x9999),
            reinterpret_cast<void*>(0xaaaa),reinterpret_cast<void*>(0xbbbb),reinterpret_cast<void*>(0xcccc)),"foreign third slot refused before modifying earlier slots");
        Check(*reinterpret_cast<std::uint32_t*>(image.data()+(avx?0xb1674c:0xb04750))==0x1111,"failed routing preserves original first slot");
        Check(InstallImportRoutes(image.data(),image.size(),0x400000,reinterpret_cast<void*>(0x1111),reinterpret_cast<void*>(0x2222),reinterpret_cast<void*>(0x3333),
            reinterpret_cast<void*>(0xaaaa),reinterpret_cast<void*>(0xbbbb),reinterpret_cast<void*>(0xcccc)),"three guarded imports routed");
        Check(*reinterpret_cast<std::uint32_t*>(image.data()+(avx?0xb1674c:0xb04750))==0xaaaa&&
              *reinterpret_cast<std::uint32_t*>(image.data()+(avx?0xb16784:0xb04788))==0xbbbb&&
              *reinterpret_cast<std::uint32_t*>(image.data()+(avx?0xb16774:0xb04778))==0xcccc,"all replacement slots correct");
    }
    Bink(image);Check(ValidBinkLayout(image.data(),image.size()),"known Bink entry guards");
    image[0x12ca0]^=1;Check(!ValidBinkLayout(image.data(),image.size()),"changed seek entry refused");
    Reset();Configure();
    Check(TestOpen("video/sting.bk2",0)==handle.data()&&GetLastError()==123&&openCalls==1,"native open result preserved");
    Check(TestWait(handle.data())==17&&GetLastError()==456&&waitCalls==1&&gotoCalls==1&&handle[3]==301,"native wait preserved after seek");
    TestWait(handle.data());Check(gotoCalls==1&&waitCalls==2,"each handle sought only once");
    TestClose(handle.data());Check(closeCalls==1&&GetLastError()==789,"original close preserved");
    auto stats=TestReceipt();Check(stats.opened==1&&stats.matched==1&&stats.seeked==1&&stats.closed==1&&!stats.failures,"bounded success counters");
    handle[3]=1;TestOpen("video/sc01.bk2",0);TestWait(handle.data());TestClose(handle.data());
    Check(handle[3]==1&&gotoCalls==1,"reused handle for career movie untouched");
    TestOpen(reinterpret_cast<const char*>(1),0x04000000);TestWait(handle.data());TestClose(handle.data());
    Check(gotoCalls==1,"memory source never interpreted as filename");
    TestOpen("E:\\Games\\grid 2\\video\\attract.bk2",0);TestWait(handle.data());TestClose(handle.data());
    Check(gotoCalls==2,"reused handle for new startup movie advances once");
    Reset();Configure();clearJobs=false;TestOpen("video/sting.bk2",0);TestWait(handle.data());
    Check(gotoCalls==0&&waitCalls==1&&TestReceipt().failures==1,"unfinished async decode leaves normal playback");
    TestWait(handle.data());Check(TestReceipt().failures==1,"failed seek never retried every frame");TestClose(handle.data());
    Reset();Configure();doSeek=false;TestOpen("video/sting.bk2",0);TestWait(handle.data());
    Check(TestReceipt().failures==1&&handle[3]==1,"seek result validated rather than forced");
    TestClose(handle.data());TestOpen("video/sc01.bk2",0);TestWait(handle.data());Check(TestReceipt().failures==1,"later playback retains failures");
    Reset();Configure();failOpen=true;Check(!TestOpen("video/sting.bk2",0)&&TestReceipt().matched==0,"failed open not registered");
    Check(!SeekToLastFrame(nullptr,{GotoFake,AsyncFake}),"invalid handle rejected");
    handle[2]=200000;Check(!SeekToLastFrame(handle.data(),{GotoFake,AsyncFake}),"unreasonable header rejected");
    std::cout<<"native startup movie routing tests passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
