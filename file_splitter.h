#pragma once

#include <cstdint>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace compression {
namespace fs = std::filesystem;

inline constexpr std::size_t DEFAULT_SPLIT_THRESHOLD_BYTES = 99ULL * 1000 * 1000;

inline fs::path make_split_chunk_path(const fs::path& base_path, std::size_t index) {
    std::ostringstream oss;
    oss << base_path.string() << ".part" << std::setw(3) << std::setfill('0') << index;
    return fs::path(oss.str());
}

inline std::vector<fs::path> get_split_chunk_paths(const fs::path& base_path) {
    std::vector<fs::path> paths;
    std::vector<fs::path> candidate_bases;
    candidate_bases.push_back(base_path);
    if (base_path.extension() != ".gz") {
        candidate_bases.push_back(base_path.string() + ".gz");
    }

    for (const auto& candidate_base : candidate_bases) {
        for (std::size_t index = 0;; ++index) {
            fs::path candidate = make_split_chunk_path(candidate_base, index);
            if (!fs::exists(candidate)) break;
            paths.push_back(candidate);
        }
    }

    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

inline bool has_split_chunks(const fs::path& base_path) {
    return !get_split_chunk_paths(base_path).empty();
}

inline bool split_file(const fs::path& source, std::size_t max_chunk_bytes = DEFAULT_SPLIT_THRESHOLD_BYTES) {
    if (!fs::exists(source) || !fs::is_regular_file(source)) return false;
    if (max_chunk_bytes == 0) return false;

    const std::uintmax_t source_size = fs::file_size(source);
    if (source_size <= max_chunk_bytes) {
        for (std::size_t index = 0;; ++index) {
            fs::path chunk_path = make_split_chunk_path(source, index);
            if (!fs::exists(chunk_path)) break;
            fs::remove(chunk_path);
        }
        return true;
    }

    for (std::size_t index = 0;; ++index) {
        fs::path chunk_path = make_split_chunk_path(source, index);
        if (!fs::exists(chunk_path)) break;
        fs::remove(chunk_path);
    }

    std::ifstream input(source, std::ios::binary);
    if (!input.is_open()) return false;

    std::vector<char> buffer(1024 * 1024);
    std::size_t chunk_index = 0;
    std::size_t current_chunk_size = 0;
    std::ofstream output(make_split_chunk_path(source, chunk_index), std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        input.close();
        return false;
    }

    while (input.good()) {
        input.read(buffer.data(), buffer.size());
        std::streamsize bytes_read = input.gcount();
        if (bytes_read <= 0) break;

        if (current_chunk_size + static_cast<std::size_t>(bytes_read) > max_chunk_bytes) {
            const std::size_t first_part = max_chunk_bytes - current_chunk_size;
            if (first_part > 0) {
                output.write(buffer.data(), first_part);
            }
            output.close();

            ++chunk_index;
            current_chunk_size = 0;
            output.open(make_split_chunk_path(source, chunk_index), std::ios::binary | std::ios::trunc);
            if (!output.is_open()) {
                input.close();
                return false;
            }

            const std::size_t remaining = static_cast<std::size_t>(bytes_read) - first_part;
            if (remaining > 0) {
                output.write(buffer.data() + first_part, remaining);
                current_chunk_size = remaining;
            }
        } else {
            output.write(buffer.data(), bytes_read);
            current_chunk_size += static_cast<std::size_t>(bytes_read);
        }
    }

    output.close();
    input.close();

    if (fs::exists(source)) {
        fs::remove(source);
    }
    return true;
}

inline bool merge_split_files(const fs::path& base_path, const fs::path& output_path = {}) {
    std::vector<fs::path> chunk_paths = get_split_chunk_paths(base_path);
    if (chunk_paths.empty()) {
        if (fs::exists(base_path) && fs::is_regular_file(base_path)) {
            if (output_path.empty() || output_path == base_path) return true;
            if (!fs::exists(output_path.parent_path()) && !output_path.parent_path().empty()) {
                fs::create_directories(output_path.parent_path());
            }
            return fs::copy_file(base_path, output_path, fs::copy_options::overwrite_existing);
        }
        return false;
    }

    fs::path target_path = output_path.empty() ? base_path : output_path;
    if (!target_path.parent_path().empty() && !fs::exists(target_path.parent_path())) {
        fs::create_directories(target_path.parent_path());
    }

    std::ofstream output(target_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) return false;

    std::vector<char> buffer(1024 * 1024);
    for (const auto& chunk_path : chunk_paths) {
        std::ifstream input(chunk_path, std::ios::binary);
        if (!input.is_open()) {
            output.close();
            if (fs::exists(target_path)) {
                fs::remove(target_path);
            }
            return false;
        }
        while (input.good()) {
            input.read(buffer.data(), buffer.size());
            std::streamsize bytes_read = input.gcount();
            if (bytes_read <= 0) break;
            output.write(buffer.data(), bytes_read);
        }
        input.close();
    }

    output.close();
    return true;
}

} // namespace compression
