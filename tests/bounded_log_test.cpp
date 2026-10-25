#include "bounded_log.h"
#include <iterator>
#include <string>
int main(int argc,char** argv) {
    if (argc!=2) return 1;
    const std::filesystem::path root(argv[1]);std::filesystem::create_directories(root);
    auto current=root/"bounded.txt",previous=root/"bounded.txt.previous";
    {
        grid2vr::BoundedLog log(current,128);
        for (unsigned i=0;i<1000;++i) log << std::string(31,'x') << '\n';
        log << "LATEST\n";log.FlushNow();if (!log) return 2;
        if (std::filesystem::file_size(current)>128 || std::filesystem::file_size(previous)>128) return 3;
        std::ifstream input(current);std::string text((std::istreambuf_iterator<char>(input)),{});
        if (!text.ends_with("LATEST\n")) return 4;
    }
    {
        grid2vr::BoundedLog log(root/"diagnostic.txt",0);
        log << std::string(10000,'d');log.flush();
        if (!log || std::filesystem::file_size(root/"diagnostic.txt")!=10000) return 5;
    }
    {
        const auto quiet=root/"disabled.txt";
        std::filesystem::remove(quiet);
        grid2vr::BoundedLog log;log.open(quiet,128,false);
        log << std::string(4096,'x') << '\n';log.flush();log.FlushNow();
        if (!log || std::filesystem::exists(quiet) || std::filesystem::exists(root/"disabled.txt.previous")) return 6;
    }
    for (const auto& name:{"bounded.txt","bounded.txt.previous","diagnostic.txt"}) std::filesystem::remove(root/name);
}
