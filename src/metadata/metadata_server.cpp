// metadata_server.cpp — lightweight metadata store with ping-ack failure detection.
//
// This process is the *single source of truth* for cluster topology:
//   1. It is the only component that reads config.json.
//   2. On startup it validates the config and pushes each node its NodeConfig
//      via the existing Configure RPC.
//   3. It runs a ping-ack failure detector: it periodically sends a Ping RPC to
//      every node; after <failure-threshold> consecutive missed acks a node is
//      declared DOWN and logged. (No reconfiguration yet — only logging.)
//   4. It serves MetadataStore.GetCluster so clients can fetch the current
//      topology + per-node liveness instead of reading a config file.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>
#include "chain.grpc.pb.h"

using namespace std;
using json = nlohmann::json;

namespace {

// ============================================================
// CLI options
// ============================================================

struct Options {
    string config_path       = "config.json";
    string bind_host         = "0.0.0.0";
    string external_host     = "172.22.154.121";     // address that nodes use to reach this server
    int    bind_port         = 50050;
    int    ping_interval_ms  = 1000;
    int    ping_timeout_ms   = 1000;
    int    failure_threshold = 3;
    bool   verbose           = false;
};

void print_usage(const char* bin) {
    cerr << "Usage: " << bin
         << " [--config <path>] [--host <host>] [--port <port>]"
            " [--external-host <host>]"
            " [--ping-interval-ms <n>] [--ping-timeout-ms <n>]"
            " [--failure-threshold <n>] [--log]\n";
}

bool parse_int_arg(const string& raw, int& out) {
    try {
        size_t consumed = 0;
        const long long v = stoll(raw, &consumed, 10);
        if (consumed != raw.size()) return false;
        if (v < 0 || v > 2147483647LL) return false;
        out = static_cast<int>(v);
        return true;
    } catch (...) { return false; }
}

bool parse_args(int argc, char** argv, Options& opt, string& err) {
    for (int i = 1; i < argc; ++i) {
        const string a = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { err = string("missing value for ") + name; return nullptr; }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { print_usage(argv[0]); exit(0); }
        else if (a == "--log") { opt.verbose = true; }
        else if (a == "--config") { const char* v = need_value("--config"); if (!v) return false; opt.config_path = v; }
        else if (a == "--host")   { const char* v = need_value("--host");   if (!v) return false; opt.bind_host = v; }
        else if (a == "--external-host") { const char* v = need_value("--external-host"); if (!v) return false; opt.external_host = v; }
        else if (a == "--port")   { const char* v = need_value("--port");   if (!v) return false; if (!parse_int_arg(v, opt.bind_port)) { err = "invalid --port"; return false; } }
        else if (a == "--ping-interval-ms")  { const char* v = need_value("--ping-interval-ms");  if (!v) return false; if (!parse_int_arg(v, opt.ping_interval_ms)  || opt.ping_interval_ms  <= 0) { err = "invalid --ping-interval-ms"; return false; } }
        else if (a == "--ping-timeout-ms")   { const char* v = need_value("--ping-timeout-ms");   if (!v) return false; if (!parse_int_arg(v, opt.ping_timeout_ms)   || opt.ping_timeout_ms <= 0) { err = "invalid --ping-timeout-ms"; return false; } }
        else if (a == "--failure-threshold") { const char* v = need_value("--failure-threshold"); if (!v) return false; if (!parse_int_arg(v, opt.failure_threshold) || opt.failure_threshold <= 0) { err = "invalid --failure-threshold"; return false; } }
        else { err = "unknown argument: " + a; return false; }
    }
    return true;
}

// ============================================================
// JSON -> proto helpers (config.json is read only here)
// ============================================================

chain::ReplicationMode parse_mode(const string& s) {
    if (s == "chain") return chain::ReplicationMode::CHAIN;
    if (s == "craq")  return chain::ReplicationMode::CRAQ;
    if (s == "crown") return chain::ReplicationMode::CROWN;
    throw invalid_argument("Unknown mode in config: " + s);
}

string mode_name(chain::ReplicationMode m) {
    switch (m) {
        case chain::ReplicationMode::CHAIN: return "chain";
        case chain::ReplicationMode::CRAQ:  return "craq";
        case chain::ReplicationMode::CROWN: return "crown";
        default: return "unknown";
    }
}

chain::NodeAddress parse_addr(const string& s) {
    auto colon = s.rfind(':');
    if (colon == string::npos)
        throw invalid_argument("Expected host:port, got: " + s);
    chain::NodeAddress a;
    a.set_host(s.substr(0, colon));
    a.set_port(stoi(s.substr(colon + 1)));
    return a;
}

chain::NodeConfig build_node_config(const json& node_json,
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

    if (node_json.contains("predecessor") && !node_json["predecessor"].is_null())
        *cfg.mutable_predecessor() = parse_addr(node_json["predecessor"].get<string>());
    if (node_json.contains("successor") && !node_json["successor"].is_null())
        *cfg.mutable_successor()   = parse_addr(node_json["successor"].get<string>());
    return cfg;
}

// ============================================================
// Validation (ported from the old client config-push path)
// ============================================================

struct CrownNodeView {
    int id = 0;
    string endpoint, predecessor, successor;
};

bool validate_minimal_config(const json& nodes, string& error) {
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

bool validate_crown_topology(const json& nodes, string& error) {
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

        if (!n.contains("predecessor") || !n.contains("successor") ||
            n["predecessor"].is_null() || n["successor"].is_null()) {
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

bool validate_config_before_configure(const json& config,
                                      chain::ReplicationMode mode,
                                      string& error) {
    if (!config.contains("nodes")) { error = "missing 'nodes'"; return false; }
    const auto& nodes = config.at("nodes");
    if (!validate_minimal_config(nodes, error)) return false;
    if (mode == chain::ReplicationMode::CROWN) return validate_crown_topology(nodes, error);
    return true;
}

// ============================================================
// Cluster state — topology + liveness, behind one mutex
// ============================================================

struct NodeEntry {
    int    node_id = 0;
    string host;
    int    port = 0;
    bool   is_head = false;
    bool   is_tail = false;
    bool   has_pred = false;
    string pred_host;
    int    pred_port = 0;
    bool   has_succ = false;
    string succ_host;
    int    succ_port = 0;

    // Liveness (mutated by the detector).
    bool   alive = true;
    int    consecutive_misses = 0;

    string endpoint() const { return host + ":" + to_string(port); }
};

class MetadataState {
public:
    MetadataState(chain::ReplicationMode mode, vector<NodeEntry> nodes)
        : mode_(mode), nodes_(std::move(nodes)) {}

    size_t size() const { return nodes_.size(); }

    string endpoint_of(size_t idx) const {
        lock_guard<mutex> lk(mtx_);
        return nodes_[idx].endpoint();
    }

    // Apply one ping result for node `idx`. Logs DOWN/UP transitions.
    void record_ping_result(size_t idx, bool ok, int failure_threshold, bool verbose) {
        std::function<void(int)> cb_to_fire;
        int failed_node_id = -1;
        {
            lock_guard<mutex> lk(mtx_);
            if (idx >= nodes_.size()) return;
            NodeEntry& n = nodes_[idx];
            if (ok) {
                n.consecutive_misses = 0;
                if (!n.alive) {
                    n.alive = true;
                    cout << "[MetadataStore] node " << n.node_id << " (" << n.endpoint()
                         << ") is UP again\n" << flush;
                }
            } else {
                ++n.consecutive_misses;
                if (verbose) {
                    cout << "[MetadataStore] node " << n.node_id << " (" << n.endpoint()
                         << ") missed ping #" << n.consecutive_misses << "\n" << flush;
                }
                if (n.alive && n.consecutive_misses >= failure_threshold) {
                    n.alive = false;
                    cout << "[MetadataStore] node " << n.node_id << " (" << n.endpoint()
                         << ") declared DOWN after " << n.consecutive_misses
                         << " missed pings\n" << flush;
                    cb_to_fire = failure_cb_;
                    failed_node_id = n.node_id;
                }
            }
        }
        if (cb_to_fire) cb_to_fire(failed_node_id);
    }

    chain::ClusterState snapshot() const {
        lock_guard<mutex> lk(mtx_);
        chain::ClusterState cs;
        cs.set_mode(mode_);
        for (const NodeEntry& n : nodes_) {
            chain::NodeStatus* s = cs.add_nodes();
            s->set_node_id(n.node_id);
            s->mutable_addr()->set_host(n.host);
            s->mutable_addr()->set_port(n.port);
            s->set_alive(n.alive);
            s->set_is_head(n.is_head);
            s->set_is_tail(n.is_tail);
            if (n.has_pred) { s->mutable_predecessor()->set_host(n.pred_host); s->mutable_predecessor()->set_port(n.pred_port); }
            if (n.has_succ) { s->mutable_successor()->set_host(n.succ_host);   s->mutable_successor()->set_port(n.succ_port); }
        }
        return cs;
    }

    // Snapshot of nodes for reconfig orchestrator use.
    vector<NodeEntry> get_nodes_copy() const {
        lock_guard<mutex> lk(mtx_);
        return nodes_;
    }

    chain::ReplicationMode mode() const { return mode_; }

    // Atomically replace topology after reconfiguration. Resets liveness for new nodes.
    void replace_nodes(vector<NodeEntry> new_nodes) {
        lock_guard<mutex> lk(mtx_);
        nodes_ = std::move(new_nodes);
    }

    // Install a callback to be invoked when a node transitions alive->down.
    void set_failure_callback(std::function<void(int)> cb) {
        lock_guard<mutex> lk(mtx_);
        failure_cb_ = std::move(cb);
    }

private:
    mutable mutex            mtx_;
    chain::ReplicationMode   mode_;
    vector<NodeEntry>        nodes_;
    std::function<void(int)> failure_cb_;
};

// ============================================================
// Ping-ack failure detector — one worker thread per node
// ============================================================

void detector_loop(MetadataState* state, size_t idx, Options opt) {
    const string endpoint = state->endpoint_of(idx);
    auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
    auto stub    = chain::ChainNode::NewStub(channel);

    uint64_t seq = 0;
    for (;;) {
        chain::PingRequest  req;
        chain::PingResponse resp;
        req.set_seq(++seq);

        grpc::ClientContext ctx;
        ctx.set_deadline(chrono::system_clock::now() + chrono::milliseconds(opt.ping_timeout_ms));
        const grpc::Status st = stub->Ping(&ctx, req, &resp);
        state->record_ping_result(idx, st.ok(), opt.failure_threshold, opt.verbose);

        this_thread::sleep_for(chrono::milliseconds(opt.ping_interval_ms));
    }
}

// ============================================================
// Reconfigure orchestrator
// ============================================================
//
// Drives the freeze → inflight-check → (data fetch) → push-config sequence.
// One reconfig active at a time. Metrics logged with [Reconfig N] prefix.

struct ReconfigContext {
    enum class Kind { Add, Failure };
    Kind kind;
    uint64_t id;

    // Add case
    std::string new_node_host;
    int         new_node_port = 0;
    int         new_node_id   = -1;

    // Failure case
    int         failed_node_id = -1;

    // Phase timing
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point phase_start;
};

class ReconfigOrchestrator {
public:
    ReconfigOrchestrator(MetadataState* state,
                         const std::string& metadata_host,
                         int metadata_port)
        : state_(state),
          metadata_host_(metadata_host),
          metadata_port_(metadata_port) {}

    // Returns false if a reconfig is already in progress.
    bool start_add(const string& new_host, int new_port, int* assigned_id_out) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (active_) return false;
            active_ = true;
        }

        auto ctx = std::make_shared<ReconfigContext>();
        ctx->kind = ReconfigContext::Kind::Add;
        ctx->id = ++next_id_;
        ctx->new_node_host = new_host;
        ctx->new_node_port = new_port;

        // Assign new node id = max existing id + 1 (always append at end)
        auto nodes = state_->get_nodes_copy();
        int max_id = -1;
        for (const auto& n : nodes) max_id = std::max(max_id, n.node_id);
        ctx->new_node_id = max_id + 1;
        if (assigned_id_out) *assigned_id_out = ctx->new_node_id;

        std::thread([this, ctx]() { run_reconfig(ctx); }).detach();
        return true;
    }

    bool start_failure(int failed_node_id) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (active_) return false;
            active_ = true;
        }

        auto ctx = std::make_shared<ReconfigContext>();
        ctx->kind = ReconfigContext::Kind::Failure;
        ctx->id = ++next_id_;
        ctx->failed_node_id = failed_node_id;

        std::thread([this, ctx]() { run_reconfig(ctx); }).detach();
        return true;
    }

