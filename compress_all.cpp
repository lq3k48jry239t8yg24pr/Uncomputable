#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static std::string escape_arg(const std::string& value) {
    std::string escaped = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') escaped.push_back('\\');
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

static bool compress_file(const fs::path& file_path) {
    if (!fs::exists(file_path) || !fs::is_regular_file(file_path)) return false;
    std::string source = file_path.string();
    std::string target = source + ".gz";
    std::string temp = file_path.string() + ".tmp.gz";
    std::string cmd = "gzip -c " + escape_arg(source) + " > " + escape_arg(temp);
    int code = std::system(cmd.c_str());
    if (code != 0) {
        std::cerr << "Compression failed for " << source << "\n";
        if (fs::exists(temp)) fs::remove(temp);
        return false;
    }
    if (fs::exists(target)) fs::remove(target);
    fs::rename(temp, target);
    fs::remove(source);
    return true;
}

int main() {
    const fs::path dirs[] = {"blc_dumps", "theorems"};
    for (auto const& dir : dirs) {
        if (!fs::exists(dir) || !fs::is_directory(dir)) {
            std::cerr << "Skipping missing directory: " << dir << "\n";
            continue;
        }
        for (auto const& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            const fs::path path = entry.path();
            const std::string filename = path.filename().string();
            if (filename == "checkpoint.txt") {
                std::cout << "Skipping checkpoint file: " << path << "\n";
                continue;
            }
            if (filename.size() >= 3 && filename.substr(filename.size() - 3) == ".gz") {
                std::cout << "Already compressed: " << path << "\n";
                continue;
            }
            if (compress_file(path)) {
                std::cout << "Compressed: " << path << " -> " << path << ".gz\n";
            }
        }
    }
    return 0;
}
