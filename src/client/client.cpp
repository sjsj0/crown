// client.cpp — configures all nodes then enters an interactive read/write loop.
//
// Write flow (non-blocking):
//   1. Client assigns a unique request_id, adds it to pending map, fires
//      Write RPC to head and returns immediately to the prompt.
//   2. Head propagates down the chain to tail.
//   3. Tail commits and sends Ack RPC directly to the client's Ack server.
//   4. Ack listener thread removes the request_id from the pending map and
//      prints a confirmation. No retry logic yet.
//
// Read flow:
//   Client sends Read RPC directly to tail and prints the response.
//
// Commands:
//   write <key> <value>
//   read  <key>
//   quit / exit
//   help

#include <iostream>
#include <fstream>
#include <string>
#include <stdexcept>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <random>
#include <chrono>
#include <iomanip>
#include <limits>
#include <memory>

#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>
#include "chain.grpc.pb.h"

using namespace std;
using json = nlohmann::json;

// ============================================================
// Helpers
// ============================================================

static string get_local_ip() {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        return "127.0.0.1";  // fallback
    }

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;  // IPv4
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &result) != 0) {
        return "127.0.0.1";  // fallback
    }

    string ip = "127.0.0.1";  // default fallback
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        if (rp->ai_family == AF_INET) {
            struct sockaddr_in* sa = (struct sockaddr_in*)rp->ai_addr;
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(sa->sin_addr), ip_str, INET_ADDRSTRLEN);
            ip = ip_str;
            break;  // use the first IPv4 address
        }
    }

    freeaddrinfo(result);
    return ip;
}

// ============================================================
// Pending write map — keyed by request_id
// ============================================================
// Written by the main thread when a write is issued.
// Cleared by the Ack listener thread when the tail confirms.

struct PendingWrite {
    uint64_t request_id = 0;
    string   key;
    string   value;
};

static mutex                              g_pending_mtx;
static unordered_map<uint64_t, PendingWrite> g_pending;  // request_id -> PendingWrite

// Monotonically increasing request ID generator.
static atomic<uint64_t> g_next_request_id{1};
static atomic<bool> g_benchmark_mode_active{false};

// ============================================================
// Benchmark helpers
// ============================================================

using SteadyClock = chrono::steady_clock;

struct ThroughputMetricsSummary {
    double duration_sec = 0.0;
    uint64_t writes_sent = 0;
    uint64_t acks_received = 0;
    uint64_t reads_sent = 0;
    uint64_t reads_ok = 0;
    uint64_t read_failures = 0;
    uint64_t write_rpc_failures = 0;
    double ack_writes_per_sec = 0.0;
    double read_requests_per_sec = 0.0;
    double read_responses_per_sec = 0.0;
    double avg_ack_latency_ms = 0.0;
};

struct ThroughputMetricsState {
    atomic<bool> enabled{false};
    SteadyClock::time_point window_start = SteadyClock::now();

    atomic<uint64_t> writes_sent{0};
    atomic<uint64_t> acks_received{0};
    atomic<uint64_t> reads_sent{0};
    atomic<uint64_t> reads_ok{0};
    atomic<uint64_t> read_failures{0};
    atomic<uint64_t> write_rpc_failures{0};

    atomic<uint64_t> ack_latency_samples{0};
    atomic<uint64_t> ack_latency_total_us{0};

    mutex pending_write_times_mtx;
    unordered_map<uint64_t, SteadyClock::time_point> pending_write_times;
};

static mutex g_metrics_state_mtx;
static shared_ptr<ThroughputMetricsState> g_metrics_state;

[[maybe_unused]] static void benchmark_attach_metrics_state(const shared_ptr<ThroughputMetricsState>& state) {
    lock_guard<mutex> lk(g_metrics_state_mtx);
    g_metrics_state = state;
}

[[maybe_unused]] static void benchmark_detach_metrics_state() {
    lock_guard<mutex> lk(g_metrics_state_mtx);
    g_metrics_state.reset();
}

static shared_ptr<ThroughputMetricsState> benchmark_current_metrics_state() {
    lock_guard<mutex> lk(g_metrics_state_mtx);
    return g_metrics_state;
}

[[maybe_unused]] static void benchmark_start_metrics_window(
    ThroughputMetricsState& state,
    SteadyClock::time_point start_time = SteadyClock::now()) {
    state.window_start = start_time;

    state.writes_sent.store(0, memory_order_relaxed);
    state.acks_received.store(0, memory_order_relaxed);
    state.reads_sent.store(0, memory_order_relaxed);
    state.reads_ok.store(0, memory_order_relaxed);
    state.read_failures.store(0, memory_order_relaxed);
    state.write_rpc_failures.store(0, memory_order_relaxed);
    state.ack_latency_samples.store(0, memory_order_relaxed);
    state.ack_latency_total_us.store(0, memory_order_relaxed);

    {
        lock_guard<mutex> lk(state.pending_write_times_mtx);
        state.pending_write_times.clear();
    }

    state.enabled.store(true, memory_order_release);
}

[[maybe_unused]] static void benchmark_stop_metrics_window(ThroughputMetricsState& state) {
    state.enabled.store(false, memory_order_release);
}

[[maybe_unused]] static string benchmark_key_for_index(const string& prefix, uint64_t index) {
    return prefix + to_string(index);
}

