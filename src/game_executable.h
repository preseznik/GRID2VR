#pragma once
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>
#include <bcrypt.h>
#include <array>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace grid2vr::gameexe {
inline constexpr char avxStandalone[]="825f30a49a668870c7bfab0309c3afb01a396fc1cec0be8df9764e32bf2fb53f";
inline constexpr char regularStandalone[]="d8ec0c631b453d6e5e9adebae4cfc9b612823fec316344c28544bc65245ba35d";
inline bool IsStandalone(const std::string& identity) {
    return identity==avxStandalone || identity==regularStandalone;
}
inline std::string Digest(const unsigned char* bytes, ULONG count) {
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    bool valid=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0 &&
        BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    std::array<unsigned char,32> digest{};
    valid=valid && BCryptHashData(hash,const_cast<PUCHAR>(bytes),count,0)>=0 &&
        BCryptFinishHash(hash,digest.data(),32,0)>=0;
    if(hash)BCryptDestroyHash(hash);
    if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);
    if(!valid)return {};
    std::string result;result.reserve(64);
    for(const auto value:digest){result+="0123456789abcdef"[value>>4];result+="0123456789abcdef"[value&15];}
    return result;
}
// Why Validate refused. The renderer cannot log before admission, so a refusal
// is written to the run folder instead; only fixed step names and numbers.
struct Refusal {
    const char* step="none";unsigned long error{};long long size{};long trust{};
    std::string sha256,normalized;std::wstring signer;
};
inline bool ValveSigned(const wchar_t* path,HANDLE handle,Refusal* refusal=nullptr) {
    WINTRUST_FILE_INFO file{};file.cbStruct=sizeof(file);file.pcwszFilePath=path;file.hFile=handle;
    WINTRUST_DATA data{};data.cbStruct=sizeof(data);data.dwUIChoice=WTD_UI_NONE;
    data.dwUnionChoice=WTD_CHOICE_FILE;data.pFile=&file;data.dwStateAction=WTD_STATEACTION_VERIFY;
    data.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG trust=WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE),&action,&data);
    if(refusal)refusal->trust=trust;
    bool valid=trust==ERROR_SUCCESS;
    if(valid){
        auto provider=WTHelperProvDataFromStateData(data.hWVTStateData);
        auto signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;
        wchar_t name[256]{};
        const auto length=signer&&signer->csCertChain&&signer->pasCertChain&&signer->pasCertChain[0].pCert?
            CertGetNameStringW(signer->pasCertChain[0].pCert,CERT_NAME_SIMPLE_DISPLAY_TYPE,0,nullptr,name,256):0;
        if(refusal)refusal->signer=name;
        valid=length==12&&!std::wcscmp(name,L"Valve Corp.");
    }
    data.dwStateAction=WTD_STATEACTION_CLOSE;
    WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE),&action,&data);
    return valid;
}
// Same contract as tools/GameExecutable.cs. Excludes only the observed Steam
// variation; every other byte before the certificate, including code, imports,
// data, resources and PE layout, stays pinned. Excluded bytes require Valve trust.
// Two standalone files are admitted separately by exact complete-file identity.
// The returned identity always hashes the original file for preservation checks.
inline std::string Validate(const wchar_t* path,bool avx,Refusal* refusal=nullptr) {
    struct File {HANDLE handle{INVALID_HANDLE_VALUE};~File(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}} file;
    Refusal ignored;auto& why=refusal?*refusal:ignored;
    const auto refuse=[&](const char* step){why.step=step;return std::string{};};
    try {
        file.handle=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(file.handle==INVALID_HANDLE_VALUE){why.error=GetLastError();return refuse("open");}
        BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
        const DWORD certificate=avx?0xef8800:0xee7800,security=avx?0x1f0:0x1e0;
        if(!GetFileInformationByHandle(file.handle,&info)||
           (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){why.error=GetLastError();return refuse("attributes");}
        if(!GetFileSizeEx(file.handle,&size)){why.error=GetLastError();return refuse("size");}
        why.size=size.QuadPart;
        if(size.QuadPart<certificate||size.QuadPart>certificate+65536)return refuse("size");
        std::vector<unsigned char> bytes(static_cast<std::size_t>(size.QuadPart));DWORD read{};
        if(!ReadFile(file.handle,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)||read!=bytes.size()){why.error=GetLastError();return refuse("read");}
        const auto actual=Digest(bytes.data(),read);why.sha256=actual;
        if(bytes.size()==certificate && actual==(avx?avxStandalone:regularStandalone))return actual;
        if(bytes.size()<certificate+8)return refuse("short_certificate");
        if(actual==(avx?"519cd994d2ff92a2a7a2a784856854183f53e359b83f8b9563cc09a0ae91e502":
                        "f92c2e1134b5d6d61404131224b41c182089bf563d28731eef0faa7d8096ab4c"))return actual;
        DWORD offset{},length{};std::memcpy(&offset,bytes.data()+security,4);std::memcpy(&length,bytes.data()+security+4,4);
        if(offset!=certificate||length!=bytes.size()-certificate)return refuse("security_directory");
        std::memset(bytes.data()+0x4c,0,0x84);
        std::memset(bytes.data()+(avx?0x1b0:0x1a0),0,4);
        std::memset(bytes.data()+security+4,0,4);
        const DWORD start=avx?0xbb9368:0xba716c,end=avx?0xd1d720:0xd0a7c4;
        std::memset(bytes.data()+start,0,end-start);
        std::memset(bytes.data()+(avx?0xdf005c:0xdde05c),0,16);
        const auto expected=avx?"12086dce2b2ccf0142fa3ad4662f832147899b1eb03ce2203c78e516a6082202":
                                "5f1601f57bc0628cbb6196af07015eaa6e58f3385618d9e8ca44ca50558af5b3";
        why.normalized=Digest(bytes.data(),certificate);
        if(why.normalized!=expected)return refuse("normalized_hash");
        return ValveSigned(path,file.handle,refusal)?actual:refuse("signature");
    } catch(...) {return refuse("exception");}
}
}
