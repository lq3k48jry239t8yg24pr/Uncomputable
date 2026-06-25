#include <iostream>
#include <vector>
#include <string>
#include <functional>
#include <stdexcept>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <pthread.h>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <map>
#include "compression_helpers.h"

namespace fs = std::filesystem;

// --- EVALUATOR COMPONENTS ---
enum Kind : uint8_t { VAR, ABS, APP };
struct Node { Kind kind; int val; Node* l; Node* r; };
struct Env;
struct Thunk { Node* node; Env* env; };
struct Env { Thunk* thunk; Env* tail; };

class Arena {
private:
    std::vector<Node> nodes; std::vector<Thunk> thunks; std::vector<Env> envs;
    size_t node_ptr = 0; size_t thunk_ptr = 0; size_t env_ptr = 0;
public:
    Arena(size_t max_elements) {
        nodes.resize(max_elements); thunks.resize(max_elements); envs.resize(max_elements);
    }
    void reset() { node_ptr = 0; thunk_ptr = 0; env_ptr = 0; }
    Node* alloc_node(Kind k, int v = 0, Node* left = nullptr, Node* right = nullptr) {
        if (node_ptr >= nodes.size()) throw std::runtime_error("Node OOM");
        Node* n = &nodes[node_ptr++]; n->kind = k; n->val = v; n->l = left; n->r = right; return n;
    }
    Thunk* alloc_thunk(Node* n, Env* e) {
        if (thunk_ptr >= thunks.size()) throw std::runtime_error("Thunk OOM");
        Thunk* t = &thunks[thunk_ptr++]; t->node = n; t->env = e; return t;
    }
    Env* alloc_env(Thunk* t, Env* tail) {
        if (env_ptr >= envs.size()) throw std::runtime_error("Env OOM");
        Env* e = &envs[env_ptr++]; e->thunk = t; e->tail = tail; return e;
    }
};

struct StackItem { bool is_update; Thunk* target; };
struct WHNFResult { Node* node; Env* env; size_t args_start; size_t args_count; };

struct NormalizeTask {
    enum Kind { NORMALIZE, BUILD_ABS, BUILD_APP };
    Kind kind;
    Node* node;
    Env* env;
    int depth;
    Node* curr_app;
    size_t args_start;
    size_t args_count;
    size_t arg_idx;
};

// Reusable workspace passed by reference to eliminate dynamic allocations
struct WorkerWorkspace {
    std::vector<StackItem> eval_stack;
    std::vector<Thunk*> args_pool;
    std::vector<NormalizeTask> task_stack;
    std::vector<Node*> result_stack;

    void clear() {
        eval_stack.clear();
        args_pool.clear();
        task_stack.clear();
        result_stack.clear();
    }
};

WHNFResult eval_whnf(Node* start_node, Env* start_env, Arena& arena, uint64_t& steps, uint64_t max_steps, WorkerWorkspace& ws) {
    Node* curr_node = start_node; Env* curr_env = start_env;
    ws.eval_stack.clear();
    size_t args_start = ws.args_pool.size();

    while (true) {
        if (steps >= max_steps) return { nullptr, nullptr, 0, 0 };
        steps++;
        if (curr_node->kind == APP) {
            ws.eval_stack.push_back({ false, arena.alloc_thunk(curr_node->r, curr_env) });
            curr_node = curr_node->l;
        } else if (curr_node->kind == ABS) {
            if (ws.eval_stack.empty()) return { curr_node, curr_env, args_start, 0 };
            StackItem top = ws.eval_stack.back(); ws.eval_stack.pop_back();
            if (top.is_update) {
                top.target->node = curr_node; top.target->env = curr_env;
            } else {
                curr_env = arena.alloc_env(top.target, curr_env); curr_node = curr_node->l;
            }
        } else if (curr_node->kind == VAR) {
            Env* e = curr_env;
            for (int i = 1; i < curr_node->val; ++i) { if (!e) break; e = e->tail; }
            if (!e) {
                while (!ws.eval_stack.empty() && ws.eval_stack.back().is_update) {
                    ws.eval_stack.back().target->node = curr_node; ws.eval_stack.back().target->env = curr_env; ws.eval_stack.pop_back();
                }
                while (!ws.eval_stack.empty()) {
                    if (!ws.eval_stack.back().is_update) ws.args_pool.push_back(ws.eval_stack.back().target);
                    ws.eval_stack.pop_back();
                }
                size_t args_count = ws.args_pool.size() - args_start;
                return { curr_node, curr_env, args_start, args_count };
            }
            Thunk* t = e->thunk;
            if (t->node->kind != ABS) ws.eval_stack.push_back({ true, t });
            curr_node = t->node; curr_env = t->env;
        }
    }
}