[[maybe_unused]] static vector<string> benchmark_build_keyset(const string& prefix, size_t key_count) {
    vector<string> keys;
    keys.reserve(key_count);
    for (size_t i = 0; i < key_count; ++i)
        keys.push_back(benchmark_key_for_index(prefix, i));
    return keys;
}

[[maybe_unused]] static const string& benchmark_select_key_round_robin(
    const vector<string>& keys,
    uint64_t operation_index) {
    if (keys.empty()) throw invalid_argument("benchmark keyset cannot be empty");
    return keys[static_cast<size_t>(operation_index % keys.size())];
}

static void benchmark_note_write_issued(uint64_t request_id) {
    auto state = benchmark_current_metrics_state();
    if (!state || !state->enabled.load(memory_order_relaxed)) return;

    state->writes_sent.fetch_add(1, memory_order_relaxed);
    const auto now = SteadyClock::now();

    lock_guard<mutex> lk(state->pending_write_times_mtx);
    state->pending_write_times[request_id] = now;
}

static void benchmark_note_write_ack(uint64_t request_id) {
    auto state = benchmark_current_metrics_state();
    if (!state || !state->enabled.load(memory_order_relaxed)) return;

    state->acks_received.fetch_add(1, memory_order_relaxed);

    SteadyClock::time_point issued_at{};
    bool found = false;
    {
        lock_guard<mutex> lk(state->pending_write_times_mtx);
        auto it = state->pending_write_times.find(request_id);
        if (it != state->pending_write_times.end()) {
            issued_at = it->second;
            state->pending_write_times.erase(it);
            found = true;
        }
    }

    if (!found) return;

    const auto latency_us = chrono::duration_cast<chrono::microseconds>(SteadyClock::now() - issued_at).count();
    state->ack_latency_total_us.fetch_add(static_cast<uint64_t>(max<int64_t>(0, latency_us)), memory_order_relaxed);
    state->ack_latency_samples.fetch_add(1, memory_order_relaxed);
}

[[maybe_unused]] static void benchmark_note_write_rpc_failure() {
    auto state = benchmark_current_metrics_state();
    if (!state || !state->enabled.load(memory_order_relaxed)) return;
    state->write_rpc_failures.fetch_add(1, memory_order_relaxed);
}

static void benchmark_note_read_sent() {
    auto state = benchmark_current_metrics_state();
    if (!state || !state->enabled.load(memory_order_relaxed)) return;
    state->reads_sent.fetch_add(1, memory_order_relaxed);
}

static void benchmark_note_read_success() {
    auto state = benchmark_current_metrics_state();
    if (!state || !state->enabled.load(memory_order_relaxed)) return;
    state->reads_ok.fetch_add(1, memory_order_relaxed);
}

static void benchmark_note_read_failure() {
    auto state = benchmark_current_metrics_state();
    if (!state || !state->enabled.load(memory_order_relaxed)) return;
    state->read_failures.fetch_add(1, memory_order_relaxed);
}

[[maybe_unused]] static ThroughputMetricsSummary benchmark_build_summary(
    const ThroughputMetricsState& state,
    SteadyClock::time_point end_time = SteadyClock::now()) {
    ThroughputMetricsSummary summary;

    summary.duration_sec = chrono::duration<double>(end_time - state.window_start).count();
    if (summary.duration_sec <= 0.0) summary.duration_sec = 1e-9;

    summary.writes_sent = state.writes_sent.load(memory_order_relaxed);
    summary.acks_received = state.acks_received.load(memory_order_relaxed);
    summary.reads_sent = state.reads_sent.load(memory_order_relaxed);
    summary.reads_ok = state.reads_ok.load(memory_order_relaxed);
    summary.read_failures = state.read_failures.load(memory_order_relaxed);
    summary.write_rpc_failures = state.write_rpc_failures.load(memory_order_relaxed);

    const uint64_t ack_latency_samples = state.ack_latency_samples.load(memory_order_relaxed);
    const uint64_t ack_latency_total_us = state.ack_latency_total_us.load(memory_order_relaxed);

    summary.ack_writes_per_sec = static_cast<double>(summary.acks_received) / summary.duration_sec;
    summary.read_requests_per_sec = static_cast<double>(summary.reads_sent) / summary.duration_sec;
    summary.read_responses_per_sec = static_cast<double>(summary.reads_ok) / summary.duration_sec;

    if (ack_latency_samples > 0) {
        summary.avg_ack_latency_ms =
            (static_cast<double>(ack_latency_total_us) / static_cast<double>(ack_latency_samples)) / 1000.0;
    }
    return summary;
}

[[maybe_unused]] static string benchmark_summary_line(const ThroughputMetricsSummary& summary,
                                                       const string& tag) {
    ostringstream out;
    out << fixed << setprecision(3)
        << "BENCH_SUMMARY"
        << " tag=" << tag
        << " duration_s=" << summary.duration_sec
        << " writes_sent=" << summary.writes_sent
        << " acks_received=" << summary.acks_received
        << " reads_sent=" << summary.reads_sent
        << " reads_ok=" << summary.reads_ok
        << " read_failures=" << summary.read_failures
        << " write_rpc_failures=" << summary.write_rpc_failures
        << " ack_wps=" << summary.ack_writes_per_sec
        << " read_req_rps=" << summary.read_requests_per_sec
        << " read_resp_rps=" << summary.read_responses_per_sec
        << " avg_ack_latency_ms=" << summary.avg_ack_latency_ms;
    return out.str();
}

