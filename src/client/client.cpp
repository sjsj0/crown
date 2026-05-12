// client.cpp — benchmark / interactive workload driver.
//
// Topology comes from the metadata store: on startup the client calls
// MetadataStore.GetCluster on <metadata_host:port> and builds its routing
// tables from the response. It never reads config files and never configures
// nodes — that is the metadata_server's job.
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
#include <string>
#include <stdexcept>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <atomic>
#include <random>
#include <chrono>
#include <iomanip>
#include <limits>
#include <memory>
#include <algorithm>

#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <grpcpp/grpcpp.h>
#include "chain.grpc.pb.h"

using namespace std;

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
    double avg_read_latency_ms = 0.0;
    double p50_ack_latency_ms = 0.0;
    double p95_ack_latency_ms = 0.0;
    double p99_ack_latency_ms = 0.0;
    double p50_read_latency_ms = 0.0;
    double p95_read_latency_ms = 0.0;
    double p99_read_latency_ms = 0.0;
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
    atomic<uint64_t> read_latency_samples{0};
    atomic<uint64_t> read_latency_total_us{0};

    mutable mutex ack_latency_values_mtx;
    vector<uint64_t> ack_latency_values_us;
    mutable mutex read_latency_values_mtx;
    vector<uint64_t> read_latency_values_us;

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
    state.read_latency_samples.store(0, memory_order_relaxed);
    state.read_latency_total_us.store(0, memory_order_relaxed);

    {
        lock_guard<mutex> lk(state.pending_write_times_mtx);
        state.pending_write_times.clear();
    }
    {
        lock_guard<mutex> lk(state.ack_latency_values_mtx);
        state.ack_latency_values_us.clear();
    }
    {
        lock_guard<mutex> lk(state.read_latency_values_mtx);
        state.read_latency_values_us.clear();
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
    const uint64_t bounded_latency_us = static_cast<uint64_t>(max<int64_t>(0, latency_us));
    state->ack_latency_total_us.fetch_add(bounded_latency_us, memory_order_relaxed);
    state->ack_latency_samples.fetch_add(1, memory_order_relaxed);
    {
        lock_guard<mutex> lk(state->ack_latency_values_mtx);
        state->ack_latency_values_us.push_back(bounded_latency_us);
    }
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

static void benchmark_note_read_success(SteadyClock::time_point issued_at) {
    auto state = benchmark_current_metrics_state();
    if (!state || !state->enabled.load(memory_order_relaxed)) return;

    state->reads_ok.fetch_add(1, memory_order_relaxed);

    const auto latency_us = chrono::duration_cast<chrono::microseconds>(SteadyClock::now() - issued_at).count();
    const uint64_t bounded_latency_us = static_cast<uint64_t>(max<int64_t>(0, latency_us));
    state->read_latency_total_us.fetch_add(bounded_latency_us, memory_order_relaxed);
    state->read_latency_samples.fetch_add(1, memory_order_relaxed);
    {
        lock_guard<mutex> lk(state->read_latency_values_mtx);
        state->read_latency_values_us.push_back(bounded_latency_us);
    }
}

static void benchmark_note_read_failure() {
    auto state = benchmark_current_metrics_state();
    if (!state || !state->enabled.load(memory_order_relaxed)) return;
    state->read_failures.fetch_add(1, memory_order_relaxed);
}

static double benchmark_latency_percentile_ms(vector<uint64_t> samples_us, uint64_t percentile) {
    if (samples_us.empty()) return 0.0;
    sort(samples_us.begin(), samples_us.end());
    const uint64_t rank = max<uint64_t>(
        1ULL,
        (percentile * static_cast<uint64_t>(samples_us.size()) + 99ULL) / 100ULL);
    const size_t index = static_cast<size_t>(
        min<uint64_t>(static_cast<uint64_t>(samples_us.size() - 1), rank - 1ULL));
    return static_cast<double>(samples_us[index]) / 1000.0;
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
    const uint64_t read_latency_samples = state.read_latency_samples.load(memory_order_relaxed);
    const uint64_t read_latency_total_us = state.read_latency_total_us.load(memory_order_relaxed);
    vector<uint64_t> ack_latency_values_us;
    vector<uint64_t> read_latency_values_us;
    {
        lock_guard<mutex> lk(state.ack_latency_values_mtx);
        ack_latency_values_us = state.ack_latency_values_us;
    }
    {
        lock_guard<mutex> lk(state.read_latency_values_mtx);
        read_latency_values_us = state.read_latency_values_us;
    }

    summary.ack_writes_per_sec = static_cast<double>(summary.acks_received) / summary.duration_sec;
    summary.read_requests_per_sec = static_cast<double>(summary.reads_sent) / summary.duration_sec;
    summary.read_responses_per_sec = static_cast<double>(summary.reads_ok) / summary.duration_sec;

    if (ack_latency_samples > 0) {
        summary.avg_ack_latency_ms =
            (static_cast<double>(ack_latency_total_us) / static_cast<double>(ack_latency_samples)) / 1000.0;
    }
    if (read_latency_samples > 0) {
        summary.avg_read_latency_ms =
            (static_cast<double>(read_latency_total_us) / static_cast<double>(read_latency_samples)) / 1000.0;
    }
    summary.p50_ack_latency_ms = benchmark_latency_percentile_ms(ack_latency_values_us, 50);
    summary.p95_ack_latency_ms = benchmark_latency_percentile_ms(ack_latency_values_us, 95);
    summary.p99_ack_latency_ms = benchmark_latency_percentile_ms(ack_latency_values_us, 99);
    summary.p50_read_latency_ms = benchmark_latency_percentile_ms(read_latency_values_us, 50);
    summary.p95_read_latency_ms = benchmark_latency_percentile_ms(read_latency_values_us, 95);
    summary.p99_read_latency_ms = benchmark_latency_percentile_ms(read_latency_values_us, 99);
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
        << " avg_ack_latency_ms=" << summary.avg_ack_latency_ms
        << " avg_read_latency_ms=" << summary.avg_read_latency_ms
        << " p50_ack_latency_ms=" << summary.p50_ack_latency_ms
        << " p95_ack_latency_ms=" << summary.p95_ack_latency_ms
        << " p99_ack_latency_ms=" << summary.p99_ack_latency_ms
        << " p50_read_latency_ms=" << summary.p50_read_latency_ms
        << " p95_read_latency_ms=" << summary.p95_read_latency_ms
        << " p99_read_latency_ms=" << summary.p99_read_latency_ms;
    return out.str();
}

enum class ClientRunMode {
    INTERACTIVE,
    BENCH_WRITE,
    BENCH_READ,
};

struct BenchmarkRunConfig {
    // Number of benchmark operations this client process should issue.
    uint64_t ops_per_client = 0;
    int key_count = 0;
    int client_index = 0;
    int num_clients = 1;
    int craq_node_id = -1;
    string key_prefix = "bench-key-";
    string value_prefix = "bench-value-";
    int crown_hot_head_pct = 0;
    int read_hot_key_pct = 0;
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
         << "  " << bin << " <metadata_host:port> [ack_port]\n"
         << "  " << bin << " <metadata_host:port> [ack_port] bench-write <ops_per_client> <key_count> <client_index> <num_clients> [key_prefix] [value_prefix] [hot=<0-100>]\n"
         << "  " << bin << " <metadata_host:port> [ack_port] bench-read  <ops_per_client> <key_count> <client_index> <num_clients> [craq_node_id] [key_prefix] [hot=<0-100>]\n";
}

static bool parse_run_mode_args(int argc,
                                char** argv,
                                int& ack_port,
                                ClientRunMode& run_mode,
                                BenchmarkRunConfig& bench_cfg,
                                string& err) {
    ack_port = 60000;
    run_mode = ClientRunMode::INTERACTIVE;

    // argv[1] is <metadata_host:port>; optional ack_port and bench args follow.
    int argi = 2;
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
        err = "benchmark mode requires: <ops_per_client> <key_count> <client_index> <num_clients>";
        return false;
    }

    if (!parse_uint64_text(argv[argi++], bench_cfg.ops_per_client) || bench_cfg.ops_per_client == 0) {
        err = "ops_per_client must be > 0";
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
        bool key_prefix_set = false;
        bool value_prefix_set = false;
        bool hot_pct_set = false;

        for (; argi < argc; ++argi) {
            const string token = argv[argi];

            auto try_parse_hot_pct = [&](int* out_pct) -> bool {
                const string short_prefix = "hot=";
                const string long_prefix = "crown_hot_head_pct=";

                string value_text;
                if (token.rfind(short_prefix, 0) == 0) {
                    value_text = token.substr(short_prefix.size());
                } else if (token.rfind(long_prefix, 0) == 0) {
                    value_text = token.substr(long_prefix.size());
                } else {
                    return false;
                }

                int parsed = 0;
                if (!parse_int_text(value_text, parsed) || parsed < 0 || parsed > 100) {
                    err = "crown_hot_head_pct must be in [0, 100]";
                    return false;
                }
                *out_pct = parsed;
                return true;
            };

            if (!hot_pct_set) {
                int parsed_hot_pct = 0;
                if (try_parse_hot_pct(&parsed_hot_pct)) {
                    bench_cfg.crown_hot_head_pct = parsed_hot_pct;
                    hot_pct_set = true;
                    continue;
                }
                if (!err.empty()) return false;
            }

            if (!key_prefix_set) {
                bench_cfg.key_prefix = token;
                key_prefix_set = true;
                continue;
            }

            if (!value_prefix_set) {
                bench_cfg.value_prefix = token;
                value_prefix_set = true;
                continue;
            }

            err = "too many arguments for benchmark write mode";
            return false;
        }
    } else {
        bool key_prefix_set = false;
        bool hot_pct_set = false;

        if (argi < argc) {
            int maybe_node_id = -1;
            if (parse_int_text(argv[argi], maybe_node_id)) {
                bench_cfg.craq_node_id = maybe_node_id;
                ++argi;
            }
        }

        for (; argi < argc; ++argi) {
            const string token = argv[argi];

            auto try_parse_hot_pct = [&](int* out_pct) -> bool {
                const string short_prefix = "hot=";
                const string long_prefix = "read_hot_key_pct=";

                string value_text;
                if (token.rfind(short_prefix, 0) == 0) {
                    value_text = token.substr(short_prefix.size());
                } else if (token.rfind(long_prefix, 0) == 0) {
                    value_text = token.substr(long_prefix.size());
                } else {
                    return false;
                }

                int parsed = 0;
                if (!parse_int_text(value_text, parsed) || parsed < 0 || parsed > 100) {
                    err = "read_hot_key_pct must be in [0, 100]";
                    return false;
                }
                *out_pct = parsed;
                return true;
            };

            if (!hot_pct_set) {
                int parsed_hot_pct = 0;
                if (try_parse_hot_pct(&parsed_hot_pct)) {
                    bench_cfg.read_hot_key_pct = parsed_hot_pct;
                    hot_pct_set = true;
                    continue;
                }
                if (!err.empty()) return false;
            }

            if (!key_prefix_set) {
                bench_cfg.key_prefix = token;
                key_prefix_set = true;
                continue;
            }

            err = "too many arguments for benchmark read mode";
            return false;
        }
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
// Topology source — fetched from the metadata store
// ============================================================

static bool fetch_cluster_state(const string& metadata_addr,
                                chain::ClusterState& out,
                                string& error) {
    auto channel = grpc::CreateChannel(metadata_addr, grpc::InsecureChannelCredentials());
    auto stub    = chain::MetadataStore::NewStub(channel);
    google::protobuf::Empty req;
    grpc::ClientContext ctx;
    ctx.set_deadline(chrono::system_clock::now() + chrono::seconds(5));
    const grpc::Status st = stub->GetCluster(&ctx, req, &out);
    if (!st.ok()) { error = st.error_message(); return false; }
    if (out.nodes_size() == 0) { error = "metadata store returned an empty cluster"; return false; }
    return true;
}

// ============================================================
// Topology
// ============================================================

struct NodeStub {
    int                                id = 0;
    string                             endpoint;
    shared_ptr<grpc::Channel>          channel;
    // shared_ptr so callers can copy the handle out from under the topology
    // lock and use it safely even if the topology is refreshed mid-RPC.
    shared_ptr<chain::ChainNode::Stub> stub;
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

struct BenchmarkTopologyKeyset {
    vector<string> keys;

    // CROWN only. These point into `keys` and let the benchmark issue requests
    // evenly across ring heads even when key_count is not divisible by ring size.
    vector<vector<size_t>> crown_key_indices_by_head;
    vector<size_t> active_crown_head_indices;
};

static BenchmarkTopologyKeyset benchmark_build_topology_keyset(
    const Topology& topo,
    const string& prefix,
    size_t key_count) {
    BenchmarkTopologyKeyset keyset;

    if (topo.mode != chain::ReplicationMode::CROWN || topo.crown_nodes_by_index.empty()) {
        keyset.keys = benchmark_build_keyset(prefix, key_count);
        return keyset;
    }

    const size_t ring_size = topo.crown_nodes_by_index.size();
    vector<size_t> desired_per_head(ring_size, key_count / ring_size);
    for (size_t i = 0; i < key_count % ring_size; ++i) {
        desired_per_head[i] += 1;
    }

    vector<vector<string>> keys_by_head(ring_size);
    size_t selected = 0;
    uint64_t candidate_index = 0;
    const uint64_t max_candidates = max<uint64_t>(
        1000000ULL,
        static_cast<uint64_t>(max<size_t>(key_count, 1)) * static_cast<uint64_t>(ring_size) * 1000ULL);

    while (selected < key_count) {
        if (candidate_index > max_candidates) {
            throw runtime_error("benchmark keyset generation failed: unable to balance CROWN keys");
        }

        string key = benchmark_key_for_index(prefix, candidate_index++);
        const size_t head_index = static_cast<size_t>(Topology::hash_key(key) % ring_size);
        if (keys_by_head[head_index].size() >= desired_per_head[head_index]) {
            continue;
        }

        keys_by_head[head_index].push_back(std::move(key));
        ++selected;
    }

    keyset.keys.reserve(key_count);
    keyset.crown_key_indices_by_head.resize(ring_size);

    for (size_t depth = 0; keyset.keys.size() < key_count; ++depth) {
        bool added = false;
        for (size_t head_index = 0; head_index < ring_size; ++head_index) {
            if (depth >= keys_by_head[head_index].size()) continue;
            const size_t key_index = keyset.keys.size();
            keyset.keys.push_back(keys_by_head[head_index][depth]);
            keyset.crown_key_indices_by_head[head_index].push_back(key_index);
            added = true;
            if (keyset.keys.size() == key_count) break;
        }
        if (!added) {
            throw runtime_error("benchmark keyset generation failed: CROWN key buckets exhausted early");
        }
    }

    for (size_t head_index = 0; head_index < ring_size; ++head_index) {
        if (!keyset.crown_key_indices_by_head[head_index].empty()) {
            keyset.active_crown_head_indices.push_back(head_index);
        }
    }

    return keyset;
}

static const string& benchmark_select_crown_key_round_robin(
    const BenchmarkTopologyKeyset& keyset,
    uint64_t operation_index,
    uint64_t client_offset) {
    if (keyset.active_crown_head_indices.empty()) {
        return benchmark_select_key_round_robin(keyset.keys, operation_index);
    }

    const size_t active_count = keyset.active_crown_head_indices.size();
    const size_t active_pos =
        static_cast<size_t>((operation_index + client_offset) % active_count);
    const size_t head_index = keyset.active_crown_head_indices[active_pos];
    const auto& key_indices = keyset.crown_key_indices_by_head[head_index];
    if (key_indices.empty()) {
        throw runtime_error("benchmark CROWN key distribution has an empty active bucket");
    }

    const uint64_t head_round = operation_index / static_cast<uint64_t>(active_count);
    const size_t key_pos =
        static_cast<size_t>((head_round + client_offset) % key_indices.size());
    return keyset.keys[key_indices[key_pos]];
}

static NodeStub* resolve_read_target(Topology& topo,
                                     const string& key,
                                     int node_id,
                                     bool verbose);

struct PreparedBenchmarkWrite {
    NodeStub* target_head = nullptr;
    string key;
    string value;
};

static vector<PreparedBenchmarkWrite> benchmark_prepare_write_batch(
    Topology& topo,
    const BenchmarkRunConfig& cfg) {
    const BenchmarkTopologyKeyset keyset = benchmark_build_topology_keyset(
        topo,
        cfg.key_prefix,
        static_cast<size_t>(cfg.key_count));
    const vector<string>& keys = keyset.keys;

    const bool crown_hotspot_enabled =
        topo.mode == chain::ReplicationMode::CROWN && cfg.crown_hot_head_pct > 0;

    int hot_head_id = -1;
    vector<size_t> hot_key_indices;
    vector<size_t> cold_key_indices;
    size_t hot_cursor = 0;
    size_t cold_cursor = 0;
    uint64_t planned_hot_ops = 0;

    if (crown_hotspot_enabled) {
        NodeStub* hot_head = topo.crown_head_for(keys.front());
        if (!hot_head) {
            throw runtime_error("bench-write prepare failed: unable to resolve CROWN hotspot head");
        }
        hot_head_id = hot_head->id;

        hot_key_indices.reserve(keys.size());
        cold_key_indices.reserve(keys.size());
        for (size_t i = 0; i < keys.size(); ++i) {
            NodeStub* key_head = topo.crown_head_for(keys[i]);
            if (!key_head) continue;
            if (key_head->id == hot_head_id) {
                hot_key_indices.push_back(i);
            } else {
                cold_key_indices.push_back(i);
            }
        }

        if (hot_key_indices.empty()) {
            throw runtime_error("bench-write prepare failed: no keys map to selected CROWN hotspot head");
        }
    }

    vector<PreparedBenchmarkWrite> prepared;
    prepared.reserve(static_cast<size_t>(cfg.ops_per_client));

    for (uint64_t op_index = 0; op_index < cfg.ops_per_client; ++op_index) {
        PreparedBenchmarkWrite next;

        if (crown_hotspot_enabled) {
            const bool want_hot =
                (cfg.crown_hot_head_pct >= 100)
                    ? true
                    : ((op_index % 100ULL) < static_cast<uint64_t>(cfg.crown_hot_head_pct));

            size_t key_idx = 0;
            if (want_hot && !hot_key_indices.empty()) {
                key_idx = hot_key_indices[hot_cursor++ % hot_key_indices.size()];
                ++planned_hot_ops;
            } else if (!want_hot && !cold_key_indices.empty()) {
                key_idx = cold_key_indices[cold_cursor++ % cold_key_indices.size()];
            } else if (!hot_key_indices.empty()) {
                key_idx = hot_key_indices[hot_cursor++ % hot_key_indices.size()];
                ++planned_hot_ops;
            } else if (!cold_key_indices.empty()) {
                key_idx = cold_key_indices[cold_cursor++ % cold_key_indices.size()];
            } else {
                throw runtime_error("bench-write prepare failed: no key candidates for CROWN hotspot mix");
            }

            next.key = keys[key_idx];
        } else if (topo.mode == chain::ReplicationMode::CROWN) {
            next.key = benchmark_select_crown_key_round_robin(
                keyset,
                op_index,
                static_cast<uint64_t>(cfg.client_index));
        } else {
            next.key = benchmark_select_key_round_robin(keys, op_index);
        }

        next.value = cfg.value_prefix + to_string(cfg.client_index) + "-" + to_string(op_index);

        if (topo.mode == chain::ReplicationMode::CROWN) {
            next.target_head = topo.crown_head_for(next.key);
            if (!next.target_head) {
                throw runtime_error("bench-write prepare failed: no CROWN head for key='" + next.key + "'");
            }
        } else {
            next.target_head = topo.head;
            if (!next.target_head) {
                throw runtime_error("bench-write prepare failed: no head node in topology");
            }
        }

        prepared.push_back(std::move(next));
    }

    if (crown_hotspot_enabled) {
        const double effective_hot_pct = prepared.empty()
            ? 0.0
            : (100.0 * static_cast<double>(planned_hot_ops) / static_cast<double>(prepared.size()));

        cout << fixed << setprecision(3)
             << "BENCH_CROWN_HOTSPOT"
             << " client_index=" << cfg.client_index
             << " num_clients=" << cfg.num_clients
             << " hot_head_id=" << hot_head_id
             << " requested_hot_pct=" << cfg.crown_hot_head_pct
             << " effective_hot_pct=" << effective_hot_pct
             << " hot_key_count=" << hot_key_indices.size()
             << " cold_key_count=" << cold_key_indices.size()
             << "\n";
    }

    return prepared;
}

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

struct PreparedBenchmarkRead {
    NodeStub* target = nullptr;
    string key;
};

static vector<PreparedBenchmarkRead> benchmark_prepare_read_batch(
    Topology& topo,
    const BenchmarkRunConfig& cfg) {
    const BenchmarkTopologyKeyset keyset = benchmark_build_topology_keyset(
        topo,
        cfg.key_prefix,
        static_cast<size_t>(cfg.key_count));
    const vector<string>& keys = keyset.keys;

    const bool read_hotspot_enabled = cfg.read_hot_key_pct > 0;
    const size_t hot_key_idx = 0;
    vector<size_t> cold_key_indices;
    size_t cold_cursor = 0;
    uint64_t planned_hot_ops = 0;

    if (read_hotspot_enabled) {
        cold_key_indices.reserve(keys.size());
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i == hot_key_idx) continue;
            cold_key_indices.push_back(i);
        }
    }

    vector<PreparedBenchmarkRead> prepared;
    prepared.reserve(static_cast<size_t>(cfg.ops_per_client));

    for (uint64_t op_index = 0; op_index < cfg.ops_per_client; ++op_index) {
        PreparedBenchmarkRead next;
        if (read_hotspot_enabled) {
            const bool want_hot =
                (cfg.read_hot_key_pct >= 100)
                    ? true
                    : ((op_index % 100ULL) < static_cast<uint64_t>(cfg.read_hot_key_pct));

            if (want_hot || cold_key_indices.empty()) {
                next.key = keys[hot_key_idx];
                ++planned_hot_ops;
            } else {
                const size_t cold_key_idx = cold_key_indices[cold_cursor++ % cold_key_indices.size()];
                next.key = keys[cold_key_idx];
            }
        } else if (topo.mode == chain::ReplicationMode::CROWN) {
            next.key = benchmark_select_crown_key_round_robin(
                keyset,
                op_index,
                static_cast<uint64_t>(cfg.client_index));
        } else {
            next.key = benchmark_select_key_round_robin(keys, op_index);
        }

        int node_id = -1;
        if (topo.mode == chain::ReplicationMode::CRAQ) {
            node_id = benchmark_select_craq_node_id(topo, op_index, cfg.craq_node_id);
        }

        next.target = resolve_read_target(topo, next.key, node_id, false);
        if (!next.target) {
            throw runtime_error("bench-read prepare failed: no target for key='" + next.key + "'");
        }

        prepared.push_back(std::move(next));
    }

    if (read_hotspot_enabled) {
        const double effective_hot_pct = prepared.empty()
            ? 0.0
            : (100.0 * static_cast<double>(planned_hot_ops) / static_cast<double>(prepared.size()));

        cout << fixed << setprecision(3)
             << "BENCH_READ_HOTSPOT"
             << " client_index=" << cfg.client_index
             << " num_clients=" << cfg.num_clients
             << " hot_key='" << keys[hot_key_idx] << "'"
             << " requested_hot_pct=" << cfg.read_hot_key_pct
             << " effective_hot_pct=" << effective_hot_pct
             << "\n";
    }

    return prepared;
}

static Topology build_topology(const chain::ClusterState& cs) {
    Topology topo;
    topo.mode = cs.mode();

    topo.nodes.reserve(static_cast<size_t>(cs.nodes_size()));
    for (const auto& n : cs.nodes()) {
        NodeStub ns;
        ns.id       = n.node_id();
        ns.endpoint = n.addr().host() + ":" + to_string(n.addr().port());
        ns.channel  = grpc::CreateChannel(ns.endpoint, grpc::InsecureChannelCredentials());
        ns.stub     = chain::ChainNode::NewStub(ns.channel);
        topo.nodes.push_back(std::move(ns));
    }

    // Chain / CRAQ: identify the single head and tail by flag.
    // CROWN: head/tail are resolved per-key at request time via crown_head_for / crown_tail_for.
    for (int i = 0; i < cs.nodes_size(); ++i) {
        if (cs.nodes(i).is_head()) topo.head = &topo.nodes[i];
        if (cs.nodes(i).is_tail()) topo.tail = &topo.nodes[i];
    }

    if (topo.mode == chain::ReplicationMode::CROWN) {
        topo.crown_nodes_by_index.assign(static_cast<size_t>(cs.nodes_size()), nullptr);
        for (int i = 0; i < cs.nodes_size(); ++i) {
            const int id = cs.nodes(i).node_id();
            if (id < 0 || id >= cs.nodes_size()) {
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
// Topology refresh — single-flight + throttle to prevent storms
// ============================================================
//
// When a Write/Read RPC fails with FAILED_PRECONDITION (frozen) or
// UNAVAILABLE (dead node), the client refreshes its topology view.
// Concurrent benchmark workers all see failures at once during reconfig;
// without protection they would all hammer the metadata server.
//
//   Single-flight: only one thread does the actual GetCluster RPC.
//   Other concurrent callers wait on a condition variable and reuse the
//   refreshed topology.
//
//   Throttle: refreshes within kRefreshMinIntervalMs of the last refresh
//   are coalesced — return immediately with the existing topology.
//
// The topology is mutated in-place under g_topo_mtx. RPC handlers must
// copy out the stub shared_ptr under shared_lock(g_topo_mtx) before
// releasing the lock — that way the stub stays valid even if the
// topology is replaced mid-RPC.

static std::shared_mutex             g_topo_mtx;
static std::mutex                    g_refresh_state_mtx;
static std::condition_variable       g_refresh_cv;
static bool                          g_refresh_active = false;
static std::chrono::steady_clock::time_point g_last_refresh
        = std::chrono::steady_clock::time_point::min();
static std::string                   g_metadata_addr_cached;

static constexpr int kRefreshMinIntervalMs = 500;

// Single-flight + throttled refresh. Returns true if topology was refreshed
// (or was already fresh); false on hard fetch failure.
static bool refresh_topology(Topology& topo) {
    using namespace std::chrono;
    std::unique_lock<std::mutex> rlk(g_refresh_state_mtx);

    // Another thread is currently refreshing — wait for it and reuse the result.
    if (g_refresh_active) {
        g_refresh_cv.wait(rlk, []{ return !g_refresh_active; });
        return true;
    }

    // Throttle: if a refresh completed recently, don't issue another one.
    if (duration_cast<milliseconds>(steady_clock::now() - g_last_refresh).count()
        < kRefreshMinIntervalMs) {
        return true;
    }

    g_refresh_active = true;
    rlk.unlock();

    // Do the actual fetch outside any lock.
    chain::ClusterState cluster;
    string err;
    bool ok = fetch_cluster_state(g_metadata_addr_cached, cluster, err);

    if (ok) {
        Topology new_topo = build_topology(cluster);
        std::unique_lock<std::shared_mutex> tlk(g_topo_mtx);
        topo = std::move(new_topo);
        tlk.unlock();
        cout << "[Client] Topology refreshed: " << cluster.nodes_size() << " nodes\n";
    } else {
        cerr << "[Client] Topology refresh failed: " << err << "\n";
    }

    rlk.lock();
    g_last_refresh = steady_clock::now();
    g_refresh_active = false;
    rlk.unlock();
    g_refresh_cv.notify_all();

    return ok;
}

// Returns true if the status indicates the client should refresh topology
// and retry (rather than fail immediately).
static bool should_refresh_on_status(const grpc::Status& status) {
    if (status.error_code() == grpc::StatusCode::UNAVAILABLE) return true;
    if (status.error_code() == grpc::StatusCode::FAILED_PRECONDITION) {
        // "node frozen" — reconfig in progress; refresh after retries
        return status.error_message().find("frozen") != string::npos;
    }
    return false;
}

// ============================================================
// Interactive commands
// ============================================================

// Resolve the head stub for `key` under a shared lock. Returns a copied
// shared_ptr<Stub> that stays valid even if the topology is replaced
// after we release the lock.
static shared_ptr<chain::ChainNode::Stub>
resolve_head_stub(Topology& topo, const string& key, string* endpoint_out, bool verbose) {
    std::shared_lock<std::shared_mutex> rlk(g_topo_mtx);
    NodeStub* target_head = nullptr;
    if (topo.mode == chain::ReplicationMode::CROWN) {
        target_head = topo.crown_head_for(key);
        if (!target_head) {
            if (verbose) {
                cerr << "[Write] No CROWN head found for key='" << key << "' "
                     << "(token=" << Topology::hash_key(key) << ")\n";
            }
            return nullptr;
        }
    } else {
        target_head = topo.head;
        if (!target_head) {
            if (verbose) cerr << "[Write] No head node in topology.\n";
            return nullptr;
        }
    }
    if (endpoint_out) *endpoint_out = target_head->endpoint;
    return target_head->stub;
}

// Non-blocking: adds to pending map, fires RPC, returns immediately.
static bool do_write(Topology& topo, const string& key, const string& value,
                     const string& client_addr, bool verbose = true) {
    uint64_t request_id = add_pending(key, value);
    benchmark_note_write_issued(request_id);

    chain::WriteRequest req;
    req.set_key(key);
    req.set_value(value);
    req.set_version(0);              // head assigns the real version
    req.set_client_addr(client_addr);
    req.set_request_id(request_id);

    // Retry loop: re-resolve head each attempt (in case topology refreshed).
    // Refresh on FAILED_PRECONDITION (frozen) or UNAVAILABLE (dead head).
    chain::WriteResponse resp;
    grpc::Status status;
    string target_endpoint;
    {
        static constexpr int kMaxAttempts = 8;
        static constexpr int kBackoffMs[] = {200, 500, 1000, 2000, 3000, 5000, 5000};

        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            auto stub_copy = resolve_head_stub(topo, key, &target_endpoint, verbose && attempt == 0);
            if (!stub_copy) {
                status = grpc::Status(grpc::StatusCode::UNAVAILABLE, "no head in topology");
                break;
            }

            grpc::ClientContext ctx;
            status = stub_copy->Write(&ctx, req, &resp);
            if (status.ok()) break;

            const bool need_refresh = should_refresh_on_status(status);
            if (!need_refresh || attempt == kMaxAttempts - 1) break;

            if (verbose) {
                cerr << "[Write] " << status.error_message()
                     << " (attempt " << (attempt + 1) << "/" << kMaxAttempts
                     << ") — refreshing topology and retrying\n";
            }
            // Single-flight refresh: concurrent callers coalesce into one fetch.
            refresh_topology(topo);
            std::this_thread::sleep_for(std::chrono::milliseconds(kBackoffMs[attempt]));
        }
    }

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
             << " key='" << key << "' to head (" << target_endpoint << ")\n";
    }
    return true;
}

static bool do_read(Topology& topo, const string& key, int node_id = -1, bool verbose = true) {
    chain::ReadRequest  req;
    chain::ReadResponse resp;
    req.set_key(key);

    benchmark_note_read_sent();
    const auto issued_at = SteadyClock::now();

    // Resolve target + stub under topology lock, then call RPC with the copy.
    // Refresh + retry on UNAVAILABLE (e.g., dead tail after reconfig).
    grpc::Status status;
    static constexpr int kMaxAttempts = 4;
    static constexpr int kBackoffMs[] = {200, 500, 1500};

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        shared_ptr<chain::ChainNode::Stub> stub_copy;
        {
            std::shared_lock<std::shared_mutex> rlk(g_topo_mtx);
            NodeStub* target = resolve_read_target(topo, key, node_id, verbose && attempt == 0);
            if (!target) {
                benchmark_note_read_failure();
                return false;
            }
            stub_copy = target->stub;
        }

        grpc::ClientContext ctx;
        status = stub_copy->Read(&ctx, req, &resp);
        if (status.ok()) break;

        if (!should_refresh_on_status(status) || attempt == kMaxAttempts - 1) break;
        if (verbose) {
            cerr << "[Read] " << status.error_message()
                 << " (attempt " << (attempt + 1) << "/" << kMaxAttempts
                 << ") — refreshing topology and retrying\n";
        }
        refresh_topology(topo);
        std::this_thread::sleep_for(std::chrono::milliseconds(kBackoffMs[attempt]));
    }

    if (!status.ok()) {
        benchmark_note_read_failure();
        if (verbose) cerr << "[Read] Failed: " << status.error_message() << "\n";
        return false;
    }
    benchmark_note_read_success(issued_at);

    if (verbose) {
        if (resp.value().empty())
            cout << "[Read] (not found)\n";
        else
            cout << "[Read] key='" << resp.key()
                 << "' value='" << resp.value()
                 << "' version=" << resp.version() << ")\n";
    }
    return true;
}

static NodeStub* resolve_read_target(Topology& topo,
                                     const string& key,
                                     int node_id,
                                     bool verbose) {
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
            return nullptr;
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
                return nullptr;
            }
        } else {
            // Random pick
            if (topo.nodes.empty()) {
                if (verbose) cerr << "[Read] No nodes in topology.\n";
                return nullptr;
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
            return nullptr;
        }
    }
    return target;
}