Node* normalize(Node* node, Env* env, Arena& arena, int depth, uint64_t& steps, uint64_t max_steps, WorkerWorkspace& ws) {
    ws.clear();
    ws.task_stack.push_back({NormalizeTask::NORMALIZE, node, env, depth, nullptr, 0, 0, 0});
    while (!ws.task_stack.empty()) {
        NormalizeTask task = ws.task_stack.back();
        ws.task_stack.pop_back();
        if (task.kind == NormalizeTask::NORMALIZE) {
            if (steps >= max_steps) return nullptr;
            WHNFResult res = eval_whnf(task.node, task.env, arena, steps, max_steps, ws);
            if (!res.node) return nullptr;
            if (res.node->kind == ABS) {
                ws.task_stack.push_back({NormalizeTask::BUILD_ABS, nullptr, nullptr, 0, nullptr, 0, 0, 0});
                int next_depth = task.depth + 1;
                Env* next_env = arena.alloc_env(arena.alloc_thunk(arena.alloc_node(VAR, next_depth), nullptr), res.env);
                ws.task_stack.push_back({NormalizeTask::NORMALIZE, res.node->l, next_env, next_depth, nullptr, 0, 0, 0});
            } else {
                Node* curr = (res.node->val > 0) ? arena.alloc_node(VAR, task.depth - res.node->val + 1) : res.node;
                if (res.args_count == 0) {
                    ws.result_stack.push_back(curr);
                } else {
                    ws.task_stack.push_back({NormalizeTask::BUILD_APP, nullptr, nullptr, task.depth, curr, res.args_start, res.args_count, 0});
                    Thunk* first_arg = ws.args_pool[res.args_start];
                    ws.task_stack.push_back({NormalizeTask::NORMALIZE, first_arg->node, first_arg->env, task.depth, nullptr, 0, 0, 0});
                }
            }
        } else if (task.kind == NormalizeTask::BUILD_ABS) {
            Node* res_node = ws.result_stack.back();
            ws.result_stack.pop_back();
            ws.result_stack.push_back(arena.alloc_node(ABS, 0, res_node));
        } else if (task.kind == NormalizeTask::BUILD_APP) {
            Node* arg_res = ws.result_stack.back();
            ws.result_stack.pop_back();
            task.curr_app = arena.alloc_node(APP, 0, task.curr_app, arg_res);
            task.arg_idx++;
            if (task.arg_idx < task.args_count) {
                ws.task_stack.push_back(task);
                Thunk* next_arg = ws.args_pool[task.args_start + task.arg_idx];
                ws.task_stack.push_back({NormalizeTask::NORMALIZE, next_arg->node, next_arg->env, task.depth, nullptr, 0, 0, 0});
            } else {
                ws.result_stack.push_back(task.curr_app);
            }
        }
    }
    return ws.result_stack.back();
}

Node* parse_blc(const std::string& bits, size_t& idx, Arena& arena) {
    if (idx >= bits.size()) throw std::runtime_error("End of stream");
    if (bits[idx++] == '0') {
        if (bits[idx++] == '0') return arena.alloc_node(ABS, 0, parse_blc(bits, idx, arena));
        Node* left = parse_blc(bits, idx, arena); return arena.alloc_node(APP, 0, left, parse_blc(bits, idx, arena));
    } else {
        int count = 1; while (idx < bits.size() && bits[idx] == '1') { count++; idx++; }
        idx++; return arena.alloc_node(VAR, count);
    }
}

std::string decode_binary_list(Node* n) {
    std::string res = "";
    while (n && n->kind == ABS && n->l->kind == APP && n->l->l->kind == APP && n->l->l->l->kind == VAR && n->l->l->l->val == 1) {
        Node* H = n->l->l->r;
        if (H && H->kind == ABS && H->l->kind == ABS && H->l->l->kind == VAR) {
            if (H->l->l->val == 2) res += "0"; else if (H->l->l->val == 1) res += "1"; else break;
        } else break;
        n = n->l->r;
    }
    return res;
}