    void on_inflight_ack(uint64_t reconfig_id, int node_id) {
        std::lock_guard<std::mutex> lk(ack_mtx_);
        if (reconfig_id != current_id_) return;
        pending_acks_.erase(node_id);
        ack_cv_.notify_all();
    }

    void on_data_ready(uint64_t reconfig_id) {
        std::lock_guard<std::mutex> lk(ack_mtx_);
        if (reconfig_id != current_id_) return;
        data_ready_ = true;
        ack_cv_.notify_all();
    }

private:
    void run_reconfig(std::shared_ptr<ReconfigContext> ctx) {
        const bool is_add = (ctx->kind == ReconfigContext::Kind::Add);
        const char* kind_str = is_add ? "add" : "failure";
        ctx->start_time = std::chrono::steady_clock::now();
        const string mode_str = mode_name(state_->mode());

        cout << "[Reconfig " << ctx->id << "] Started ("
             << kind_str << " " << mode_str;
        if (is_add) cout << " node " << ctx->new_node_id;
        else cout << " failed_node=" << ctx->failed_node_id;
        cout << ")\n" << flush;

        // ----- Phase 1: Freeze -----
        ctx->phase_start = std::chrono::steady_clock::now();
        auto nodes = state_->get_nodes_copy();
        vector<NodeEntry> survivors;
        for (auto& n : nodes) {
            if (!is_add && n.node_id == ctx->failed_node_id) continue;
            survivors.push_back(n);
        }

        {
            std::lock_guard<std::mutex> lk(ack_mtx_);
            current_id_ = ctx->id;
            pending_acks_.clear();
            data_ready_ = false;
        }

        const int freeze_failures = freeze_all(survivors, ctx->id);
        const auto freeze_ms = ms_since(ctx->phase_start);
        cout << "[Reconfig " << ctx->id << "] Freeze sent to " << survivors.size()
             << " nodes (" << freeze_ms << "ms";
        if (freeze_failures > 0) cout << ", " << freeze_failures << " failed";
        cout << ")\n" << flush;

        // ----- Phase 2: Inflight check -----
        ctx->phase_start = std::chrono::steady_clock::now();
        const auto mode = state_->mode();
        {
            std::lock_guard<std::mutex> lk(ack_mtx_);
            if (mode == chain::ReplicationMode::CROWN) {
                for (const auto& n : survivors) pending_acks_.insert(n.node_id);
            } else {
                // CHAIN/CRAQ: only tail terminates the inflight token
                for (const auto& n : survivors) {
                    if (n.is_tail) pending_acks_.insert(n.node_id);
                }
            }
        }

        // Wait up to 30s for all InflightAcks
        bool inflight_ok;
        {
            std::unique_lock<std::mutex> lk(ack_mtx_);
            inflight_ok = ack_cv_.wait_for(lk, std::chrono::seconds(30), [this] {
                return pending_acks_.empty();
            });
        }
        const auto inflight_ms = ms_since(ctx->phase_start);

        if (!inflight_ok) {
            cerr << "[Reconfig " << ctx->id << "] Aborted (reason: inflight timeout after "
                 << inflight_ms << "ms)\n" << flush;
            // Best-effort unfreeze by re-pushing existing config
            unfreeze_via_reconfigure(survivors, mode);
            finish_reconfig();
            return;
        }
        cout << "[Reconfig " << ctx->id << "] All InflightAcks received ("
             << inflight_ms << "ms)\n" << flush;

        // ----- Phase 3: Data fetch (Add only) -----
        if (is_add) {
            ctx->phase_start = std::chrono::steady_clock::now();
            if (survivors.empty()) {
                cerr << "[Reconfig " << ctx->id << "] Aborted: no source node available\n" << flush;
                finish_reconfig();
                return;
            }
            const NodeEntry& source = survivors[0];
            if (!send_bootstrap(ctx->new_node_host, ctx->new_node_port,
                                source.host, source.port,
                                ctx->id, ctx->new_node_id)) {
                cerr << "[Reconfig " << ctx->id << "] Aborted: BootstrapFromSource RPC failed\n" << flush;
                unfreeze_via_reconfigure(survivors, mode);
                finish_reconfig();
                return;
            }
            // Wait for DataReady
            bool data_ok;
            {
                std::unique_lock<std::mutex> lk(ack_mtx_);
                data_ok = ack_cv_.wait_for(lk, std::chrono::seconds(60), [this] {
                    return data_ready_;
                });
            }
            const auto fetch_ms = ms_since(ctx->phase_start);
            if (!data_ok) {
                cerr << "[Reconfig " << ctx->id << "] Aborted: data-fetch timeout after "
                     << fetch_ms << "ms\n" << flush;
                unfreeze_via_reconfigure(survivors, mode);
                finish_reconfig();
                return;
            }
            cout << "[Reconfig " << ctx->id << "] Data fetch complete ("
                 << fetch_ms << "ms)\n" << flush;
        }

        // ----- Phase 4: Build + push new config -----
        ctx->phase_start = std::chrono::steady_clock::now();
        vector<NodeEntry> new_topology = build_new_topology(survivors, ctx, mode, is_add);
        const int cfg_failures = push_topology(new_topology, mode);
        const auto cfg_ms = ms_since(ctx->phase_start);
        cout << "[Reconfig " << ctx->id << "] New config pushed to " << new_topology.size()
             << " nodes (" << cfg_ms << "ms";
        if (cfg_failures > 0) cout << ", " << cfg_failures << " failed";
        cout << ")\n" << flush;

        // Commit the new topology atomically
        state_->replace_nodes(new_topology);

        const auto total_ms = ms_since(ctx->start_time);
        cout << "[Reconfig " << ctx->id << "] Complete (total: " << total_ms << "ms)\n" << flush;
        finish_reconfig();
    }

