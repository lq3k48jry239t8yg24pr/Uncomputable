#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <memory>
#include <filesystem>
#include "compression_helpers.h"

namespace fs = std::filesystem;

// --- AST AND TYPE STRUCTURES ---
enum Kind : uint8_t { VAR, ABS, APP };
struct Node { Kind kind; int val; Node* l; Node* r; };

struct Type {
    enum TypeKind { T_VAR, T_FUN } kind;
    int id;
    Type* from = nullptr;
    Type* to = nullptr;
    Type* instance = nullptr; // Used for Union-Find substitution
};

class Arena {
private:
    std::vector<Node> nodes;
    std::vector<Type> types;
    size_t node_ptr = 0;
    size_t type_ptr = 0;
public:
    Arena(size_t max_elements) {
        nodes.resize(max_elements);
        types.resize(max_elements);
    }
    void reset() { node_ptr = 0; type_ptr = 0; }
    
    Node* alloc_node(Kind k, int v = 0, Node* left = nullptr, Node* right = nullptr) {
        Node* n = &nodes[node_ptr++];
        n->kind = k; n->val = v; n->l = left; n->r = right;
        return n;
    }
    Type* alloc_type_var(int id) {
        Type* t = &types[type_ptr++];
        t->kind = Type::T_VAR; t->id = id; t->from = t->to = t->instance = nullptr;
        return t;
    }
    Type* alloc_type_fun(Type* from, Type* to) {
        Type* t = &types[type_ptr++];
        t->kind = Type::T_FUN; t->from = from; t->to = to; t->instance = nullptr;
        return t;
    }
};

// --- BIT STREAM READER ---
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

// --- BLC AST PARSER ---
Node* parse_blc(const std::string& bits, size_t& idx, Arena& arena) {
    if (idx >= bits.size()) return nullptr;
    if (bits[idx++] == '0') {
        if (bits[idx++] == '0') return arena.alloc_node(ABS, 0, parse_blc(bits, idx, arena));
        Node* left = parse_blc(bits, idx, arena); 
        return arena.alloc_node(APP, 0, left, parse_blc(bits, idx, arena));
    } else {
        int count = 1; while (idx < bits.size() && bits[idx] == '1') { count++; idx++; }
        idx++; return arena.alloc_node(VAR, count);
    }
}

// --- HINDLEY-MILNER TYPE INFERENCE MECHANICS ---
Type* find_root(Type* t) {
    if (t->kind == Type::T_VAR && t->instance != nullptr) {
        t->instance = find_root(t->instance);
        return t->instance;
    }
    return t;
}

bool occurs_in(Type* v, Type* t) {
    t = find_root(t);
    if (t == v) return true;
    if (t->kind == Type::T_FUN) {
        return occurs_in(v, t->from) || occurs_in(v, t->to);
    }
    return false;
}

bool unify(Type* t1, Type* t2) {
    t1 = find_root(t1);
    t2 = find_root(t2);
    if (t1 == t2) return true;
    if (t1->kind == Type::T_VAR) {
        if (occurs_in(t1, t2)) return false; // Infinite type check failed
        t1->instance = t2; return true;
    }
    if (t2->kind == Type::T_VAR) {
        if (occurs_in(t2, t1)) return false;
        t2->instance = t1; return true;
    }
    return unify(t1->from, t2->from) && unify(t1->to, t2->to);
}

Type* infer(Node* n, std::vector<Type*>& env, Arena& arena, int& next_type_id, bool& success) {
    if (!success || !n) return nullptr;
    
    if (n->kind == VAR) {
        int idx = n->val;
        if (idx <= 0 || idx > env.size()) { success = false; return nullptr; } // Free variables are logical leaks
        return env[env.size() - idx];
    } 
    else if (n->kind == ABS) {
        Type* alpha = arena.alloc_type_var(next_type_id++);
        env.push_back(alpha);
        Type* beta = infer(n->l, env, arena, next_type_id, success);
        env.pop_back();
        if (!success) return nullptr;
        return arena.alloc_type_fun(alpha, beta);
    } 
    else { // APP
        Type* t_left = infer(n->l, env, arena, next_type_id, success);
        Type* t_right = infer(n->r, env, arena, next_type_id, success);
        if (!success) return nullptr;
        
        Type* alpha = arena.alloc_type_var(next_type_id++);
        Type* expected_fun = arena.alloc_type_fun(t_right, alpha);
        if (!unify(t_left, expected_fun)) { success = false; return nullptr; }
        return alpha;
    }
}

