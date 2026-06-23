#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <cstdint>
#include <filesystem>
#include "compression_helpers.h"

namespace fs = std::filesystem;

class BitReader {
    std::ifstream& is; char byte = 0; int bit_pos = -1;
public:
    BitReader(std::ifstream& in) : is(in) {}
    bool read_bit(char& bit) {
        if (bit_pos < 0) { if (!is.get(byte)) return false; bit_pos = 7; }
        bit = ((byte >> bit_pos) & 1) ? '1' : '0'; bit_pos--; return true;
    }
    std::string read_bits(int n) {
        std::string s(n, '0');
        for (int i = 0; i < n; ++i) { if (!read_bit(s[i])) return ""; }
        return s;
    }
};

int main() {
    std::cout << "Enter the BLC program length to analyze: ";
    int target_len;
    if (!(std::cin >> target_len) || target_len <= 0) return 1;

    fs::path filename = "blc_dumps/" + std::to_string(target_len) + ".bin";
    compression::DecompressedPath decompressed(filename);
    if (!decompressed) { std::cerr << "Could not open compressed or uncompressed file: " << filename << "\n"; return 1; }
    std::ifstream infile(decompressed.path, std::ios::binary);
    if (!infile.is_open()) { std::cerr << "Could not open file: " << decompressed.path << "\n"; return 1; }

    BitReader reader(infile);
    std::map<std::string, uint64_t> output_counts;
    uint64_t limit_hits = 0; uint64_t total_programs = 0;

    while (true) {
        std::string prog = reader.read_bits(target_len);
        if (prog.length() < target_len) break; 
        
        std::string status = reader.read_bits(1);
        if (status.empty()) break;

        if (status == "1") { limit_hits++; } 
        else {
            std::string len_bits = reader.read_bits(32); // Read using unified 32-bit format
            if (len_bits.length() < 32) break;
            
            uint32_t out_len = 0;
            for (char c : len_bits) out_len = (out_len << 1) | (c == '1' ? 1 : 0);
            
            std::string output = reader.read_bits(out_len);
            if (output.length() < out_len) break;
            
            output_counts[output]++;
        }
        total_programs++;
    }

    std::cout << "\n=== Analysis for Length " << target_len << " ===\n";
    std::cout << "Total Recorded Programs: " << total_programs << "\n";
    std::cout << "Programs Hit Step Limit: " << limit_hits << "\n\n";
    std::cout << "Output Frequencies:\n--------------------------------\n";
    if (output_counts.empty()) std::cout << "No standard outputs recorded.\n";
    else {
        for (const auto& pair : output_counts) {
            std::string out_disp = pair.first.empty() ? "(empty string)" : pair.first;
            std::cout << "Output [" << out_disp << "] : " << pair.second << " times\n";
        }
    } return 0;
}