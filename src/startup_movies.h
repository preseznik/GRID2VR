#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace grid2vr::startupmovies {
enum class Movie : std::uint32_t { None, Sting, Intel, Attract, AttractJapanese };
bool ParseRequest(std::string_view text);
Movie MovieForPath(std::string_view path, std::string_view gameDirectory);
bool ValidGameLayout(const std::uint8_t* image, std::size_t bytes, std::uintptr_t base);
bool ValidBinkLayout(const std::uint8_t* image, std::size_t bytes);
bool InstallImportRoutes(std::uint8_t* image, std::size_t bytes, std::uintptr_t base,
    void* expectedOpen, void* expectedWait, void* expectedClose,
    void* replacementOpen, void* replacementWait, void* replacementClose);

using Open = void* (__stdcall*)(const char*, std::uint32_t);
using Wait = std::int32_t (__stdcall*)(void*);
using Close = void (__stdcall*)(void*);
using GoTo = void (__stdcall*)(void*, std::uint32_t, std::uint32_t);
using AsyncWait = std::int32_t (__stdcall*)(void*, std::int32_t);
struct SeekApi { GoTo goTo{}; AsyncWait asyncWait{}; };
// Uses the decoder's public header and normal seek API. Does not write Bink or
// game state, invent completion, bypass the native callbacks, or skip consent.
bool SeekToLastFrame(void* handle, const SeekApi& api);

// Called once from the proxy's DllMain. Missing request is completely inactive.
// Only guarded IAT pointer replacement happens here; full file hashes are
// checked lazily outside the loader lock before any movie is advanced.
bool Attach();
#ifdef GRID2VR_STARTUP_MOVIES_TEST
struct TestCounts { std::uint32_t opened,matched,seeked,closed,failures; };
void TestConfigure(Open open,Wait wait,Close close,const SeekApi& api,const char* folder);
void* TestOpen(const char* path,std::uint32_t flags);
std::int32_t TestWait(void* handle);
void TestClose(void* handle);
TestCounts TestReceipt();
#endif
}
