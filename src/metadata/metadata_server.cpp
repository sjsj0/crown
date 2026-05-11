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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
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
    int    bind_port         = 50050;
    int    ping_interval_ms  = 1000;
    int    ping_timeout_ms   = 1000;
    int    failure_threshold = 3;
    bool   verbose           = false;
};

void print_usage(const char* bin) {
    cerr << "Usage: " << bin
         << " [--config <path>] [--host <host>] [--port <port>]"
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
        lock_guard<mutex> lk(mtx_);
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
            }
        }
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

private:
    mutable mutex          mtx_;
    chain::ReplicationMode mode_;
    vector<NodeEntry>      nodes_;
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
// MetadataStore gRPC service
// ============================================================

class MetadataStoreServiceImpl final : public chain::MetadataStore::Service {
public:
    explicit MetadataStoreServiceImpl(MetadataState& state) : state_(state) {}

    grpc::Status GetCluster(grpc::ServerContext*           /*ctx*/,
                            const google::protobuf::Empty* /*req*/,
                            chain::ClusterState*           resp) override {
        *resp = state_.snapshot();
        return grpc::Status::OK;
    }

private:
    MetadataState& state_;
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

    // --- Serve MetadataStore.GetCluster ---
    MetadataStoreServiceImpl service(state);
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