// --- LOGICAL PRETTY-PRINTER ---
void assign_names(Type* t, std::map<Type*, std::string>& name_map, char& next_char) {
    t = find_root(t);
    if (t->kind == Type::T_VAR) {
        if (name_map.find(t) == name_map.end()) {
            name_map[t] = std::string(1, next_char++);
        }
    } else {
        assign_names(t->from, name_map, next_char);
        assign_names(t->to, name_map, next_char);
    }
}

std::string type_to_proposition(Type* t, std::map<Type*, std::string>& name_map, bool is_left = false) {
    t = find_root(t);
    if (t->kind == Type::T_VAR) return name_map[t];
    
    std::string l_str = type_to_proposition(t->from, name_map, true);
    std::string r_str = type_to_proposition(t->to, name_map, false);
    
    if (is_left) {
        return "(" + l_str + "->" + r_str + ")";
    } else {
        return l_str + "->" + r_str;
    }
}

int main() {
    std::cout << "Enter BLC Layer to extract theorems from: ";
    int target_len;
    if (!(std::cin >> target_len) || target_len <= 0) return 1;

    fs::path filename = "blc_dumps/" + std::to_string(target_len) + ".bin";
    compression::DecompressedPath decompressed(filename);
    if (!decompressed) { std::cerr << "Could not locate file: " << filename << " or " << filename << ".gz\n"; return 1; }
    std::ifstream infile(decompressed.path, std::ios::binary);
    if (!infile.is_open()) { std::cerr << "Could not open file: " << decompressed.path << "\n"; return 1; }

    BitReader reader(infile);
    Arena shared_arena(10000);
    uint64_t valid_theorems = 0, total_computed = 0;

    std::cout << "\n--- DISCOVERED THEOREMS FOR LAYER " << target_len << " ---\n";

    while (true) {
        std::string prog_bits = reader.read_bits(target_len);
        if (prog_bits.length() < target_len) break; 
        
        std::string status = reader.read_bits(1);
        std::string output;
        if (status.empty()) break;

        if (status == "0") {
            std::string len_bits = reader.read_bits(32);
            if (len_bits.length() < 32) break;
            uint32_t out_len = 0;
            for (char c : len_bits) out_len = (out_len << 1) | (c == '1' ? 1 : 0);
            output = reader.read_bits(out_len);
        }
        total_computed++;

        // Process Proof into Theorem
        shared_arena.reset();
        size_t parse_idx = 0;
        Node* ast = parse_blc(prog_bits, parse_idx, shared_arena);
        
        if (ast && parse_idx == prog_bits.size()) {
            std::vector<Type*> env;
            int next_type_id = 0;
            bool success = true;
            Type* inferred_type = infer(ast, env, shared_arena, next_type_id, success);
            
            if (success && inferred_type) {
                valid_theorems++;
                std::map<Type*, std::string> name_map;
                char start_char = 'A';
                assign_names(inferred_type, name_map, start_char);
                std::string theorem = type_to_proposition(inferred_type, name_map);
                
                std::cout << "Proof[" << prog_bits << "]\u22A2Theorem:" << theorem << "\n" << "Witness[" << output << "]\n";
            }
        }
    }

    std::cout << "\n----------------------------------------\n";
    std::cout << "Layer Synthesis Report:\n";
    std::cout << "Total Evaluated Programs: " << total_computed << "\n";
    std::cout << "Mathematically Sound Theorems Extracted: " << valid_theorems << "\n";

    return 0;
}