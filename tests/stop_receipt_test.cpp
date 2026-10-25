#include "stop_receipt.h"
#include <windows.h>
#include <cstdio>
#include <string>

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path);return {std::istreambuf_iterator<char>(input),{}};
}
#define CHECK(condition, message) if(!(condition)){puts(message);return __LINE__;}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 1;
    const auto folder=std::filesystem::path(argv[1])/std::to_wstring(GetCurrentProcessId());
    std::filesystem::create_directories(folder);
    const auto path=folder/L"runtime-stopped";
    constexpr auto exited="{\"failed\":false,\"source\":\"game_exit\"}\n";
    grid2vr::StopReceipt receipt;
    CHECK(receipt.GameExit(path) && Read(path)==exited,"Exit receipt was not completed");
    // Same ordering as Present: exit callback, handoff completion, ordinary stop.
    CHECK(receipt.Stop(path,false) && Read(path)==exited,"Exit receipt was overwritten by normal stop");
    const auto held=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    CHECK(held!=INVALID_HANDLE_VALUE,"Could not deny a second writer");
    // It must not even reopen the file after exit. Process termination during a
    // second truncating open caused the zero-byte Okutama receipt.
    const bool untouched=receipt.Stop(path,true) && receipt.Reported() && Read(path)==exited;
    CloseHandle(held);
    CHECK(untouched,"Completed exit receipt was reopened");

    grid2vr::StopReceipt failed;
    const auto failedPath=folder/L"failed-stop";
    CHECK(failed.Stop(failedPath,true) && Read(failedPath)=="{\"failed\":true}\n","Failure acknowledgment lost");
    CHECK(failed.Stop(failedPath,false) && Read(failedPath)=="{\"failed\":true}\n","Failure acknowledgment overwritten");
    // Manual Stop VR followed by native Quit still records the terminal exit.
    CHECK(failed.GameExit(failedPath) && Read(failedPath)==exited,"Native exit did not replace ordinary stop");
    grid2vr::StopReceipt retry;
    const auto missing=folder/L"missing"/L"runtime-stopped";
    CHECK(!retry.GameExit(missing) && !retry.Reported(),"Failed exit write reported success");
    CHECK(!retry.Stop(missing,false) && !retry.Reported(),"Failed normal write reported success");
    std::filesystem::create_directory(missing.parent_path());
    CHECK(retry.Stop(missing,false) && retry.Reported() && Read(missing)=="{\"failed\":false}\n","Stop write could not retry");
    for(const auto& item:{path,failedPath,missing,missing.parent_path(),folder})std::filesystem::remove(item);
    puts("Exit receipt survives later stop calls without reopening; failure, retry and manual-stop/exit paths pass.");
    return 0;
}
