#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ostream>

namespace grid2vr {
// Runtime logs retain at most two fixed-size segments. Diagnostic mode (limit
// zero) retains the original complete log. The caller owns the output directory.
class BoundedLog : public std::ostream {
    class Buffer : public std::streambuf {
    public:
        bool Open(const std::filesystem::path& path,size_t limit,bool enabled) {
            enabled_=enabled;
            if (!enabled_) return true;
            path_=path;limit_=limit;written_=unflushed_=0;
            file_.open(path,std::ios::binary|std::ios::trunc);return bool(file_);
        }
        bool Flush() { if (!enabled_) return true;file_.flush();unflushed_=0;return bool(file_); }
    private:
        std::streamsize xsputn(const char* data,std::streamsize count) override {
            if (!enabled_) return count;
            std::streamsize done{};
            while (done<count && file_) {
                if (limit_ && written_==limit_) {
                    file_.close();std::error_code error;
                    auto previous=path_;previous+=L".previous";
                    std::filesystem::remove(previous,error);
                    if (error) return done;
                    std::filesystem::rename(path_,previous,error);
                    if (error) return done;
                    file_.open(path_,std::ios::binary|std::ios::trunc);written_=0;
                }
                const auto bytes=limit_?std::min<size_t>(size_t(count-done),limit_-written_):size_t(count-done);
                file_.write(data+done,bytes);
                if (!file_) return done;
                written_+=bytes;unflushed_+=bytes;done+=bytes;
            }
            return done;
        }
        int_type overflow(int_type value) override {
            if (traits_type::eq_int_type(value,traits_type::eof())) return traits_type::not_eof(value);
            const char byte=traits_type::to_char_type(value);
            return xsputn(&byte,1)==1?value:traits_type::eof();
        }
        int sync() override {
            // Avoid per-draw disk flushes during a sustained session.
            return !limit_ || unflushed_>=65536 ? (Flush()?0:-1):0;
        }
        std::filesystem::path path_;
        std::ofstream file_;
        size_t limit_{},written_{},unflushed_{};
        bool enabled_{true};
    } buffer_;
public:
    BoundedLog():std::ostream(nullptr) { rdbuf(&buffer_); }
    BoundedLog(const std::filesystem::path& path,size_t limit):BoundedLog() { open(path,limit); }
    void open(const std::filesystem::path& path,size_t limit=0,bool enabled=true) { if (!buffer_.Open(path,limit,enabled)) setstate(std::ios::failbit); }
    void FlushNow() { if (!buffer_.Flush()) setstate(std::ios::failbit); }
};
constexpr size_t sessionLogSegmentBytes=1024*1024;
}