    void finish_reconfig() {
        std::lock_guard<std::mutex> lk(mtx_);
        active_ = false;
    }

    // Send Freeze RPC to all nodes in parallel.
    int freeze_all(const vector<NodeEntry>& nodes, uint64_t reconfig_id) {
        std::atomic<int> failures{0};
        vector<std::thread> threads;
        for (const auto& n : nodes) {
            threads.emplace_back([&, n]() {
                const string ep = n.endpoint();
                auto channel = grpc::CreateChannel(ep, grpc::InsecureChannelCredentials());
                auto stub = chain::ChainNode::NewStub(channel);
                chain::FreezeRequest req;
                req.set_reconfig_id(reconfig_id);
                req.mutable_metadata_addr()->set_host(metadata_host_);
                req.mutable_metadata_addr()->set_port(metadata_port_);
                google::protobuf::Empty resp;
                grpc::ClientContext ctx;
                ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
                grpc::Status st = stub->Freeze(&ctx, req, &resp);
                if (!st.ok()) {
                    cerr << "[Reconfig " << reconfig_id << "] Freeze RPC failed for node "
                         << n.node_id << ": " << st.error_message() << "\n";
                    failures.fetch_add(1);
                }
            });
        }
        for (auto& t : threads) t.join();
        return failures.load();
    }