struct EvalResult { bool valid; bool hit_limit; std::string output; };

EvalResult check_program_output(const std::string& prog, Arena& arena, WorkerWorkspace& ws) {
    arena.reset(); size_t idx = 0;
    try {
        Node* p_node = parse_blc(prog, idx, arena);
        if (idx != prog.size()) return {false, false, ""};
        Node* root = arena.alloc_node(APP, 0, p_node, arena.alloc_node(ABS, 0, arena.alloc_node(VAR, 1)));
        uint64_t steps = 0; uint64_t max_steps = 10000000;
        Node* result = normalize(root, nullptr, arena, 0, steps, max_steps, ws);
        if (steps >= max_steps) return {true, true, ""};
        if (result) return {true, false, decode_binary_list(result)};
    } catch (...) { return {false, false, ""}; }
    return {false, false, ""};
}

// --- BATCH MULTI-THREADING COMPONENTS ---
const size_t BATCH_SIZE = 512;

struct ProgramTaskBatch {
    int len;
    std::vector<std::string> programs;
    size_t start_index;
};

struct ProgramResultBatch {
    int len;
    std::vector<std::string> programs;
    std::vector<EvalResult> results;
    size_t start_index;
};

class TaskQueue {
private:
    std::queue<ProgramTaskBatch> tasks;
    std::mutex mutex;
    std::condition_variable cond_pop;
    std::condition_variable cond_push;
    bool done = false;
    const size_t MAX_SIZE = 200;
public:
    void push(ProgramTaskBatch batch) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            cond_push.wait(lock, [&] { return tasks.size() < MAX_SIZE; });
            tasks.push(std::move(batch));
        }
        cond_pop.notify_one();
    }
    bool pop(ProgramTaskBatch& batch) {
        std::unique_lock<std::mutex> lock(mutex);
        cond_pop.wait(lock, [&] { return done || !tasks.empty(); });
        if (tasks.empty()) return false;
        batch = std::move(tasks.front());
        tasks.pop();
        cond_push.notify_one();
        return true;
    }
    void finish() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        cond_pop.notify_all();
        cond_push.notify_all();
    }
};

class ResultCollector {
private:
    std::mutex mutex;
    std::condition_variable cond;
    std::map<size_t, ProgramResultBatch> results;
    size_t next_index = 0;
    bool finished = false;
public:
    void submit(size_t index, ProgramResultBatch result) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            results.emplace(index, std::move(result));
        }
        cond.notify_all();
    }
    bool wait_take_next(ProgramResultBatch& out) {
        std::unique_lock<std::mutex> lock(mutex);
        cond.wait(lock, [&] { return finished || results.count(next_index) != 0; });
        auto it = results.find(next_index);
        if (it == results.end()) return false;
        out = std::move(it->second);
        next_index += out.programs.size();
        results.erase(it);
        return true;
    }
    void finish() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            finished = true;
        }
        cond.notify_all();
    }
};

class ScopedArena {
public:
    Arena arena;
    ScopedArena(size_t max_elems) : arena(max_elems) {}
};

struct WorkerContext {
    TaskQueue* task_queue;
    ResultCollector* collector;
    int id;
};

void* worker_thread_entry(void* arg) {
    WorkerContext* ctx = static_cast<WorkerContext*>(arg);
    ScopedArena local_arena(50000);
    WorkerWorkspace ws;
    ws.eval_stack.reserve(5000);
    ws.args_pool.reserve(1000);
    ws.task_stack.reserve(5000);
    ws.result_stack.reserve(5000);

    ProgramTaskBatch batch;
    while (ctx->task_queue->pop(batch)) {
        ProgramResultBatch res_batch;
        res_batch.len = batch.len;
        res_batch.start_index = batch.start_index;
        res_batch.programs = std::move(batch.programs);
        res_batch.results.resize(res_batch.programs.size());

        for (size_t i = 0; i < res_batch.programs.size(); ++i) {
            res_batch.results[i] = check_program_output(res_batch.programs[i], local_arena.arena, ws);
        }
        ctx->collector->submit(res_batch.start_index, std::move(res_batch));
    }
    delete ctx;
    return nullptr;
}