enum class ClientRunMode {
    INTERACTIVE,
    BENCH_WRITE,
    BENCH_READ,
};

struct BenchmarkRunConfig {
    uint64_t total_ops = 0;
    int key_count = 0;
    int client_index = 0;
    int num_clients = 1;
    int craq_node_id = -1;
    string key_prefix = "bench-key-";
    string value_prefix = "bench-value-";
};

static bool parse_int_text(const string& s, int& out) {
    if (s.empty()) return false;
    try {
        size_t consumed = 0;
        const long long parsed = stoll(s, &consumed, 10);
        if (consumed != s.size()) return false;
        if (parsed < numeric_limits<int>::min() || parsed > numeric_limits<int>::max()) return false;
        out = static_cast<int>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

static bool parse_uint64_text(const string& s, uint64_t& out) {
    if (s.empty()) return false;
    try {
        size_t consumed = 0;
        out = stoull(s, &consumed, 10);
        return consumed == s.size();
    } catch (...) {
        return false;
    }
}

static string mode_name(chain::ReplicationMode mode) {
    switch (mode) {
        case chain::ReplicationMode::CHAIN: return "chain";
        case chain::ReplicationMode::CRAQ: return "craq";
        case chain::ReplicationMode::CROWN: return "crown";
        default: return "unknown";
    }
}

static size_t pending_write_count() {
    lock_guard<mutex> lk(g_pending_mtx);
    return g_pending.size();
}

static bool remove_pending_request(uint64_t request_id) {
    lock_guard<mutex> lk(g_pending_mtx);
    return g_pending.erase(request_id) > 0;
}

static void benchmark_wait_for_pending_acks() {
    auto last_log = SteadyClock::now();
    while (true) {
        const size_t pending = pending_write_count();
        if (pending == 0) return;

        const auto now = SteadyClock::now();
        if (now - last_log >= chrono::seconds(2)) {
            cout << "[Bench] Waiting for " << pending << " pending ack(s)...\n";
            last_log = now;
        }
        this_thread::sleep_for(chrono::milliseconds(5));
    }
}

static void print_usage(const char* bin) {
    cerr << "Usage:\n"
         << "  " << bin << " <config.json> <true/false> [ack_port]\n"
         << "  " << bin << " <config.json> <true/false> [ack_port] bench-write <total_ops> <key_count> <client_index> <num_clients> [key_prefix] [value_prefix]\n"
         << "  " << bin << " <config.json> <true/false> [ack_port] bench-read  <total_ops> <key_count> <client_index> <num_clients> [craq_node_id] [key_prefix]\n";
}

static bool parse_run_mode_args(int argc,
                                char** argv,
                                int& ack_port,
                                ClientRunMode& run_mode,
                                BenchmarkRunConfig& bench_cfg,
                                string& err) {
    ack_port = 60000;
    run_mode = ClientRunMode::INTERACTIVE;

    int argi = 3;
    int maybe_ack_port = 0;
    if (argi < argc && parse_int_text(argv[argi], maybe_ack_port)) {
        ack_port = maybe_ack_port;
        ++argi;
    }

    if (ack_port < 1 || ack_port > 65535) {
        err = "ack_port must be in [1, 65535]";
        return false;
    }

    if (argi >= argc) return true;

    const string mode_arg = argv[argi++];
    const bool is_write = (mode_arg == "bench-write");
    const bool is_read = (mode_arg == "bench-read");
    if (!is_write && !is_read) {
        err = "unknown mode '" + mode_arg + "'";
        return false;
    }

    run_mode = is_write ? ClientRunMode::BENCH_WRITE : ClientRunMode::BENCH_READ;

    if (argc - argi < 4) {
        err = "benchmark mode requires: <total_ops> <key_count> <client_index> <num_clients>";
        return false;
    }

    if (!parse_uint64_text(argv[argi++], bench_cfg.total_ops) || bench_cfg.total_ops == 0) {
        err = "total_ops must be > 0";
        return false;
    }
    if (!parse_int_text(argv[argi++], bench_cfg.key_count) || bench_cfg.key_count <= 0) {
        err = "key_count must be > 0";
        return false;
    }
    if (!parse_int_text(argv[argi++], bench_cfg.client_index) || bench_cfg.client_index < 0) {
        err = "client_index must be >= 0";
        return false;
    }
    if (!parse_int_text(argv[argi++], bench_cfg.num_clients) || bench_cfg.num_clients <= 0) {
        err = "num_clients must be > 0";
        return false;
    }
    if (bench_cfg.client_index >= bench_cfg.num_clients) {
        err = "client_index must be < num_clients";
        return false;
    }

    if (is_write) {
        if (argi < argc) bench_cfg.key_prefix = argv[argi++];
        if (argi < argc) bench_cfg.value_prefix = argv[argi++];
    } else {
        if (argi < argc) {
            int maybe_node_id = -1;
            if (parse_int_text(argv[argi], maybe_node_id)) {
                bench_cfg.craq_node_id = maybe_node_id;
                ++argi;
            }
        }
        if (argi < argc) bench_cfg.key_prefix = argv[argi++];
    }

    if (argi != argc) {
        err = "too many arguments for benchmark mode";
        return false;
    }
    return true;
}

static uint64_t add_pending(const string& key, const string& value) {
    uint64_t id = g_next_request_id.fetch_add(1, memory_order_relaxed);
    lock_guard<mutex> lk(g_pending_mtx);
    g_pending[id] = { id, key, value };
    return id;
}

// Called by the Ack listener thread. Removes from map and prints confirmation.
static void ack_pending(uint64_t request_id, uint64_t version) {
    string key;
    bool found = false;
    {
        lock_guard<mutex> lk(g_pending_mtx);
        auto it = g_pending.find(request_id);
        if (it != g_pending.end()) {
            key = it->second.key;
            g_pending.erase(it);
            found = true;
        }
    }

    if (!found) {
        if (!g_benchmark_mode_active.load(memory_order_relaxed)) {
            cout << "\n[Ack] Received ack for unknown request_id=" << request_id << "\n> " << flush;
        }
        return;
    }

    benchmark_note_write_ack(request_id);
    if (!g_benchmark_mode_active.load(memory_order_relaxed)) {
        cout << "\n[Ack] Write committed: request_id=" << request_id
             << " key='" << key << "'"
             << " version=" << version << "\n> " << flush;
    }
}

// ============================================================
// Client-side Ack service — the tail calls this
// ============================================================

class ClientAckServiceImpl final : public chain::ChainNode::Service {
public:
    grpc::Status Ack(grpc::ServerContext*     /*ctx*/,
                     const chain::AckRequest* req,
                     google::protobuf::Empty* /*resp*/) override {
        ack_pending(req->request_id(), req->version());
        return grpc::Status::OK;
    }

    // Unused by the client-side service.
    grpc::Status Configure(grpc::ServerContext*, const chain::NodeConfig*,
                           google::protobuf::Empty*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "");
    }
    grpc::Status Write(grpc::ServerContext*, const chain::WriteRequest*,
                       chain::WriteResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "");
    }
    grpc::Status Read(grpc::ServerContext*, const chain::ReadRequest*,
                      chain::ReadResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "");
    }
    grpc::Status Propagate(grpc::ServerContext*, const chain::PropagateRequest*,
                           google::protobuf::Empty*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "");
    }
    grpc::Status VersionQuery(grpc::ServerContext*, const chain::VersionQueryRequest*,
                              chain::VersionQueryResponse*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "");
    }
};

