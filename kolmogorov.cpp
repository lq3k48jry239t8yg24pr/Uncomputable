#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <string>
#include <vector>
#include "compression_helpers.h"

namespace fs = std::filesystem;

namespace fs = std::filesystem;

class BitReader {
public:
    static constexpr std::size_t MAX_BIT_BUFFER = 10'000'000;

private:
    std::ifstream& is;
    char byte = 0;
    int bit_pos = -1;

public:
    explicit BitReader(std::ifstream& in) : is(in) {}

    bool read_bit(char& bit) {
        if (bit_pos < 0) {
            if (!is.get(byte)) return false;
            bit_pos = 7;
        }
        bit = ((byte >> bit_pos) & 1) ? '1' : '0';
        bit_pos--;
        return true;
    }

    std::string read_bits(int n) {
        if (n < 0 || static_cast<std::size_t>(n) > MAX_BIT_BUFFER) {
            return std::string();
        }
        std::string s;
        s.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            char bit;
            if (!read_bit(bit)) return std::string();
            s.push_back(bit);
        }
        return s;
    }
};

static int parse_length_from_filename(const std::string& filename) {
    static const std::regex pattern("(\\d+)\\.bin(?:\\.gz)?$");
    std::smatch match;
    if (std::regex_search(filename, match, pattern)) {
        return std::stoi(match[1].str());
    }
    return -1;
}

int main(int argc, char* argv[]) {
    fs::path dump_dir = "blc_dumps";
    if (argc > 1) {
        dump_dir = argv[1];
    }

    if (!fs::exists(dump_dir) || !fs::is_directory(dump_dir)) {
        std::cerr << "Error: dump directory '" << dump_dir.string() << "' does not exist or is not a directory.\n";
        return 1;
    }

    std::vector<std::pair<int, fs::path>> dump_files;
    for (auto const& entry : fs::directory_iterator(dump_dir)) {
        if (!entry.is_regular_file()) continue;
        int len = parse_length_from_filename(entry.path().filename().string());
        if (len > 0) dump_files.emplace_back(len, entry.path());
    }

    std::sort(dump_files.begin(), dump_files.end(), [](auto const& a, auto const& b) {
        return a.first < b.first;
    });

    if (dump_files.empty()) {
        std::cerr << "Error: no .bin dump files found in '" << dump_dir.string() << "'.\n";
        return 1;
    }

    std::map<std::string, std::pair<int, std::string>> best_complexities;

    for (auto const& [length, path] : dump_files) {
        compression::DecompressedPath decompressed(path);
        if (!decompressed) {
            std::cerr << "Warning: failed to decompress " << path.string() << "\n";
            continue;
        }
        std::ifstream infile(decompressed.path, std::ios::binary);
        if (!infile.is_open()) {
            std::cerr << "Warning: failed to open " << decompressed.path.string() << "\n";
            continue;
        }

        BitReader reader(infile);
        uint64_t record_count = 0;

        while (true) {
            std::string program_bits = reader.read_bits(length);
            if (program_bits.size() < static_cast<size_t>(length)) break;

            char status_bit;
            if (!reader.read_bit(status_bit)) break;
            if (status_bit == '1') {
                record_count++;
                continue;
            }

            std::string len_bits = reader.read_bits(32);
            if (len_bits.size() < 32) break;
            uint32_t output_len = 0;
            for (char bit : len_bits) {
                output_len = (output_len << 1) | (bit == '1' ? 1u : 0u);
            }

            if (output_len > BitReader::MAX_BIT_BUFFER) {
                std::cerr << "Warning: invalid output length " << output_len << " in " << path.filename().string() << "\n";
                break;
            }

            std::string output_bits = reader.read_bits(static_cast<int>(output_len));
            if (output_bits.size() < output_len) break;

            auto it = best_complexities.find(output_bits);
            if (it == best_complexities.end() || length < it->second.first) {
                best_complexities[output_bits] = { length, program_bits };
            }
            record_count++;
        }

        std::cout << "Processed " << record_count << " records from " << path.filename().string() << "\n";
    }

    std::vector<std::tuple<std::string, int, std::string>> sorted_results;
    sorted_results.reserve(best_complexities.size());
    for (auto const& entry : best_complexities) {
        sorted_results.emplace_back(entry.first, entry.second.first, entry.second.second);
    }

    std::sort(sorted_results.begin(), sorted_results.end(), [](auto const& a, auto const& b) {
        if (std::get<1>(a) != std::get<1>(b)) return std::get<1>(a) < std::get<1>(b);
        return std::get<0>(a) < std::get<0>(b);
    });

    std::ofstream outfile("kolmogorov.txt");
    if (!outfile.is_open()) {
        std::cerr << "Error: could not create kolmogorov.txt\n";
        return 1;
    }

    outfile << "# output\tcomplexity\texample_program\n";
    for (auto const& [output, complexity, program] : sorted_results) {
        std::string output_label = output.empty() ? "<empty>" : output;
        outfile << output_label << '\t' << complexity << '\t' << program << '\n';
    }

    std::cout << "Wrote " << sorted_results.size() << " unique output strings to kolmogorov.txt\n";
    return 0;
}