static ThroughputMetricsSummary run_bench_write(Topology& topo,
                                                const string& client_addr,
                                                const BenchmarkRunConfig& cfg) {
    auto state = make_shared<ThroughputMetricsState>();
    benchmark_attach_metrics_state(state);

    const vector<PreparedBenchmarkWrite> prepared_writes = benchmark_prepare_write_batch(topo, cfg);

    struct AsyncWriteCall {
        uint64_t request_id = 0;
        chain::WriteRequest request;
        chain::WriteResponse response;
        grpc::ClientContext ctx;
        grpc::Status status;
        unique_ptr<grpc::ClientAsyncResponseReader<chain::WriteResponse>> rpc;
    };

    benchmark_start_metrics_window(*state);
    const auto issue_start = SteadyClock::now();

    grpc::CompletionQueue cq;
    size_t issued = 0;
    for (const auto& prepared : prepared_writes) {
        uint64_t request_id = add_pending(prepared.key, prepared.value);
        benchmark_note_write_issued(request_id);

        auto* call = new AsyncWriteCall();
        call->request_id = request_id;
        call->request.set_key(prepared.key);
        call->request.set_value(prepared.value);
        call->request.set_version(0);  // head assigns the real version
        call->request.set_client_addr(client_addr);
        call->request.set_request_id(request_id);

        call->rpc = prepared.target_head->stub->AsyncWrite(&call->ctx, call->request, &cq);
        if (!call->rpc) {
            benchmark_note_write_rpc_failure();
            (void)remove_pending_request(request_id);
            delete call;
            continue;
        }

        call->rpc->Finish(&call->response, &call->status, call);
        ++issued;
    }

    const auto issue_end = SteadyClock::now();
    const double issue_duration_sec =
        chrono::duration_cast<chrono::duration<double>>(issue_end - issue_start).count();
    const double issue_wps = (issue_duration_sec > 0.0)
        ? (static_cast<double>(issued) / issue_duration_sec)
        : 0.0;
    cout << fixed << setprecision(3)
         << "BENCH_WRITE_ISSUE"
         << " client_index=" << cfg.client_index
         << " num_clients=" << cfg.num_clients
         << " ops_issued=" << issued
         << " issue_duration_s=" << issue_duration_sec
         << " issue_wps=" << issue_wps
         << "\n";

    for (size_t completed = 0; completed < issued; ++completed) {
        void* tag = nullptr;
        bool ok = false;
        if (!cq.Next(&tag, &ok) || tag == nullptr) {
            throw runtime_error("bench-write async completion queue closed unexpectedly");
        }

        auto* call = static_cast<AsyncWriteCall*>(tag);
        if (!ok || !call->status.ok() || !call->response.success()) {
            benchmark_note_write_rpc_failure();
            (void)remove_pending_request(call->request_id);
        }
        delete call;
    }
    cq.Shutdown();

    benchmark_wait_for_pending_acks();
    benchmark_stop_metrics_window(*state);
    const ThroughputMetricsSummary summary = benchmark_build_summary(*state);
    benchmark_detach_metrics_state();
    return summary;
}

