#pragma once
#include <filesystem>
#include <fstream>

namespace grid2vr {
// Render-owner thread only, including the game-exit handoff callback.
class StopReceipt {
    bool reported_{};
    static bool Write(const std::filesystem::path& path,const char* text) {
        std::ofstream file(path);
        file << text;
        file.close();
        return file.good();
    }
public:
    bool Reported() const { return reported_; }
    bool Stop(const std::filesystem::path& path,bool failed) {
        if(reported_)return true;
        return reported_=Write(path,failed?"{\"failed\":true}\n":"{\"failed\":false}\n");
    }
    bool GameExit(const std::filesystem::path& path) {
        // Publish this state before the exit handoff releases its waiting worker.
        // A later Present must never truncate the completed final receipt.
        return reported_=Write(path,"{\"failed\":false,\"source\":\"game_exit\"}\n");
    }
};
}