// ============================================================
// Helpers — build proto messages from JSON
// ============================================================

static chain::ReplicationMode parse_mode(const string& s) {
    if (s == "chain") return chain::ReplicationMode::CHAIN;
    if (s == "craq")  return chain::ReplicationMode::CRAQ;
    if (s == "crown") return chain::ReplicationMode::CROWN;
    throw invalid_argument("Unknown mode in config: " + s);
}

static chain::NodeAddress parse_addr(const string& s) {
    auto colon = s.rfind(':');
    if (colon == string::npos)
        throw invalid_argument("Expected host:port, got: " + s);
    chain::NodeAddress a;
    a.set_host(s.substr(0, colon));
    a.set_port(stoi(s.substr(colon + 1)));
    return a;
}

static chain::NodeConfig build_node_config(const json& node_json,
                                            chain::ReplicationMode mode,
                                            int crown_node_count,
                                            const string& craq_tail_addr = "") {
    chain::NodeConfig cfg;
    cfg.set_node_id(node_json.at("id").get<int>());
    cfg.set_mode(mode);
    cfg.set_is_head(node_json.value("is_head", false));
    cfg.set_is_tail(node_json.value("is_tail", false));
    if (mode == chain::ReplicationMode::CROWN) {
        // Keep wire compatibility: use head_ranges count to carry ring size.
        for (int i = 0; i < crown_node_count; ++i) {
            (void)cfg.add_head_ranges();
        }
    }
    if (mode == chain::ReplicationMode::CRAQ && !craq_tail_addr.empty()) {
        *cfg.mutable_tail() = parse_addr(craq_tail_addr);
    }

    string host = node_json.at("host").get<string>();
    int    port = node_json.at("port").get<int>();
    chain::NodeAddress self_addr;
    self_addr.set_host(host);
    self_addr.set_port(port);
    *cfg.mutable_self_addr() = self_addr;

    if (!node_json["predecessor"].is_null())
        *cfg.mutable_predecessor() = parse_addr(node_json["predecessor"].get<string>());
    if (!node_json["successor"].is_null())
        *cfg.mutable_successor()   = parse_addr(node_json["successor"].get<string>());
    return cfg;
}

// ============================================================
// Validation
// ============================================================

struct CrownNodeView {
    int id = 0;
    string endpoint, predecessor, successor;
};

static bool validate_minimal_config(const json& nodes, string& error) {
    if (!nodes.is_array() || nodes.empty()) { error = "'nodes' must be a non-empty array"; return false; }
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        try {
            (void)n.at("id").get<int>();
            (void)n.at("host").get<string>();
            (void)n.at("port").get<int>();
            if (n.contains("predecessor") && !n.at("predecessor").is_null())
                (void)parse_addr(n.at("predecessor").get<string>());
            if (n.contains("successor") && !n.at("successor").is_null())
                (void)parse_addr(n.at("successor").get<string>());
        } catch (const exception& ex) {
            error = "invalid node at index " + to_string(i) + ": " + ex.what();
            return false;
        }
    }
    return true;
}

