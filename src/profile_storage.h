#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace grid2vr::profilestorage {
struct Request {
    std::filesystem::path remote;
    std::string id;
    std::uint32_t account{};
};
bool ParseRequest(std::string_view text, Request& result);
bool MatchesRunDataRoot(const std::filesystem::path& output, const std::filesystem::path& dataRoot);
bool ValidFileName(std::string_view name);
bool ValidImageLayout(const std::uint8_t* image, std::size_t bytes, std::uintptr_t relocatedBase);
bool UsesManagedUserFolder(std::uintptr_t callerRva);
bool InstallImportRoutes(std::uint8_t* image,std::size_t bytes,std::uintptr_t relocatedBase,
    void* expectedStorage,void* expectedUser,void* storageReplacement,void* userReplacement);
struct WriteFailure {
    std::uint32_t count{};
    char category[40]{};
    char keyHex[521]{};
    std::int32_t requestedBytes{};
    std::uint32_t win32Error{};
    bool keyTruncated{};
};
// Local implementation of the seven ISteamRemoteStorage010 methods used by the
// fingerprinted game. No member stores or forwards a Steam storage interface.
class Store {
public:
    explicit Store(const Request& request);
    ~Store();
    Store(const Store&)=delete;
    Store& operator=(const Store&)=delete;
    bool Ready() const;
    bool Write(const char* name, const void* data, std::int32_t bytes);
    std::int32_t Read(const char* name, void* data, std::int32_t bytes);
    bool Delete(const char* name);
    bool Exists(const char* name);
    std::int32_t Size(const char* name);
    std::int32_t Count();
    const char* Name(std::int32_t index, std::int32_t* size);
    bool UserFolder(char* output, std::int32_t capacity);
    WriteFailure LastWriteFailure() const;
private:
    struct State;
    std::unique_ptr<State> state;
};
}