// --- ROADMAP PARSER FOR PRUNING ---
size_t get_expr_len(const std::string& bits, size_t idx) {
    if (idx >= bits.size()) return 0;
    if (bits[idx] == '0') {
        if (bits[idx+1] == '0') return 2 + get_expr_len(bits, idx + 2);
        size_t l_len = get_expr_len(bits, idx + 2);
        return 2 + l_len + get_expr_len(bits, idx + 2 + l_len);
    } else {
        size_t count = 0;
        while (idx < bits.size() && bits[idx] == '1') { count++; idx++; }
        return count + 1;
    }
}

// --- ROADMAP-GUIDED GENERATOR ---
void generate_dfs(int n, int depth, std::string& current, const std::string& resume_target, bool skipping, const std::function<void()>& on_term) {
    if (n <= 0) return;

    enum ChoiceType { T_ABS, T_APP, T_VAR };
    ChoiceType target_type = T_ABS;
    int target_left_len = 0;

    if (skipping) {
        size_t idx = current.length();
        if (idx >= resume_target.length()) { skipping = false; } 
        else {
            if (resume_target[idx] == '0' && resume_target[idx+1] == '0') target_type = T_ABS;
            else if (resume_target[idx] == '0' && resume_target[idx+1] == '1') {
                target_type = T_APP;
                target_left_len = get_expr_len(resume_target, idx + 2);
            } else target_type = T_VAR;
        }
    }

    if (n >= 2) {
        bool run_abs = true; bool next_skip = false;
        if (skipping) {
            if (target_type == T_ABS) { run_abs = true; next_skip = true; }
            else run_abs = false; 
        }
        if (run_abs) {
            current.append("00");
            generate_dfs(n - 2, depth + 1, current, resume_target, next_skip, on_term);
            current.erase(current.size() - 2);
        }
    }

    if (n >= 2) {
        bool run_app = true;
        if (skipping) {
            if (target_type == T_ABS) run_app = true;
            else if (target_type == T_VAR) run_app = false;
        }
        if (run_app) {
            current.append("01");
            for (int left_len = 1; left_len <= n - 3; ++left_len) {
                bool run_this_len = true; bool next_skip = false;
                if (skipping && target_type == T_APP) {
                    if (left_len < target_left_len) run_this_len = false;
                    else if (left_len == target_left_len) { run_this_len = true; next_skip = true; }
                    else { run_this_len = true; next_skip = false; }
                }
                if (run_this_len) {
                    generate_dfs(left_len, depth, current, resume_target, next_skip, [&]() {
                        generate_dfs(n - 2 - left_len, depth, current, resume_target, next_skip, on_term);
                    });
                }
            }
            current.erase(current.size() - 2);
        }
    }

    for (int k = 1; k <= depth; ++k) {
        if (k + 1 == n) {
            bool run_var = true;
            if (skipping && target_type == T_VAR) run_var = false;
            if (run_var) {
                current.append(k, '1'); current.push_back('0');
                on_term();
                current.erase(current.size() - (k + 1));
            }
        }
    }
}