static bool validate_crown_topology(const json& nodes, string& error) {
    vector<CrownNodeView> parsed;
    unordered_map<string, size_t> by_endpoint;
    unordered_map<int, size_t> by_id;

    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        CrownNodeView v;
        v.id       = n.at("id").get<int>();
        v.endpoint = n.at("host").get<string>() + ":" + to_string(n.at("port").get<int>());

        if (by_endpoint.count(v.endpoint)) {
            error = "duplicate endpoint in CROWN config: " + v.endpoint;
            return false;
        }
        if (by_id.count(v.id)) {
            error = "duplicate CROWN node id: " + to_string(v.id);
            return false;
        }

        if (n["predecessor"].is_null() || n["successor"].is_null()) {
            error = "CROWN node " + v.endpoint + " must have both predecessor and successor";
            return false;
        }
        v.predecessor = n.at("predecessor").get<string>();
        v.successor   = n.at("successor").get<string>();

        by_endpoint[v.endpoint] = parsed.size();
        by_id[v.id] = parsed.size();
        parsed.push_back(std::move(v));
    }

    for (size_t expected = 0; expected < parsed.size(); ++expected) {
        if (!by_id.count(static_cast<int>(expected))) {
            error = "CROWN node ids must be contiguous in [0, "
                  + to_string(parsed.size() - 1) + "]";
            return false;
        }
    }

    for (const auto& n : parsed) {
        if (!by_endpoint.count(n.predecessor)) { error = "predecessor " + n.predecessor + " not found"; return false; }
        if (!by_endpoint.count(n.successor))   { error = "successor "   + n.successor   + " not found"; return false; }
        if (parsed[by_endpoint[n.predecessor]].successor != n.endpoint ||
            parsed[by_endpoint[n.successor]].predecessor != n.endpoint) {
            error = "ring inconsistency at " + n.endpoint; return false;
        }
    }

    unordered_set<string> visited;
    string current = parsed.front().endpoint;
    for (size_t step = 0; step < parsed.size(); ++step) {
        if (visited.count(current)) { error = "ring cycle before covering all nodes"; return false; }
        visited.insert(current);
        current = parsed[by_endpoint[current]].successor;
    }
    if (current != parsed.front().endpoint || visited.size() != parsed.size()) {
        error = "ring does not close or is disconnected"; return false;
    }
    return true;
}

static bool validate_config_before_configure(const json& config,
                                             chain::ReplicationMode mode,
                                             string& error) {
    if (!config.contains("nodes")) { error = "missing 'nodes'"; return false; }
    const auto& nodes = config.at("nodes");
    if (!validate_minimal_config(nodes, error)) return false;
    if (mode == chain::ReplicationMode::CROWN) return validate_crown_topology(nodes, error);
    return true;
}

// ============================================================
// Topology
// ============================================================

struct NodeStub {
    int                                id = 0;
    string                             endpoint;
    shared_ptr<grpc::Channel>          channel;
    unique_ptr<chain::ChainNode::Stub> stub;
};

struct Topology {
    chain::ReplicationMode mode;

    // Chain / CRAQ: single head and tail pointer.
    NodeStub* head = nullptr;
    NodeStub* tail = nullptr;

    // All nodes — owns the memory. Pointers above point into this vector,
    // so the vector must not be resized after build.
    vector<NodeStub> nodes;

    // CROWN only: index -> node by node id.
    vector<NodeStub*> crown_nodes_by_index;

    // CROWN: hash key to uint64 and select owner via modulo.
    static uint64_t hash_key(const string& key) {
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : key) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return h;
    }

    // Find the node that owns `key` as head (for writes) in CROWN mode.
    NodeStub* crown_head_for(const string& key) {
        if (crown_nodes_by_index.empty()) return nullptr;
        const size_t head_index = static_cast<size_t>(hash_key(key) % crown_nodes_by_index.size());
        return crown_nodes_by_index[head_index];
    }

    // Find the node that owns `key` as tail (for reads) in CROWN mode.
    NodeStub* crown_tail_for(const string& key) {
        if (crown_nodes_by_index.empty()) return nullptr;
        const size_t head_index = static_cast<size_t>(hash_key(key) % crown_nodes_by_index.size());
        const size_t tail_index = (head_index + crown_nodes_by_index.size() - 1) % crown_nodes_by_index.size();
        return crown_nodes_by_index[tail_index];
    }
};

[[maybe_unused]] static int benchmark_select_craq_node_id(const Topology& topo,
                                                           uint64_t operation_index,
                                                           int preferred_node_id = -1) {
    if (topo.mode != chain::ReplicationMode::CRAQ || topo.nodes.empty()) return -1;

    if (preferred_node_id != -1) {
        for (const auto& node : topo.nodes)
            if (node.id == preferred_node_id) return preferred_node_id;
        return -1;
    }

    const size_t idx = static_cast<size_t>(operation_index % topo.nodes.size());
    return topo.nodes[idx].id;
}

