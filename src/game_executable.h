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
inline bool ValveSigned(const wchar_t* path,HANDLE handle) {
    WINTRUST_FILE_INFO file{};file.cbStruct=sizeof(file);file.pcwszFilePath=path;file.hFile=handle;
    WINTRUST_DATA data{};data.cbStruct=sizeof(data);data.dwUIChoice=WTD_UI_NONE;
    data.dwUnionChoice=WTD_CHOICE_FILE;data.pFile=&file;data.dwStateAction=WTD_STATEACTION_VERIFY;
    data.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    bool valid=WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE),&action,&data)==ERROR_SUCCESS;
    if(valid){
        auto provider=WTHelperProvDataFromStateData(data.hWVTStateData);
        auto signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;
        wchar_t name[256]{};
        valid=signer&&signer->csCertChain&&signer->pasCertChain&&signer->pasCertChain[0].pCert&&
            CertGetNameStringW(signer->pasCertChain[0].pCert,CERT_NAME_SIMPLE_DISPLAY_TYPE,0,nullptr,name,256)==12&&
            !std::wcscmp(name,L"Valve Corp.");
    }
    data.dwStateAction=WTD_STATEACTION_CLOSE;
    WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE),&action,&data);
    return valid;
}
// Same contract as tools/GameExecutable.cs. Excludes only the observed Steam
// variation; every other byte before the certificate, including code, imports,
// data, resources and PE layout, stays pinned. Excluded bytes require Valve trust.
// The returned identity always hashes the original file for preservation checks.
inline std::string Validate(const wchar_t* path,bool avx) {
    struct File {HANDLE handle{INVALID_HANDLE_VALUE};~File(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}} file;
    try {
        file.handle=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(file.handle==INVALID_HANDLE_VALUE)return {};
        BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
        const DWORD certificate=avx?0xef8800:0xee7800,security=avx?0x1f0:0x1e0;
        if(!GetFileInformationByHandle(file.handle,&info)||
           (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
           !GetFileSizeEx(file.handle,&size)||size.QuadPart<certificate+8||size.QuadPart>certificate+65536)return {};
        std::vector<unsigned char> bytes(static_cast<std::size_t>(size.QuadPart));DWORD read{};
        if(!ReadFile(file.handle,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)||read!=bytes.size())return {};
        const auto actual=Digest(bytes.data(),read);
        if(actual==(avx?"519cd994d2ff92a2a7a2a784856854183f53e359b83f8b9563cc09a0ae91e502":
                        "f92c2e1134b5d6d61404131224b41c182089bf563d28731eef0faa7d8096ab4c"))return actual;
        DWORD offset{},length{};std::memcpy(&offset,bytes.data()+security,4);std::memcpy(&length,bytes.data()+security+4,4);
        if(offset!=certificate||length!=bytes.size()-certificate)return {};
        std::memset(bytes.data()+0x4c,0,0x84);
        std::memset(bytes.data()+(avx?0x1b0:0x1a0),0,4);
        std::memset(bytes.data()+security+4,0,4);
        const DWORD start=avx?0xbb9368:0xba716c,end=avx?0xd1d720:0xd0a7c4;
        std::memset(bytes.data()+start,0,end-start);
        std::memset(bytes.data()+(avx?0xdf005c:0xdde05c),0,16);
        const auto expected=avx?"12086dce2b2ccf0142fa3ad4662f832147899b1eb03ce2203c78e516a6082202":
                                "5f1601f57bc0628cbb6196af07015eaa6e58f3385618d9e8ca44ca50558af5b3";
        return Digest(bytes.data(),certificate)==expected&&ValveSigned(path,file.handle)?actual:std::string{};
    } catch(...) {return {};}
}
}