    // Send BootstrapFromSource to the new node.
    bool send_bootstrap(const string& new_host, int new_port,
                        const string& source_host, int source_port,
                        uint64_t reconfig_id, int new_node_id) {
        const string target = new_host + ":" + std::to_string(new_port);
        auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
        auto stub = chain::ChainNode::NewStub(channel);
        chain::BootstrapRequest req;
        req.set_reconfig_id(reconfig_id);
        req.mutable_source_addr()->set_host(source_host);
        req.mutable_source_addr()->set_port(source_port);
        req.set_new_node_id(new_node_id);
        google::protobuf::Empty resp;
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        grpc::Status st = stub->BootstrapFromSource(&ctx, req, &resp);
        if (!st.ok()) {
            cerr << "[Reconfig " << reconfig_id << "] BootstrapFromSource failed: "
                 << st.error_message() << "\n";
            return false;
        }
        return true;
    }

    // Compute the new topology after add or failure.
    vector<NodeEntry> build_new_topology(const vector<NodeEntry>& survivors,
                                         std::shared_ptr<ReconfigContext> ctx,
                                         chain::ReplicationMode mode,
                                         bool is_add) {
        vector<NodeEntry> out = survivors;

        if (is_add) {
            NodeEntry n;
            n.node_id = ctx->new_node_id;
            n.host    = ctx->new_node_host;
            n.port    = ctx->new_node_port;
            n.alive   = true;
            out.push_back(n);
        }

        // Sort by node_id for deterministic ordering
        std::sort(out.begin(), out.end(),
                  [](const NodeEntry& a, const NodeEntry& b) { return a.node_id < b.node_id; });

        const size_t N = out.size();
        if (N == 0) return out;

        if (mode == chain::ReplicationMode::CROWN) {
            // Ring: each node has predecessor and successor; no fixed head/tail
            for (size_t i = 0; i < N; ++i) {
                size_t prev_i = (i + N - 1) % N;
                size_t next_i = (i + 1) % N;
                out[i].is_head = false;
                out[i].is_tail = false;
                out[i].has_pred = true;
                out[i].pred_host = out[prev_i].host;
                out[i].pred_port = out[prev_i].port;
                out[i].has_succ = true;
                out[i].succ_host = out[next_i].host;
                out[i].succ_port = out[next_i].port;
            }
        } else {
            // CHAIN/CRAQ: linear chain. First is head, last is tail.
            for (size_t i = 0; i < N; ++i) {
                out[i].is_head = (i == 0);
                out[i].is_tail = (i == N - 1);
                if (i == 0) {
                    out[i].has_pred = false;
                } else {
                    out[i].has_pred = true;
                    out[i].pred_host = out[i - 1].host;
                    out[i].pred_port = out[i - 1].port;
                }
                if (i == N - 1) {
                    out[i].has_succ = false;
                } else {
                    out[i].has_succ = true;
                    out[i].succ_host = out[i + 1].host;
                    out[i].succ_port = out[i + 1].port;
                }
            }
        }
        return out;
    }