int main() {
    int max_length = 45;
    
    // Scale to use all available cores
    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4; 
    std::cout << "=> Initializing system using " << num_threads << " worker threads.\n";

    fs::create_directory("blc_dumps");

    int start_len = 1;
    std::string resume_string = "";
    uint64_t resume_pos = 0;

    if (fs::exists("blc_dumps/checkpoint.txt")) {
        std::ifstream cp("blc_dumps/checkpoint.txt");
        cp >> start_len; cp >> resume_string; cp >> resume_pos;
        std::cout << "=> Resuming from Length " << start_len << " (Safe rollback to file position " << resume_pos << " bytes)...\n";
    }

    for (int len = start_len; len <= max_length; ++len) {
        std::string filename = "blc_dumps/" + std::to_string(len) + ".bin";
        if (len == start_len && !resume_string.empty() && fs::exists(filename)) {
            fs::resize_file(filename, resume_pos);
        }

        bool append = (len == start_len && !resume_string.empty());
        std::ofstream outfile(filename, append ? (std::ios::binary | std::ios::app) : std::ios::binary);

        uint8_t current_byte = 0;
        int bit_count = 0;
        auto write_bit = [&](char bit) {
            current_byte = (current_byte << 1) | (bit == '1' ? 1 : 0);
            bit_count++;
            if (bit_count == 8) {
                outfile.put(current_byte);
                current_byte = 0;
                bit_count = 0;
            }
        };

        TaskQueue task_queue;
        ResultCollector collector;

        const size_t STACK_SIZE = 8 * 1024 * 1024; // 8MiB
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, STACK_SIZE);

        std::vector<pthread_t> workers(num_threads);
        for (unsigned int i = 0; i < num_threads; ++i) {
            WorkerContext* ctx = new WorkerContext{&task_queue, &collector, static_cast<int>(i)};
            pthread_create(&workers[i], &attr, worker_thread_entry, ctx);
        }
        pthread_attr_destroy(&attr);

        std::atomic<uint64_t> progs_since_save(0);
        std::string last_saved_program = resume_string;

        auto writer_thread_fn = [&] {
            ProgramResultBatch batch;
            while (collector.wait_take_next(batch)) {
                for (size_t k = 0; k < batch.programs.size(); ++k) {
                    const auto& program = batch.programs[k];
                    const auto& result = batch.results[k];

                    if (result.valid && (result.hit_limit || !result.output.empty())) {
                        for (char c : program) write_bit(c);
                        write_bit(result.hit_limit ? '1' : '0');
                        if (!result.hit_limit) {
                            if (result.output.length() > 2) {
                                std::cout << "[Length " << batch.len << "] " << program << " -> " << result.output << std::endl;
                            }
                            uint32_t out_len = static_cast<uint32_t>(result.output.length());
                            for (int i = 31; i >= 0; --i) {
                                write_bit(((out_len >> i) & 1) ? '1' : '0');
                            }
                            for (char c : result.output) write_bit(c);
                        } else {
                            std::cout << "[Length " << batch.len << "] " << program << " -> Hit max steps" << std::endl;
                        }
                        progs_since_save++;
                        last_saved_program = program;
                    }

                    if (bit_count == 0 && progs_since_save >= 50000) {
                        outfile.flush();
                        uint64_t current_fpos = outfile.tellp();
                        std::ofstream cp_tmp("blc_dumps/checkpoint.tmp");
                        cp_tmp << len << "\n" << last_saved_program << "\n" << current_fpos << "\n";
                        cp_tmp.close();
                        fs::rename("blc_dumps/checkpoint.tmp", "blc_dumps/checkpoint.txt");
                        progs_since_save = 0;
                    }
                }
            }
        };

        std::thread writer_thread(writer_thread_fn);

        std::string program_buffer;
        bool initial_skip = (len == start_len && !resume_string.empty());
        
        ProgramTaskBatch current_batch;
        current_batch.len = len;
        current_batch.start_index = 0;
        current_batch.programs.reserve(BATCH_SIZE);
        size_t total_task_count = 0;

        generate_dfs(len, 0, program_buffer, resume_string, initial_skip, [&] {
            current_batch.programs.push_back(program_buffer);
            total_task_count++;
            if (current_batch.programs.size() == BATCH_SIZE) {
                task_queue.push(std::move(current_batch));
                current_batch.len = len;
                current_batch.start_index = total_task_count;
                current_batch.programs.reserve(BATCH_SIZE);
            }
        });

        if (!current_batch.programs.empty()) {
            task_queue.push(std::move(current_batch));
        }

        task_queue.finish();
        for (auto& t : workers) {
            pthread_join(t, nullptr);
        }

        collector.finish();
        if (writer_thread.joinable()) writer_thread.join();

        if (bit_count > 0) outfile.put(current_byte << (8 - bit_count));
        outfile.close();
        if (!compression::compress_file(filename)) {
            std::cerr << "Error: failed to compress " << filename << "\n";
            return 1;
        }
        resume_string.clear();

        std::ofstream cp_tmp("blc_dumps/checkpoint.tmp");
        cp_tmp << len + 1 << "\n" << "\n" << 0 << "\n";
        cp_tmp.close();
        fs::rename("blc_dumps/checkpoint.tmp", "blc_dumps/checkpoint.txt");
    }
    return 0;
}