static Topology build_topology(const json& config, chain::ReplicationMode mode) {
    Topology topo;
    topo.mode = mode;
    const auto& jnodes = config.at("nodes");

    for (const auto& n : jnodes) {
        NodeStub ns;
        ns.id       = n.at("id").get<int>();
        ns.endpoint = n.at("host").get<string>() + ":" + to_string(n.at("port").get<int>());
        ns.channel  = grpc::CreateChannel(ns.endpoint, grpc::InsecureChannelCredentials());
        ns.stub     = chain::ChainNode::NewStub(ns.channel);
        topo.nodes.push_back(std::move(ns));
    }

    // Chain / CRAQ: identify the single head and tail by flag.
    // CROWN: head/tail are resolved per-key at request time via crown_head_for / crown_tail_for.
    for (size_t i = 0; i < jnodes.size(); ++i) {
        if (jnodes[i].value("is_head", false)) topo.head = &topo.nodes[i];
        if (jnodes[i].value("is_tail", false)) topo.tail = &topo.nodes[i];
    }

    if (mode == chain::ReplicationMode::CROWN) {
        topo.crown_nodes_by_index.assign(jnodes.size(), nullptr);
        for (size_t i = 0; i < jnodes.size(); ++i) {
            const int id = jnodes[i].at("id").get<int>();
            if (id < 0 || id >= static_cast<int>(jnodes.size())) {
                throw invalid_argument("CROWN node id out of range while building topology");
            }
            if (topo.crown_nodes_by_index[id] != nullptr) {
                throw invalid_argument("duplicate CROWN node id while building topology");
            }
            topo.crown_nodes_by_index[id] = &topo.nodes[i];
        }
    }

    return topo;
}

// ============================================================
// Interactive commands
// ============================================================

// Non-blocking: adds to pending map, fires RPC, returns immediately.
static bool do_write(Topology& topo, const string& key, const string& value,
                     const string& client_addr, bool verbose = true) {
    // Resolve the head node for this key.
    // CHAIN / CRAQ: single static head.
    // CROWN: head index = hash(key) % node_count.
    NodeStub* target_head = nullptr;
    if (topo.mode == chain::ReplicationMode::CROWN) {
        target_head = topo.crown_head_for(key);
        if (!target_head) {
            if (verbose) {
                cerr << "[Write] No CROWN head found for key='" << key << "' "
                     << "(token=" << Topology::hash_key(key) << ")\n";
            }
            return false;
        }
    } else {
        target_head = topo.head;
        if (!target_head) {
            if (verbose) cerr << "[Write] No head node in topology.\n";
            return false;
        }
    }

    uint64_t request_id = add_pending(key, value);
    benchmark_note_write_issued(request_id);

    chain::WriteRequest req;
    req.set_key(key);
    req.set_value(value);
    req.set_version(0);              // head assigns the real version
    req.set_client_addr(client_addr);
    req.set_request_id(request_id);

    // Fire and forget — ack comes asynchronously from the tail.
    chain::WriteResponse resp;
    grpc::ClientContext  ctx;
    grpc::Status status = target_head->stub->Write(&ctx, req, &resp);

    if (!status.ok() || !resp.success()) {
        benchmark_note_write_rpc_failure();
        (void)remove_pending_request(request_id);
        if (verbose) {
            if (!status.ok()) {
                cerr << "[Write] Failed: " << status.error_message() << "\n";
            } else {
                cerr << "[Write] Failed: head returned success=false\n";
            }
        }
        return false;
    }

    if (verbose) {
        cout << "[Write] Sent request_id=" << request_id
             << " key='" << key << "' to head (" << target_head->endpoint << ")\n";
    }
    return true;
}

static bool do_read(Topology& topo, const string& key, int node_id = -1, bool verbose = true) {
    // Resolve the target node for this key.
    // CHAIN: single static tail.
    // CRAQ: specified node_id or random.
    // CROWN: tail index = (head index - 1 + node_count) % node_count.
    NodeStub* target = nullptr;
    if (topo.mode == chain::ReplicationMode::CROWN) {
        target = topo.crown_tail_for(key);
        if (!target) {
            if (verbose) {
                cerr << "[Read] No CROWN tail found for key='" << key << "' "
                     << "(token=" << Topology::hash_key(key) << ")\n";
            }
            return false;
        }
    } else if (topo.mode == chain::ReplicationMode::CRAQ) {
        if (node_id != -1) {
            // Find node with specified id
            for (auto& ns : topo.nodes) {
                if (ns.id == node_id) {
                    target = &ns;
                    break;
                }
            }
            if (!target) {
                if (verbose) cerr << "[Read] Node with id " << node_id << " not found.\n";
                return false;
            }
        } else {
            // Random pick
            if (topo.nodes.empty()) {
                if (verbose) cerr << "[Read] No nodes in topology.\n";
                return false;
            }
            static std::random_device rd;
            static std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, topo.nodes.size() - 1);
            int random_index = dis(gen);
            target = &topo.nodes[random_index];
        }
    } else {
        // CHAIN: use tail
        target = topo.tail;
        if (!target) {
            if (verbose) cerr << "[Read] No tail node in topology.\n";
            return false;
        }
    }

    chain::ReadRequest  req;
    chain::ReadResponse resp;
    grpc::ClientContext ctx;
    req.set_key(key);

    benchmark_note_read_sent();
    grpc::Status status = target->stub->Read(&ctx, req, &resp);
    if (!status.ok()) {
        benchmark_note_read_failure();
        if (verbose) cerr << "[Read] Failed: " << status.error_message() << "\n";
        return false;
    }
    benchmark_note_read_success();

    if (verbose) {
        if (resp.value().empty())
            cout << "[Read] (not found)\n";
        else
            cout << "[Read] key='" << resp.key()
                 << "' value='" << resp.value()
                 << "' version=" << resp.version()
                 << " (via " << target->endpoint << ")\n";
    }
    return true;
}