    // Push Configure to all nodes in the new topology in parallel.
    int push_topology(const vector<NodeEntry>& topology, chain::ReplicationMode mode) {
        std::atomic<int> failures{0};
        const int crown_count = (mode == chain::ReplicationMode::CROWN)
                                  ? static_cast<int>(topology.size()) : 0;

        // For CRAQ, find tail address
        string craq_tail_addr;
        if (mode == chain::ReplicationMode::CRAQ) {
            for (const auto& n : topology) {
                if (n.is_tail) {
                    craq_tail_addr = n.endpoint();
                    break;
                }
            }
        }

        vector<std::thread> threads;
        for (const auto& n : topology) {
            threads.emplace_back([&, n]() {
                chain::NodeConfig cfg;
                cfg.set_node_id(n.node_id);
                cfg.set_mode(mode);
                cfg.set_is_head(n.is_head);
                cfg.set_is_tail(n.is_tail);
                cfg.mutable_self_addr()->set_host(n.host);
                cfg.mutable_self_addr()->set_port(n.port);
                if (n.has_pred) {
                    cfg.mutable_predecessor()->set_host(n.pred_host);
                    cfg.mutable_predecessor()->set_port(n.pred_port);
                }
                if (n.has_succ) {
                    cfg.mutable_successor()->set_host(n.succ_host);
                    cfg.mutable_successor()->set_port(n.succ_port);
                }
                if (mode == chain::ReplicationMode::CROWN) {
                    for (int i = 0; i < crown_count; ++i) (void)cfg.add_head_ranges();
                }
                if (mode == chain::ReplicationMode::CRAQ && !craq_tail_addr.empty()) {
                    cfg.mutable_tail()->set_host(craq_tail_addr.substr(0, craq_tail_addr.rfind(':')));
                    cfg.mutable_tail()->set_port(std::stoi(craq_tail_addr.substr(craq_tail_addr.rfind(':') + 1)));
                }

                auto channel = grpc::CreateChannel(n.endpoint(), grpc::InsecureChannelCredentials());
                auto stub = chain::ChainNode::NewStub(channel);
                google::protobuf::Empty resp;
                grpc::ClientContext ctx;
                ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
                grpc::Status st = stub->Configure(&ctx, cfg, &resp);
                if (!st.ok()) {
                    cerr << "[Reconfig] Configure failed for node " << n.node_id
                         << ": " << st.error_message() << "\n";
                    failures.fetch_add(1);
                }
            });
        }
        for (auto& t : threads) t.join();
        return failures.load();
    }

