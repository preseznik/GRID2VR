#include "profile_storage.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace grid2vr::profilestorage;
namespace fs=std::filesystem;
void Check(bool value,const char* what){if(!value)throw std::runtime_error(what);}
void Put(std::vector<std::uint8_t>& image,std::size_t at,std::uint32_t value){std::memcpy(image.data()+at,&value,4);}
std::string RequestText(const fs::path& remote,const std::string& id,const std::string& account="13216522"){
    const auto path=remote.u8string();return "GRID2VR_PROFILE_STORAGE_V1\n"+std::string(path.begin(),path.end())+"\n"+id+"\n"+account+"\n";
}
int main(int argc,char** argv){
    try{
        Check(argc==2,"fixture root argument");const auto fixture=fs::absolute(argv[1]).lexically_normal();
        // Each test uses a fresh child; never delete a caller-supplied directory.
        const auto root=fixture/("run-"+std::to_string(GetCurrentProcessId()));
        Check(!fs::exists(root),"fresh fixture directory");fs::create_directories(root);
        const std::string id="0123456789abcdef0123456789abcdef";
        const auto remote=root/"artifacts"/"profiles"/id/"remote";fs::create_directories(remote);
        Request request;Check(ParseRequest(RequestText(remote,id),request),"valid request");
        Check(!ParseRequest(RequestText(remote,id,"4294967296"),request),"overflow account rejected");
        Check(!ParseRequest(RequestText(remote,id,"0"),request),"zero account rejected");
        Check(!ParseRequest(RequestText(remote,id)+"extra\n",request),"extra request field rejected");
        Check(!ParseRequest(RequestText(remote/"..",id),request),"parent root rejected");
        Check(!ParseRequest(RequestText(root/"remote",id),request),"unowned root rejected");
        Check(ParseRequest(RequestText(remote,id),request),"request restored");
        for(const char* name:{"../save","a/b","a\\b","c:save","NUL","CON.txt","LPT1.dat","COM9","x ","x.",".","..","","a?b"})
            Check(!ValidFileName(name),"unsafe filename rejected");
        Check(ValidFileName("savegame@profile0#QKIGMYYI.LKR"),"actual save filename accepted");
        Check(ValidFileName("safe..leaf"),"dot-dot inside leaf allowed");
        const auto original=root/"original-steam-save";{std::ofstream file(original);file<<"untouched";}
        {
            Store store(request);Check(store.Ready(),"safe store ready");Check(store.Count()==0,"fresh profile empty");
            const char* name="savegame@profile0#QKIGMYYI.LKR";const std::array<char,5> bytes{'a','b','\0','d','e'};
            Check(store.Write(name,bytes.data(),bytes.size()),"local write");
            Check(store.Exists(name)&&store.Size(name)==5&&store.Count()==1,"local file metadata");
            std::array<char,8> read{};Check(store.Read(name,read.data(),8)==5&&!std::memcmp(read.data(),bytes.data(),5),"binary local read");
            std::int32_t size{};Check(std::string(store.Name(0,&size))==name&&size==5,"local enumeration");
            Check(store.Name(1,&size)==nullptr&&size==0,"enumeration bound");
            char saveRoot[512]{},cloudRoot[512]{};
            Check(store.UserFolder(saveRoot,sizeof(saveRoot))&&store.UserFolder(cloudRoot,sizeof(cloudRoot)),"both EGO callers receive managed root");
            // Shipped EGO code strips its constructor's user-data prefix from
            // the save directory, converts '\\' to '@', then adds '#' + file.
            std::string key=std::string(saveRoot)+"\\savegame\\profile0";
            Check(key.starts_with(cloudRoot),"save path shares cloud-key root");key.erase(0,std::strlen(cloudRoot)+1);
            std::replace(key.begin(),key.end(),'\\','@');key+="#QKIGMYYI.LKR";
            Check(key==name&&store.Read(key.c_str(),read.data(),8)==5,"EGO key matches copied Steam filename");
            std::string badKey=std::string(saveRoot)+"\\savegame\\profile0#QKIGMYYI.LKR";
            std::replace(badKey.begin(),badKey.end(),'\\','@');
            Check(!store.Write(badKey.c_str(),"bad",3),"unstripped drive path stays rejected");
            auto failure=store.LastWriteFailure();
            Check(failure.count==1&&std::string(failure.category)=="invalid_filename"&&failure.requestedBytes==3,"failed write has specific metadata");
            Check(store.Count()==1&&store.Read(name,read.data(),8)==5,"read after write failure");
            Check(store.LastWriteFailure().count==1,"read and enumeration preserve failure");
            Check(!store.Write("../original-steam-save","bad",3),"traversal write rejected");
            Check(!store.Delete("../original-steam-save"),"traversal delete rejected");
            Check(!store.Write(".grid2vr-write-10-2.tmp","bad",3),"temporary namespace reserved");
            Check(!store.Write(name,nullptr,5)&&!store.Write(name,bytes.data(),-1),"invalid input rejected");
            char folder[512]{};Check(store.UserFolder(folder,sizeof(folder)),"fallback folder provided");
            Check(fs::path(folder)==remote.parent_path()/"local"&&fs::is_directory(fs::path(folder)/"savegame"),"fallback stays in profile");
            Check(!store.UserFolder(folder,2),"small folder buffer rejected");
            HANDLE locked=CreateFileW((remote/name).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
            Check(locked!=INVALID_HANDLE_VALUE,"lock original local file");
            Check(!store.Write(name,"new",3),"blocked atomic replacement fails safely");CloseHandle(locked);
            failure=store.LastWriteFailure();
            Check(std::string(failure.category)=="atomic_replace_failed"&&failure.win32Error!=0,"atomic error retained");
            Check(store.Read(name,read.data(),8)==5&&!std::memcmp(read.data(),bytes.data(),5),"failed replacement preserves old contents");
            Check(store.Write(name,"new",3)&&store.Size(name)==3,"atomic replacement succeeds");
            Check(store.LastWriteFailure().count==failure.count&&std::string(store.LastWriteFailure().category)==failure.category,"later success does not erase earlier failures");
            Check(!store.Write(std::string(300,'x').c_str(),"bad",3),"long key rejected");
            failure=store.LastWriteFailure();Check(failure.keyTruncated&&std::strlen(failure.keyHex)==520,"failure key capture bounded");
            Check(store.Delete(name)&&!store.Exists(name)&&store.Count()==0,"local delete");
            Check(store.Write("zero",nullptr,0)&&store.Exists("zero")&&store.Size("zero")==0,"zero byte save supported");
            Check(store.Delete("zero"),"zero delete");
            Check(CreateHardLinkW((remote/"linked").c_str(),original.c_str(),nullptr),"hardlink fixture");
            Check(!store.Write("linked","bad",3)&&!store.Delete("linked")&&!store.Exists("linked"),"hardlink operations refused");
            Check(DeleteFileW((remote/"linked").c_str()),"remove hardlink fixture");
            Check(!MoveFileExW(remote.c_str(),(remote.parent_path()/"moved").c_str(),0),"leased root cannot be moved");
        }
        {std::ifstream file(original);std::string text;file>>text;Check(text=="untouched","unrelated save unchanged");}
        Check(CreateHardLinkW((remote/"linked").c_str(),original.c_str(),nullptr),"startup hardlink fixture");
        {Store unsafe(request);Check(!unsafe.Ready(),"startup hardlinks refused");}
        Check(DeleteFileW((remote/"linked").c_str()),"remove startup hardlink");
        for(int i=0;i<65;++i){std::ofstream file(remote/("file"+std::to_string(i)));file<<'x';}
        {Store tooMany(request);Check(!tooMany.Ready(),"bounded file count");}
        std::vector<std::uint8_t> image(0x100e000);constexpr std::uintptr_t base=0x400000;
        image[0]='M';image[1]='Z';Put(image,0x3c,0x158);Put(image,0x158,IMAGE_NT_SIGNATURE);
        Put(image,0x160,0x524b8944);Put(image,0x1a8,0x100e000);Put(image,0xd89394,0xd89d28);
        Put(image,0xd893a0,0xd89ff0);Put(image,0xd893a4,0xb16818);std::memcpy(image.data()+0xd89ff0,"steam_api.dll",13);
        for(const auto at:{0x48b9a,0x22d435,0x50763b,0x56aae0,0x6316fc,0x7d948c}){
            image[at]=0xff;image[at+1]=0x15;Put(image,at+2,base+0xb16824);}
        image[0x146c99]=0xff;image[0x146c9a]=0x15;Put(image,0x146c9b,base+0xb1681c);
        const std::uint8_t tail[]{0x85,0xc0,0x74,0x6a,0x8b,0x10,0x8b,0x52,0x18,0x68,4,1,0,0};
        std::memcpy(image.data()+0x146c9f,tail,sizeof(tail));
        const std::uint8_t constructorTail[]{0x8b,0x10,0x8b,0x52,0x18,0x68,4,1,0,0,0x8d,0x4e,0x18,0x51,0x8b,0xc8,0xff,0xd2};
        image[0x7f4b77]=0xff;image[0x7f4b78]=0x15;Put(image,0x7f4b79,base+0xb1681c);
        std::memcpy(image.data()+0x7f4b7d,constructorTail,sizeof(constructorTail));
        Check(ValidImageLayout(image.data(),image.size(),base),"known image guards");
        Check(!ValidImageLayout(image.data(),100,base),"truncated image refused");
        image[0x146c9f]^=1;Check(!ValidImageLayout(image.data(),image.size(),base),"changed folder instruction refused");image[0x146c9f]^=1;
        image[0x7f4b7d]^=1;Check(!ValidImageLayout(image.data(),image.size(),base),"changed cloud constructor instruction refused");image[0x7f4b7d]^=1;
        Put(image,0xb16824,0x1111);Put(image,0xb1681c,0x2222);
        Check(!InstallImportRoutes(image.data(),image.size(),base,reinterpret_cast<void*>(0x9999),reinterpret_cast<void*>(0x2222),reinterpret_cast<void*>(0x3333),reinterpret_cast<void*>(0x4444)),"foreign hook refused");
        Check(*reinterpret_cast<std::uint32_t*>(image.data()+0xb16824)==0x1111,"foreign refusal does not modify IAT");
        Check(InstallImportRoutes(image.data(),image.size(),base,reinterpret_cast<void*>(0x1111),reinterpret_cast<void*>(0x2222),reinterpret_cast<void*>(0x3333),reinterpret_cast<void*>(0x4444)),"guarded IAT routes installed");
        Check(*reinterpret_cast<std::uint32_t*>(image.data()+0xb16824)==0x3333&&*reinterpret_cast<std::uint32_t*>(image.data()+0xb1681c)==0x4444,"both IAT routes match");
        Check(UsesManagedUserFolder(0x146c9f)&&UsesManagedUserFolder(0x7f4b7d)&&!UsesManagedUserFolder(0x146c9e)&&!UsesManagedUserFolder(0x7f4b7c)&&!UsesManagedUserFolder(0x8d455)&&!UsesManagedUserFolder(0x137702)&&!UsesManagedUserFolder(0x4abf16),"only two save folder callers diverted");
        // Steam starts the regular executable before it dispatches to AVX. It
        // also has real save code (-noavx), so it needs isolation, not a bypass.
        std::fill(image.begin(),image.end(),0);image[0]='M';image[1]='Z';Put(image,0x3c,0x148);
        Put(image,0x148,IMAGE_NT_SIGNATURE);Put(image,0x150,0x524b87ac);Put(image,0x198,0xffd000);
        Put(image,0xd7757c,0xd77f14);Put(image,0xd77588,0xd781dc);Put(image,0xd7758c,0xb0481c);
        std::memcpy(image.data()+0xd781dc,"steam_api.dll",13);
        for(const auto at:{0x1845fa,0x1d102b,0x2206c0,0x2573ec,0x27eb25,0x5ff05c}){
            image[at]=0xff;image[at+1]=0x15;Put(image,at+2,base+0xb04828);}
        image[0x604249]=0xff;image[0x60424a]=0x15;Put(image,0x60424b,base+0xb04820);
        std::memcpy(image.data()+0x60424f,tail,sizeof(tail));
        image[0x174af7]=0xff;image[0x174af8]=0x15;Put(image,0x174af9,base+0xb04820);
        std::memcpy(image.data()+0x174afd,constructorTail,sizeof(constructorTail));
        Check(ValidImageLayout(image.data(),image.size(),base),"known regular executable guarded");
        Put(image,0xb04828,0x1111);Put(image,0xb04820,0x2222);
        Check(InstallImportRoutes(image.data(),image.size(),base,reinterpret_cast<void*>(0x1111),reinterpret_cast<void*>(0x2222),reinterpret_cast<void*>(0x3333),reinterpret_cast<void*>(0x4444)),"regular executable routes installed");
        Check(*reinterpret_cast<std::uint32_t*>(image.data()+0xb04828)==0x3333&&*reinterpret_cast<std::uint32_t*>(image.data()+0xb04820)==0x4444,"regular IAT both routed");
        Check(UsesManagedUserFolder(0x60424f)&&UsesManagedUserFolder(0x174afd)&&!UsesManagedUserFolder(0x60424e)&&!UsesManagedUserFolder(0x174afc),"regular exact folder callers");
        image[0x174afd]^=1;Check(!ValidImageLayout(image.data(),image.size(),base),"regular changed cloud constructor refused");image[0x174afd]^=1;
        image[0x60424f]^=1;Check(!ValidImageLayout(image.data(),image.size(),base),"regular changed instruction refused");
        fs::remove_all(root);std::cout<<"profile storage isolation tests passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