static ThroughputMetricsSummary run_bench_write(Topology& topo,
                                                const string& client_addr,
                                                const BenchmarkRunConfig& cfg) {
    auto state = make_shared<ThroughputMetricsState>();
    benchmark_attach_metrics_state(state);

    const vector<string> keys = benchmark_build_keyset(cfg.key_prefix, static_cast<size_t>(cfg.key_count));

    benchmark_start_metrics_window(*state);
    uint64_t op_index = 0;
    while (true) {
        const uint64_t global_op = static_cast<uint64_t>(cfg.client_index)
            + static_cast<uint64_t>(cfg.num_clients) * op_index;
        if (global_op >= cfg.total_ops) break;

        const string& key = benchmark_select_key_round_robin(keys, global_op);
        const string value = cfg.value_prefix + to_string(cfg.client_index) + "-" + to_string(op_index);
        (void)do_write(topo, key, value, client_addr, false);
        ++op_index;
    }

    benchmark_wait_for_pending_acks();
    benchmark_stop_metrics_window(*state);
    const ThroughputMetricsSummary summary = benchmark_build_summary(*state);
    benchmark_detach_metrics_state();
    return summary;
}

static ThroughputMetricsSummary run_bench_read(Topology& topo,
                                               const string& client_addr,
                                               const BenchmarkRunConfig& cfg) {
    const vector<string> keys = benchmark_build_keyset(cfg.key_prefix, static_cast<size_t>(cfg.key_count));

    // Seed keys before measuring so benchmark reads can hit previously written values.
    for (size_t i = 0; i < keys.size(); ++i) {
        const string value = cfg.value_prefix + "seed-" + to_string(cfg.client_index) + "-" + to_string(i);
        (void)do_write(topo, keys[i], value, client_addr, false);
    }
    benchmark_wait_for_pending_acks();

    auto state = make_shared<ThroughputMetricsState>();
    benchmark_attach_metrics_state(state);

    benchmark_start_metrics_window(*state);

    uint64_t op_index = 0;
    while (true) {
        const uint64_t global_op = static_cast<uint64_t>(cfg.client_index)
            + static_cast<uint64_t>(cfg.num_clients) * op_index;
        if (global_op >= cfg.total_ops) break;

        const string& key = benchmark_select_key_round_robin(keys, global_op);

        int node_id = -1;
        if (topo.mode == chain::ReplicationMode::CRAQ) {
            node_id = benchmark_select_craq_node_id(topo, global_op, cfg.craq_node_id);
        }
        (void)do_read(topo, key, node_id, false);
        ++op_index;
    }

    benchmark_stop_metrics_window(*state);
    const ThroughputMetricsSummary summary = benchmark_build_summary(*state);
    benchmark_detach_metrics_state();
    return summary;
}

static void print_help() {
    cout << "Commands:\n"
         << "  write <key> <value>  — non-blocking write (ack printed when tail confirms)\n"
         << "  read  <key> [node_id] — read from specified node (CRAQ) or tail (CHAIN/CROWN)\n"
         << "  quit / exit          — exit\n"
         << "  help                 — show this message\n";
}

static void run_interactive_loop(Topology& topo, const string& client_addr) {
    print_help();
    cout << "\n";

    string line;
    while (true) {
        cout << "> ";
        if (!getline(cin, line)) break;

        istringstream iss(line);
        string cmd;
        iss >> cmd;

        if (cmd.empty())                    continue;
        if (cmd == "quit" || cmd == "exit") break;
        if (cmd == "help")                { print_help(); continue; }

        if (cmd == "write") {
            string key, value;
            iss >> key;
            getline(iss >> ws, value);
            if (key.empty() || value.empty()) { cerr << "Usage: write <key> <value>\n"; continue; }
            do_write(topo, key, value, client_addr);

        } else if (cmd == "read") {
            string key;
            iss >> key;
            int node_id = -1;
            if (topo.mode == chain::ReplicationMode::CRAQ) {
                string node_id_str;
                if (iss >> node_id_str) {
                    try {
                        node_id = stoi(node_id_str);
                    } catch (const exception&) {
                        node_id = -1;
                    }
                }
            }
            if (key.empty()) { 
                if (topo.mode == chain::ReplicationMode::CRAQ) {
                    cerr << "Usage: read <key> [node_id]\n"; 
                } else {
                    cerr << "Usage: read <key>\n"; 
                }
                continue; 
            }
            do_read(topo, key, node_id);

        } else {
            cerr << "Unknown command '" << cmd << "'. Type 'help'.\n";
        }
    }

    cout << "[Client] Goodbye.\n";
}