    // Used on abort paths: push the surviving topology to unfreeze
    void unfreeze_via_reconfigure(const vector<NodeEntry>& nodes, chain::ReplicationMode mode) {
        cerr << "[Reconfig] Attempting unfreeze by re-pushing existing config\n";
        push_topology(nodes, mode);
    }

    static int64_t ms_since(std::chrono::steady_clock::time_point t) {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count();
    }

    MetadataState* state_;
    std::string    metadata_host_;
    int            metadata_port_;
    std::mutex     mtx_;
    bool           active_ = false;
    std::atomic<uint64_t> next_id_{0};

    // Ack tracking (used by RPCs to signal phase completion)
    std::mutex             ack_mtx_;
    std::condition_variable ack_cv_;
    uint64_t                current_id_ = 0;
    std::set<int>           pending_acks_;
    bool                    data_ready_ = false;
};

// ============================================================
// MetadataStore gRPC service
// ============================================================

class MetadataStoreServiceImpl final : public chain::MetadataStore::Service {
public:
    MetadataStoreServiceImpl(MetadataState& state, ReconfigOrchestrator& orch)
        : state_(state), orch_(orch) {}

    grpc::Status GetCluster(grpc::ServerContext*           /*ctx*/,
                            const google::protobuf::Empty* /*req*/,
                            chain::ClusterState*           resp) override {
        *resp = state_.snapshot();
        return grpc::Status::OK;
    }

