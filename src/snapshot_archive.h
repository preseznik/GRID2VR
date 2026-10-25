#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>

namespace grid2vr {
// Bounded CPU evidence, packed during rendering and unpacked after recovery.
// Each record is name length, data length (little-endian uint16), name, data.
class SnapshotArchive {
public:
    bool Write(const std::filesystem::path& folder,std::string_view name,const void* data,size_t size) {
        std::lock_guard lock(mutex_);
        if (failed_ || name.empty() || name.size()>95 || !data || !size || size>512 || bytes_+4+name.size()+size>64ull*1024*1024) return false;
        if (!file_.is_open()) {
            file_.open(folder/L"cpu-snapshots.bin",std::ios::binary);
            file_.write("G2VRSNP1",8);bytes_=8;
        }
        const uint16_t header[]{static_cast<uint16_t>(name.size()),static_cast<uint16_t>(size)};
        file_.write(reinterpret_cast<const char*>(header),sizeof(header));file_.write(name.data(),name.size());
        file_.write(static_cast<const char*>(data),size);file_.flush();
        bytes_+=4+name.size()+size;failed_=!file_.good();return !failed_;
    }
private:
    std::mutex mutex_;
    std::ofstream file_;
    size_t bytes_{};
    bool failed_{};
};
}
