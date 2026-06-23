#include <algorithm>
#include <cmath>
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

static long double two_pow_neg(int len) {
    return std::ldexp(1.0L, -len);
}

static long double log2_safe(long double value) {
    return value > 0.0L ? std::log2(value) : -INFINITY;
}

static std::string format_output(const std::string& bits) {
    return bits.empty() ? "<empty>" : bits;
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

    std::map<std::string, long double> cumulative_prior;
    std::ofstream outfile("solomonoff.txt");
    if (!outfile.is_open()) {
        std::cerr << "Error: could not create solomonoff.txt\n";
        return 1;
    }

    outfile << "# Solomonoff prior approximation from " << dump_dir.string() << "\n";
    outfile << "# Each section shows the cumulative prior after processing each length file.\n";
    outfile << "# Format: output\tnew_contribution\tcumulative_prior\n";
    outfile << "# NOTE: We approximate the prior using weight 2^{-length} for each halting output program. Also, each value is normalized and the actual value is the value shown minus 232.\n\n";

    long double global_mass = 0.0L;
    int files_processed = 0;

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
        std::map<std::string, long double> file_prior;
        std::map<std::string, uint64_t> file_counts;
        uint64_t records = 0;
        uint64_t halting = 0;
        uint64_t nonhalting = 0;

        while (true) {
            std::string program_bits = reader.read_bits(length);
            if (program_bits.size() < static_cast<size_t>(length)) break;

            char status_bit;
            if (!reader.read_bit(status_bit)) break;

            if (status_bit == '1') {
                nonhalting++;
                records++;
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

            long double weight = two_pow_neg(length);
            cumulative_prior[output_bits] += weight;
            file_prior[output_bits] += weight;
            file_counts[output_bits] += 1;
            global_mass += weight;
            halting++;
            records++;
        }

        files_processed++;
        outfile << "=== File: " << path.filename().string() << " (length=" << length << ") ===\n";
        outfile << "Records processed: " << records << "\n";
        outfile << "Halting outputs: " << halting << "\n";
        outfile << "Non-halting records: " << nonhalting << "\n";
        outfile << "Total cumulative prior log2 mass: " << std::fixed << log2_safe(global_mass) << "\n";
        outfile << "New output contributions in this file (output, log2 new contribution, log2 cumulative prior, occurrences):\n";

        std::vector<std::tuple<std::string, long double, long double, uint64_t>> contributions;
        contributions.reserve(file_prior.size());
        for (auto const& entry : file_prior) {
            const std::string& output = entry.first;
            long double new_weight = entry.second;
            contributions.emplace_back(output, log2_safe(new_weight), log2_safe(cumulative_prior[output]), file_counts[output]);
        }

        std::sort(contributions.begin(), contributions.end(), [](auto const& a, auto const& b) {
            if (std::get<2>(a) != std::get<2>(b)) return std::get<2>(a) > std::get<2>(b);
            return std::get<0>(a) < std::get<0>(b);
        });

        for (auto const& [output, new_log2, cum_log2, count] : contributions) {
            outfile << format_output(output) << '\t' << new_log2 << '\t' << cum_log2 << '\t' << count << "\n";
        }
        outfile << "\n";
    }

    outfile << "# Final cumulative Solomonoff prior distribution\n";
    std::vector<std::pair<std::string, long double>> final_results;
    for (auto const& entry : cumulative_prior) {
        final_results.emplace_back(entry.first, log2_safe(entry.second));
    }
    std::sort(final_results.begin(), final_results.end(), [](auto const& a, auto const& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });

    outfile << "Output\tCumulativePriorLog2\n";
    for (auto const& entry : final_results) {
        outfile << format_output(entry.first) << '\t' << entry.second << "\n";
    }

    std::cout << "Created solomonoff.txt with " << final_results.size() << " unique outputs over " << files_processed << " files.\n";
    return 0;
}