    grpc::Status Join(grpc::ServerContext*       /*ctx*/,
                      const chain::JoinRequest*  req,
                      chain::JoinResponse*       resp) override {
        int assigned = -1;
        if (!orch_.start_add(req->addr().host(), req->addr().port(), &assigned)) {
            resp->set_accepted(false);
            resp->set_error("reconfig already in progress");
            return grpc::Status::OK;
        }
        resp->set_accepted(true);
        resp->set_assigned_node_id(assigned);
        return grpc::Status::OK;
    }

    grpc::Status InflightAck(grpc::ServerContext*               /*ctx*/,
                             const chain::InflightAckRequest*   req,
                             google::protobuf::Empty*           /*resp*/) override {
        orch_.on_inflight_ack(req->reconfig_id(), req->node_id());
        return grpc::Status::OK;
    }

    grpc::Status DataReady(grpc::ServerContext*             /*ctx*/,
                           const chain::DataReadyRequest*   req,
                           google::protobuf::Empty*         /*resp*/) override {
        orch_.on_data_ready(req->reconfig_id());
        return grpc::Status::OK;
    }

private:
    MetadataState&        state_;
    ReconfigOrchestrator& orch_;
};

// ============================================================
// Configure-push — replaces what client.cpp used to do
// ============================================================

// Returns the number of nodes that failed to configure.
int push_configure_to_all(const json& config,
                          chain::ReplicationMode mode,
                          int crown_node_count,
                          const string& craq_tail_addr,
                          bool verbose) {
    int failures = 0;
    for (const auto& node_json : config.at("nodes")) {
        const string target = node_json.at("host").get<string>() + ":"
                            + to_string(node_json.at("port").get<int>());
        const chain::NodeConfig cfg = build_node_config(node_json, mode, crown_node_count, craq_tail_addr);

        auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
        auto stub    = chain::ChainNode::NewStub(channel);
        google::protobuf::Empty resp;
        grpc::ClientContext     ctx;
        ctx.set_deadline(chrono::system_clock::now() + chrono::seconds(5));
        const grpc::Status st = stub->Configure(&ctx, cfg, &resp);
        if (st.ok()) {
            if (verbose)
                cout << "[MetadataStore] configured node " << cfg.node_id() << " at " << target << "\n";
        } else {
            cerr << "[MetadataStore] failed to configure node " << cfg.node_id()
                 << " at " << target << ": " << st.error_message() << "\n";
            ++failures;
        }
    }
    return failures;
}

