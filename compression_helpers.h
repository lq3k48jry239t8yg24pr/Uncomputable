#pragma once

#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include "file_splitter.h"

namespace compression {
namespace fs = std::filesystem;

inline std::string escape_shell_arg(const std::string& arg) {
    std::string out;
    out.reserve(arg.size() + 2);
    out.push_back('"');
    for (char c : arg) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

inline bool run_shell_command(const std::string& command) {
    int status = std::system(command.c_str());
    return status == 0;
}

inline fs::path make_temp_filename(const fs::path& base) {
    static std::mt19937_64 gen((unsigned)std::random_device{}());
    static std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);
    fs::path temp_dir = base.has_parent_path() ? base.parent_path() : fs::temp_directory_path();
    if (temp_dir.empty() || !fs::exists(temp_dir)) {
        temp_dir = fs::temp_directory_path();
    }
    std::string name = base.filename().string() + "." + std::to_string(dist(gen)) + ".tmp";
    return temp_dir / name;
}

inline bool rename_or_copy(const fs::path& source, const fs::path& destination) {
    try {
        fs::rename(source, destination);
        return true;
    } catch (const fs::filesystem_error&) {
        if (!fs::copy_file(source, destination, fs::copy_options::overwrite_existing)) {
            return false;
        }
        return fs::remove(source);
    }
}

inline bool compress_file(const fs::path& path) {
    if (!fs::exists(path)) return false;
    fs::path gz_path = path;
    gz_path += ".gz";
    fs::path temp_path = make_temp_filename(path);
    std::string cmd = "gzip -c " + escape_shell_arg(path.string()) + " > " + escape_shell_arg(temp_path.string());
    if (!run_shell_command(cmd)) {
        if (fs::exists(temp_path)) fs::remove(temp_path);
        return false;
    }
    if (fs::exists(gz_path)) fs::remove(gz_path);
    if (!rename_or_copy(temp_path, gz_path)) {
        if (fs::exists(temp_path)) fs::remove(temp_path);
        return false;
    }
    fs::remove(path);
    return true;
}

inline bool compress_file_to_gz(const fs::path& path, const fs::path& gz_path) {
    if (!fs::exists(path)) return false;
    fs::path temp_path = make_temp_filename(path);
    std::string cmd = "gzip -c " + escape_shell_arg(path.string()) + " > " + escape_shell_arg(temp_path.string());
    if (!run_shell_command(cmd)) {
        if (fs::exists(temp_path)) fs::remove(temp_path);
        return false;
    }
    if (fs::exists(gz_path)) fs::remove(gz_path);
    if (!rename_or_copy(temp_path, gz_path)) {
        if (fs::exists(temp_path)) fs::remove(temp_path);
        return false;
    }
    fs::remove(path);
    return true;
}

inline bool compress_file_and_split_if_needed(const fs::path& path) {
    if (!compress_file(path)) return false;
    fs::path gz_path = path;
    gz_path += ".gz";
    if (fs::exists(gz_path)) {
        std::error_code ec;
        std::uintmax_t size = fs::file_size(gz_path, ec);
        if (!ec && size > DEFAULT_SPLIT_THRESHOLD_BYTES) {
            return split_file(gz_path);
        }
    }
    return true;
}

inline bool decompress_gz_to_path(const fs::path& gz_path, const fs::path& out_path) {
    if (!fs::exists(gz_path)) return false;
    std::string cmd = "gzip -d -c " + escape_shell_arg(gz_path.string()) + " > " + escape_shell_arg(out_path.string());
    if (!run_shell_command(cmd)) {
        if (fs::exists(out_path)) fs::remove(out_path);
        return false;
    }
    return true;
}

struct DecompressedPath {
    fs::path path;
    bool remove_on_exit = false;

    explicit DecompressedPath(const fs::path& base_path) {
        if (base_path.extension() == ".gz") {
            if (fs::exists(base_path)) {
                fs::path temp_path = make_temp_filename(base_path);
                if (!decompress_gz_to_path(base_path, temp_path)) return;
                path = temp_path;
                remove_on_exit = true;
                return;
            }

            if (has_split_chunks(base_path)) {
                fs::path temp_archive = make_temp_filename(base_path);
                if (!merge_split_files(base_path, temp_archive)) return;
                fs::path temp_path = make_temp_filename(base_path);
                if (!decompress_gz_to_path(temp_archive, temp_path)) {
                    if (fs::exists(temp_archive)) fs::remove(temp_archive);
                    return;
                }
                if (fs::exists(temp_archive)) fs::remove(temp_archive);
                path = temp_path;
                remove_on_exit = true;
                return;
            }
        }

        if (fs::exists(base_path)) {
            path = base_path;
            remove_on_exit = false;
            return;
        }

        if (has_split_chunks(base_path)) {
            fs::path temp_path = make_temp_filename(base_path);
            if (!merge_split_files(base_path, temp_path)) return;
            path = temp_path;
            remove_on_exit = true;
            return;
        }

        fs::path gz_path = base_path;
        gz_path += ".gz";
        if (fs::exists(gz_path)) {
            path = make_temp_filename(base_path);
            if (!decompress_gz_to_path(gz_path, path)) {
                path.clear();
                return;
            }
            remove_on_exit = true;
            return;
        }

        if (has_split_chunks(gz_path)) {
            fs::path temp_archive = make_temp_filename(gz_path);
            if (!merge_split_files(gz_path, temp_archive)) return;
            path = make_temp_filename(base_path);
            if (!decompress_gz_to_path(temp_archive, path)) {
                if (fs::exists(temp_archive)) fs::remove(temp_archive);
                path.clear();
                return;
            }
            if (fs::exists(temp_archive)) fs::remove(temp_archive);
            remove_on_exit = true;
        }
    }

    ~DecompressedPath() {
        if (remove_on_exit && !path.empty() && fs::exists(path)) {
            fs::remove(path);
        }
    }

    explicit operator bool() const { return !path.empty(); }
};

} // namespace compression