static ThroughputMetricsSummary run_bench_read(Topology& topo,
                                               const string& client_addr,
                                               const BenchmarkRunConfig& cfg) {
    const BenchmarkTopologyKeyset keyset = benchmark_build_topology_keyset(
        topo,
        cfg.key_prefix,
        static_cast<size_t>(cfg.key_count));
    const vector<string>& keys = keyset.keys;

    // Seed keys before measuring so benchmark reads can hit previously written values.
    for (size_t i = 0; i < keys.size(); ++i) {
        const string value = cfg.value_prefix + "seed-" + to_string(cfg.client_index) + "-" + to_string(i);
        (void)do_write(topo, keys[i], value, client_addr, false);
    }
    benchmark_wait_for_pending_acks();

    auto state = make_shared<ThroughputMetricsState>();
    benchmark_attach_metrics_state(state);

    const vector<PreparedBenchmarkRead> prepared_reads = benchmark_prepare_read_batch(topo, cfg);

    struct AsyncReadCall {
        chain::ReadRequest request;
        chain::ReadResponse response;
        grpc::ClientContext ctx;
        grpc::Status status;
        SteadyClock::time_point issued_at;
        unique_ptr<grpc::ClientAsyncResponseReader<chain::ReadResponse>> rpc;
    };

    benchmark_start_metrics_window(*state);
    const auto issue_start = SteadyClock::now();

    grpc::CompletionQueue cq;
    size_t issued = 0;
    for (const auto& prepared : prepared_reads) {
        benchmark_note_read_sent();

        auto* call = new AsyncReadCall();
        call->request.set_key(prepared.key);
        call->issued_at = SteadyClock::now();

        call->rpc = prepared.target->stub->AsyncRead(&call->ctx, call->request, &cq);
        if (!call->rpc) {
            benchmark_note_read_failure();
            delete call;
            continue;
        }

        call->rpc->Finish(&call->response, &call->status, call);
        ++issued;
    }

    const auto issue_end = SteadyClock::now();
    const double issue_duration_sec =
        chrono::duration_cast<chrono::duration<double>>(issue_end - issue_start).count();
    const double issue_rps = (issue_duration_sec > 0.0)
        ? (static_cast<double>(issued) / issue_duration_sec)
        : 0.0;
    cout << fixed << setprecision(3)
         << "BENCH_READ_ISSUE"
         << " client_index=" << cfg.client_index
         << " num_clients=" << cfg.num_clients
         << " ops_issued=" << issued
         << " issue_duration_s=" << issue_duration_sec
         << " issue_rps=" << issue_rps
         << "\n";

    for (size_t completed = 0; completed < issued; ++completed) {
        void* tag = nullptr;
        bool ok = false;
        if (!cq.Next(&tag, &ok) || tag == nullptr) {
            throw runtime_error("bench-read async completion queue closed unexpectedly");
        }

        auto* call = static_cast<AsyncReadCall*>(tag);
        if (!ok || !call->status.ok()) {
            benchmark_note_read_failure();
        } else {
            benchmark_note_read_success(call->issued_at);
        }
        delete call;
    }
    cq.Shutdown();

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
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const string metadata_addr = argv[1];

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

    // --- Fetch topology from the metadata store ----------------
    chain::ClusterState cluster;
    string fetch_error;
    if (!fetch_cluster_state(metadata_addr, cluster, fetch_error)) {
        cerr << "[Client] Failed to fetch cluster topology from " << metadata_addr
             << ": " << fetch_error << "\n";
        ack_server->Shutdown();
        ack_thread.join();
        return 1;
    }
    // Cache for refresh_topology() — it uses the same metadata address.
    g_metadata_addr_cached = metadata_addr;
    const chain::ReplicationMode mode = cluster.mode();
    cout << "[Client] Topology from " << metadata_addr
         << ": mode=" << mode_name(mode) << " nodes=" << cluster.nodes_size() << "\n\n";

    // --- Run mode ----------------------------------------------
    Topology topo = build_topology(cluster);
    if (run_mode == ClientRunMode::INTERACTIVE) {
        run_interactive_loop(topo, client_addr);
    } else {
        g_benchmark_mode_active.store(true, memory_order_release);

        const bool is_write = (run_mode == ClientRunMode::BENCH_WRITE);
        cout << "[Client] Running " << (is_write ? "bench-write" : "bench-read")
             << " mode=" << mode_name(mode)
             << " ops_per_client=" << bench_cfg.ops_per_client
             << " aggregate_requested_ops="
             << (bench_cfg.ops_per_client * static_cast<uint64_t>(bench_cfg.num_clients))
             << " key_count=" << bench_cfg.key_count
             << " client_index=" << bench_cfg.client_index
             << " num_clients=" << bench_cfg.num_clients;
        if (is_write && mode == chain::ReplicationMode::CROWN) {
            cout << " crown_hot_head_pct=" << bench_cfg.crown_hot_head_pct;
        }
        if (!is_write) {
            cout << " read_hot_key_pct=" << bench_cfg.read_hot_key_pct;
        }
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