// ============================================================
// main
// ============================================================

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    const string configure_arg = argv[2];
    if (configure_arg != "true" && configure_arg != "false") {
        cerr << "Second argument must be 'true' or 'false'.\n";
        print_usage(argv[0]);
        return 1;
    }
    const bool should_configure = (configure_arg == "true");

    int ack_port = 60000;
    ClientRunMode run_mode = ClientRunMode::INTERACTIVE;
    BenchmarkRunConfig bench_cfg;
    string cli_error;
    if (!parse_run_mode_args(argc, argv, ack_port, run_mode, bench_cfg, cli_error)) {
        cerr << "Argument error: " << cli_error << "\n";
        print_usage(argv[0]);
        return 1;
    }

    const string client_addr = get_local_ip() + ":" + to_string(ack_port);

    // --- Start Ack server in background thread -----------------
    ClientAckServiceImpl ack_service;
    grpc::ServerBuilder  builder;
    builder.AddListeningPort("0.0.0.0:" + to_string(ack_port),
                             grpc::InsecureServerCredentials());
    builder.RegisterService(&ack_service);
    unique_ptr<grpc::Server> ack_server = builder.BuildAndStart();
    if (!ack_server) {
        cerr << "Failed to start Ack server on port " << ack_port << "\n";
        return 1;
    }
    cout << "[Client] Ack server listening on " << client_addr << "\n";
    thread ack_thread([&] { ack_server->Wait(); });

    // --- Load and validate config ------------------------------
    ifstream file(argv[1]);
    if (!file.is_open()) { cerr << "Cannot open config file: " << argv[1] << "\n"; return 1; }

    json config;
    try { file >> config; }
    catch (const json::exception& ex) { cerr << "JSON parse error: " << ex.what() << "\n"; return 1; }

    chain::ReplicationMode mode = parse_mode(config.at("mode").get<string>());
    const int crown_node_count = (mode == chain::ReplicationMode::CROWN)
        ? static_cast<int>(config.at("nodes").size())
        : 0;

    string validation_error;
    if (!validate_config_before_configure(config, mode, validation_error)) {
        cerr << "[Client] Config validation failed: " << validation_error << "\n";
        return 1;
    }

    string craq_tail_addr;
    if (mode == chain::ReplicationMode::CRAQ) {
        for (const auto& node_json : config.at("nodes")) {
            if (node_json.value("is_tail", false)) {
                craq_tail_addr = node_json.at("host").get<string>() + ":"
                              + to_string(node_json.at("port").get<int>());
                break;
            }
        }
        if (craq_tail_addr.empty()) {
            cerr << "[Client] CRAQ config must include a tail node with is_tail=true\n";
            return 1;
        }
    }

    // --- Configure all nodes -----------------------------------
    int failures = 0;
    if (should_configure) {
        for (const auto& node_json : config.at("nodes")) {
            string target = node_json.at("host").get<string>() + ":"
                            + to_string(node_json.at("port").get<int>());
            chain::NodeConfig cfg = build_node_config(node_json, mode, crown_node_count);
            auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
            auto stub    = chain::ChainNode::NewStub(channel);
            google::protobuf::Empty resp;
            grpc::ClientContext     ctx;
            grpc::Status status = stub->Configure(&ctx, cfg, &resp);
            if (status.ok())
                cout << "[Client] Configured node " << cfg.node_id() << " at " << target << "\n";
            else {
                cerr << "[Client] Failed to configure node " << cfg.node_id()
                    << " at " << target << ": " << status.error_message() << "\n";
                ++failures;
            }
        }
    } else {
        cout << "[Client] Skipping Configure RPCs for this run.\n";
    }

    if (failures > 0) {
        cerr << "[Client] " << failures << " node(s) failed to configure.\n";
        ack_server->Shutdown();
        ack_thread.join();
        return 1;
    }

    cout << "[Client] All nodes configured successfully.\n\n";

    // --- Run mode ----------------------------------------------
    Topology topo = build_topology(config, mode);
    if (run_mode == ClientRunMode::INTERACTIVE) {
        run_interactive_loop(topo, client_addr);
    } else {
        g_benchmark_mode_active.store(true, memory_order_release);

        const bool is_write = (run_mode == ClientRunMode::BENCH_WRITE);
        cout << "[Client] Running " << (is_write ? "bench-write" : "bench-read")
             << " mode=" << mode_name(mode)
             << " total_ops=" << bench_cfg.total_ops
             << " key_count=" << bench_cfg.key_count
             << " client_index=" << bench_cfg.client_index
             << " num_clients=" << bench_cfg.num_clients;
        if (!is_write && mode == chain::ReplicationMode::CRAQ) {
            cout << " craq_node_id=" << bench_cfg.craq_node_id;
        }
        cout << "\n";

        ThroughputMetricsSummary summary = is_write
            ? run_bench_write(topo, client_addr, bench_cfg)
            : run_bench_read(topo, client_addr, bench_cfg);

        const string tag = string(is_write ? "bench-write" : "bench-read")
            + ":" + mode_name(mode)
            + ":c" + to_string(bench_cfg.client_index)
            + "/" + to_string(bench_cfg.num_clients);
        cout << benchmark_summary_line(summary, tag) << "\n";

        g_benchmark_mode_active.store(false, memory_order_release);
    }

    // --- Shutdown ----------------------------------------------
    ack_server->Shutdown();
    ack_thread.join();
    return 0;
}