// Build the in-memory NodeEntry list from the validated config.
vector<NodeEntry> build_node_entries(const json& config) {
    vector<NodeEntry> out;
    for (const auto& n : config.at("nodes")) {
        NodeEntry e;
        e.node_id = n.at("id").get<int>();
        e.host    = n.at("host").get<string>();
        e.port    = n.at("port").get<int>();
        e.is_head = n.value("is_head", false);
        e.is_tail = n.value("is_tail", false);
        if (n.contains("predecessor") && !n.at("predecessor").is_null()) {
            const chain::NodeAddress a = parse_addr(n.at("predecessor").get<string>());
            e.has_pred = true; e.pred_host = a.host(); e.pred_port = a.port();
        }
        if (n.contains("successor") && !n.at("successor").is_null()) {
            const chain::NodeAddress a = parse_addr(n.at("successor").get<string>());
            e.has_succ = true; e.succ_host = a.host(); e.succ_port = a.port();
        }
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace

// ============================================================
// main
// ============================================================

int main(int argc, char** argv) {
    Options opt;
    string arg_err;
    if (!parse_args(argc, argv, opt, arg_err)) {
        cerr << "[MetadataStore] " << arg_err << "\n";
        print_usage(argv[0]);
        return 1;
    }

    // --- Load + validate config (the only place config.json is read) ---
    ifstream file(opt.config_path);
    if (!file.is_open()) {
        cerr << "[MetadataStore] cannot open config file: " << opt.config_path << "\n";
        return 1;
    }
    json config;
    try { file >> config; }
    catch (const json::exception& ex) {
        cerr << "[MetadataStore] JSON parse error: " << ex.what() << "\n";
        return 1;
    }

    chain::ReplicationMode mode;
    try { mode = parse_mode(config.at("mode").get<string>()); }
    catch (const exception& ex) { cerr << "[MetadataStore] " << ex.what() << "\n"; return 1; }

    string validation_error;
    if (!validate_config_before_configure(config, mode, validation_error)) {
        cerr << "[MetadataStore] config validation failed: " << validation_error << "\n";
        return 1;
    }

    const int crown_node_count = (mode == chain::ReplicationMode::CROWN)
        ? static_cast<int>(config.at("nodes").size())
        : 0;

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
            cerr << "[MetadataStore] CRAQ config must include a tail node with is_tail=true\n";
            return 1;
        }
    }

    cout << "[MetadataStore] loaded config '" << opt.config_path
         << "' mode=" << mode_name(mode)
         << " nodes=" << config.at("nodes").size() << "\n";

    // --- Push topology to all nodes ---
    const int failures = push_configure_to_all(config, mode, crown_node_count, craq_tail_addr, opt.verbose);
    if (failures > 0) {
        cerr << "[MetadataStore] " << failures << " node(s) failed to configure; aborting.\n";
        return 1;
    }
    cout << "[MetadataStore] all nodes configured.\n";

    // --- Build cluster state + start failure detector ---
    MetadataState state(mode, build_node_entries(config));
    for (size_t i = 0; i < state.size(); ++i) {
        thread(detector_loop, &state, i, opt).detach();
    }
    cout << "[MetadataStore] ping-ack detector running ("
         << "interval=" << opt.ping_interval_ms << "ms"
         << " timeout=" << opt.ping_timeout_ms << "ms"
         << " threshold=" << opt.failure_threshold << ")\n";

    // --- Build reconfigure orchestrator ---
    // Use advertised host (or fallback to bind_host) for nodes to send InflightAck back.
    string meta_addr_for_nodes = opt.external_host;
    if (meta_addr_for_nodes.empty() || meta_addr_for_nodes == opt.bind_host) {
        // Fallback: if bind_host is 0.0.0.0, use 127.0.0.1 (local); otherwise use bind_host
        meta_addr_for_nodes = (opt.bind_host == "0.0.0.0") ? "127.0.0.1" : opt.bind_host;
    }
    ReconfigOrchestrator orchestrator(&state, meta_addr_for_nodes, opt.bind_port);

    // Wire automatic failure-triggered reconfig
    state.set_failure_callback([&orchestrator](int failed_node_id) {
        if (!orchestrator.start_failure(failed_node_id)) {
            cerr << "[Reconfig] Failure for node " << failed_node_id
                 << " detected, but reconfig already in progress\n";
        }
    });

    // --- Serve MetadataStore.GetCluster + reconfig RPCs ---
    MetadataStoreServiceImpl service(state, orchestrator);
    const string bind_addr = opt.bind_host + ":" + to_string(opt.bind_port);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(bind_addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if (!server) {
        cerr << "[MetadataStore] failed to bind on " << bind_addr << "\n";
        return 1;
    }
    cout << "[MetadataStore] listening on " << bind_addr << "\n" << flush;
    server->Wait();
    return 0;
}
