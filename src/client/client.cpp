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
#include <limits>
#include <string>
#include <stdexcept>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>
#include "chain.grpc.pb.h"

using namespace std;
using json = nlohmann::json;

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

static uint64_t add_pending(const string& key, const string& value) {
    uint64_t id = g_next_request_id.fetch_add(1, memory_order_relaxed);
    lock_guard<mutex> lk(g_pending_mtx);
    g_pending[id] = { id, key, value };
    return id;
}

// Called by the Ack listener thread. Removes from map and prints confirmation.
static void ack_pending(uint64_t request_id, uint64_t version) {
    lock_guard<mutex> lk(g_pending_mtx);
    auto it = g_pending.find(request_id);
    if (it == g_pending.end()) {
        // Already removed or unknown — ignore.
        cout << "\n[Ack] Received ack for unknown request_id=" << request_id << "\n> " << flush;
        return;
    }
    cout << "\n[Ack] Write committed: request_id=" << request_id
         << " key='" << it->second.key << "'"
         << " version=" << version << "\n> " << flush;
    g_pending.erase(it);
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
                                            chain::ReplicationMode mode) {
    chain::NodeConfig cfg;
    cfg.set_node_id(node_json.at("id").get<int>());
    cfg.set_mode(mode);
    cfg.set_is_head(node_json.value("is_head", false));
    cfg.set_is_tail(node_json.value("is_tail", false));

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

    if (node_json.contains("head_ranges"))
        for (const auto& r : node_json["head_ranges"]) {
            auto* kr = cfg.add_head_ranges();
            kr->set_start_key(r.at("start").get<string>());
            kr->set_end_key(r.at("end").get<string>());
        }
    if (node_json.contains("tail_ranges"))
        for (const auto& r : node_json["tail_ranges"]) {
            auto* kr = cfg.add_tail_ranges();
            kr->set_start_key(r.at("start").get<string>());
            kr->set_end_key(r.at("end").get<string>());
        }
    return cfg;
}

// ============================================================
// Validation
// ============================================================

struct TokenRange {
    uint64_t start = 0, end = 0;
    string start_text, end_text;
};

struct CrownNodeView {
    int id = 0;
    string endpoint, predecessor, successor;
    vector<TokenRange> head_ranges, tail_ranges;
};

static bool parse_token_text(const string& text, uint64_t& out) {
    if (text.empty()) return false;
    try {
        size_t consumed = 0;
        int base = (text.size() > 2 && text[0] == '0' && (text[1]=='x'||text[1]=='X')) ? 16 : 10;
        out = static_cast<uint64_t>(stoull(text, &consumed, base));
        return consumed == text.size();
    } catch (...) { return false; }
}

static string range_key(const TokenRange& r) { return to_string(r.start) + ":" + to_string(r.end); }
static string range_text(const TokenRange& r) { return "[" + r.start_text + "," + r.end_text + "]"; }

static vector<pair<uint64_t,uint64_t>> to_non_wrapped_segments(const TokenRange& r) {
    if (r.start <= r.end) return {{r.start, r.end}};
    return {{0, r.end}, {r.start, numeric_limits<uint64_t>::max()}};
}

static bool ranges_overlap(const TokenRange& a, const TokenRange& b) {
    for (const auto& as : to_non_wrapped_segments(a))
        for (const auto& bs : to_non_wrapped_segments(b))
            if (as.first <= bs.second && bs.first <= as.second) return true;
    return false;
}

static bool parse_token_range(const json& j, const string& where, TokenRange& out, string& err) {
    if (!j.contains("start") || !j.contains("end")) { err = where + " needs start/end"; return false; }
    try {
        out.start_text = j.at("start").get<string>();
        out.end_text   = j.at("end").get<string>();
    } catch (const exception& ex) { err = where + ": " + ex.what(); return false; }
    if (!parse_token_text(out.start_text, out.start)) { err = where + " bad start"; return false; }
    if (!parse_token_text(out.end_text,   out.end))   { err = where + " bad end";   return false; }
    return true;
}

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

    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        CrownNodeView v;
        v.id       = n.at("id").get<int>();
        v.endpoint = n.at("host").get<string>() + ":" + to_string(n.at("port").get<int>());
        if (n["predecessor"].is_null() || n["successor"].is_null()) {
            error = "CROWN node " + v.endpoint + " must have both predecessor and successor";
            return false;
        }
        v.predecessor = n.at("predecessor").get<string>();
        v.successor   = n.at("successor").get<string>();
        for (size_t r = 0; r < n.value("head_ranges", json::array()).size(); ++r) {
            TokenRange tr;
            if (!parse_token_range(n.at("head_ranges")[r], "head_ranges[" + to_string(r) + "]", tr, error)) return false;
            v.head_ranges.push_back(tr);
        }
        for (size_t r = 0; r < n.value("tail_ranges", json::array()).size(); ++r) {
            TokenRange tr;
            if (!parse_token_range(n.at("tail_ranges")[r], "tail_ranges[" + to_string(r) + "]", tr, error)) return false;
            v.tail_ranges.push_back(tr);
        }
        by_endpoint[v.endpoint] = parsed.size();
        parsed.push_back(std::move(v));
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

    struct OwnedRange { TokenRange range; string owner; };
    vector<OwnedRange> all_heads, all_tails;
    for (const auto& n : parsed) {
        for (const auto& r : n.head_ranges) all_heads.push_back({r, n.endpoint});
        for (const auto& r : n.tail_ranges) all_tails.push_back({r, n.endpoint});
    }
    if (all_heads.empty()) { error = "CROWN needs at least one head range"; return false; }
    if (all_tails.empty()) { error = "CROWN needs at least one tail range"; return false; }

    for (size_t i = 0; i < all_heads.size(); ++i)
        for (size_t j = i+1; j < all_heads.size(); ++j)
            if (ranges_overlap(all_heads[i].range, all_heads[j].range)) {
                error = "head range overlap: " + all_heads[i].owner + " and " + all_heads[j].owner; return false;
            }
    for (size_t i = 0; i < all_tails.size(); ++i)
        for (size_t j = i+1; j < all_tails.size(); ++j)
            if (ranges_overlap(all_tails[i].range, all_tails[j].range)) {
                error = "tail range overlap: " + all_tails[i].owner + " and " + all_tails[j].owner; return false;
            }

    unordered_map<string,int> hc, tc;
    unordered_set<string> all_keys;
    for (const auto& o : all_heads) { ++hc[range_key(o.range)]; all_keys.insert(range_key(o.range)); }
    for (const auto& o : all_tails) { ++tc[range_key(o.range)]; all_keys.insert(range_key(o.range)); }
    for (const auto& k : all_keys) {
        if ((hc.count(k) ? hc[k] : 0) != 1 || (tc.count(k) ? tc[k] : 0) != 1) {
            error = "token range " + k + " must have exactly one head and one tail"; return false;
        }
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
    string                             endpoint;
    shared_ptr<grpc::Channel>          channel;
    unique_ptr<chain::ChainNode::Stub> stub;

    // CROWN only — token ranges this node owns as head / tail.
    // Empty for chain / craq nodes.
    vector<TokenRange> head_ranges;
    vector<TokenRange> tail_ranges;
};

struct Topology {
    chain::ReplicationMode mode;

    // Chain / CRAQ: single head and tail pointer.
    NodeStub* head = nullptr;
    NodeStub* tail = nullptr;

    // All nodes — owns the memory. Pointers above point into this vector,
    // so the vector must not be resized after build.
    vector<NodeStub> nodes;

    // CROWN: hash a key to a uint64 token using FNV-1a, then find the
    // node whose head_range or tail_range contains that token.
    static uint64_t hash_key(const string& key) {
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : key) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return h;
    }

    // Returns true if token falls inside range, supporting wrap-around.
    static bool token_in_range(uint64_t token, const TokenRange& r) {
        if (r.start <= r.end)
            return token >= r.start && token <= r.end;
        // Wrapped range (e.g. start=0xE000... end=0x1FFF...):
        return token >= r.start || token <= r.end;
    }

    // Find the node that owns `key` as head (for writes) in CROWN mode.
    NodeStub* crown_head_for(const string& key) {
        uint64_t token = hash_key(key);
        for (auto& ns : nodes)
            for (const auto& r : ns.head_ranges)
                if (token_in_range(token, r)) return &ns;
        return nullptr;
    }

    // Find the node that owns `key` as tail (for reads) in CROWN mode.
    NodeStub* crown_tail_for(const string& key) {
        uint64_t token = hash_key(key);
        for (auto& ns : nodes)
            for (const auto& r : ns.tail_ranges)
                if (token_in_range(token, r)) return &ns;
        return nullptr;
    }
};

static Topology build_topology(const json& config, chain::ReplicationMode mode) {
    Topology topo;
    topo.mode = mode;
    const auto& jnodes = config.at("nodes");

    for (const auto& n : jnodes) {
        NodeStub ns;
        ns.endpoint = n.at("host").get<string>() + ":" + to_string(n.at("port").get<int>());
        ns.channel  = grpc::CreateChannel(ns.endpoint, grpc::InsecureChannelCredentials());
        ns.stub     = chain::ChainNode::NewStub(ns.channel);

        // Parse CROWN token ranges if present.
        if (n.contains("head_ranges")) {
            for (const auto& r : n["head_ranges"]) {
                TokenRange tr;
                string err;
                if (parse_token_range(r, "head_ranges", tr, err))
                    ns.head_ranges.push_back(tr);
            }
        }
        if (n.contains("tail_ranges")) {
            for (const auto& r : n["tail_ranges"]) {
                TokenRange tr;
                string err;
                if (parse_token_range(r, "tail_ranges", tr, err))
                    ns.tail_ranges.push_back(tr);
            }
        }

        topo.nodes.push_back(std::move(ns));
    }

    // Chain / CRAQ: identify the single head and tail by flag.
    // CROWN: head/tail are resolved per-key at request time via crown_head_for / crown_tail_for.
    for (size_t i = 0; i < jnodes.size(); ++i) {
        if (jnodes[i].value("is_head", false)) topo.head = &topo.nodes[i];
        if (jnodes[i].value("is_tail", false)) topo.tail = &topo.nodes[i];
    }

    return topo;
}

// ============================================================
// Interactive commands
// ============================================================

// Non-blocking: adds to pending map, fires RPC, returns immediately.
static void do_write(Topology& topo, const string& key, const string& value,
                     const string& client_addr) {
    // Resolve the head node for this key.
    // CR / CRAQ: single static head.
    // CROWN: find the node whose head_range contains hash(key).
    NodeStub* target_head = nullptr;
    if (topo.mode == chain::ReplicationMode::CROWN) {
        target_head = topo.crown_head_for(key);
        if (!target_head) {
            cerr << "[Write] No CROWN head found for key='" << key << "' "
                 << "(token=" << Topology::hash_key(key) << ")\n";
            return;
        }
    } else {
        target_head = topo.head;
        if (!target_head) { cerr << "[Write] No head node in topology.\n"; return; }
    }

    uint64_t request_id = add_pending(key, value);

    chain::WriteRequest req;
    req.set_key(key);
    req.set_value(value);
    req.set_version(0);              // head assigns the real version
    req.set_client_addr(client_addr);
    req.set_request_id(request_id);

    // Fire and forget — ack comes asynchronously from the tail.
    chain::WriteResponse ignored;
    grpc::ClientContext  ctx;
    target_head->stub->Write(&ctx, req, &ignored);

    cout << "[Write] Sent request_id=" << request_id
         << " key='" << key << "' to head (" << target_head->endpoint << ")\n";
}

static void do_read(Topology& topo, const string& key) {
    // Resolve the tail node for this key.
    // CR / CRAQ: single static tail.
    // CROWN: find the node whose tail_range contains hash(key).
    NodeStub* target_tail = nullptr;
    if (topo.mode == chain::ReplicationMode::CROWN) {
        target_tail = topo.crown_tail_for(key);
        if (!target_tail) {
            cerr << "[Read] No CROWN tail found for key='" << key << "' "
                 << "(token=" << Topology::hash_key(key) << ")\n";
            return;
        }
    } else {
        target_tail = topo.tail;
        if (!target_tail) { cerr << "[Read] No tail node in topology.\n"; return; }
    }

    chain::ReadRequest  req;
    chain::ReadResponse resp;
    grpc::ClientContext ctx;
    req.set_key(key);

    grpc::Status status = target_tail->stub->Read(&ctx, req, &resp);
    if (!status.ok()) { cerr << "[Read] Failed: " << status.error_message() << "\n"; return; }

    if (resp.value().empty())
        cout << "[Read] (not found)\n";
    else
        cout << "[Read] key='" << resp.key()
             << "' value='" << resp.value()
             << "' version=" << resp.version()
             << " (via " << target_tail->endpoint << ")\n";
}

static void print_help() {
    cout << "Commands:\n"
         << "  write <key> <value>  — non-blocking write (ack printed when tail confirms)\n"
         << "  read  <key>          — read directly from tail\n"
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
            if (key.empty()) { cerr << "Usage: read <key>\n"; continue; }
            do_read(topo, key);

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
        cerr << "Usage: " << argv[0] << " <config.json> [ack_port]\n";
        return 1;
    }

    const int    ack_port    = (argc >= 3) ? stoi(argv[2]) : 60000;
    const string client_addr = "127.0.0.1:" + to_string(ack_port);

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

    string validation_error;
    if (!validate_config_before_configure(config, mode, validation_error)) {
        cerr << "[Client] Config validation failed: " << validation_error << "\n";
        return 1;
    }

    // --- Configure all nodes -----------------------------------
    int failures = 0;
    for (const auto& node_json : config.at("nodes")) {
        string target = node_json.at("host").get<string>() + ":"
                      + to_string(node_json.at("port").get<int>());
        chain::NodeConfig cfg = build_node_config(node_json, mode);
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

    if (failures > 0) {
        cerr << "[Client] " << failures << " node(s) failed to configure.\n";
        ack_server->Shutdown();
        ack_thread.join();
        return 1;
    }

    cout << "[Client] All nodes configured successfully.\n\n";

    // --- Interactive loop --------------------------------------
    Topology topo = build_topology(config, mode);
    run_interactive_loop(topo, client_addr);

    // --- Shutdown ----------------------------------------------
    ack_server->Shutdown();
    ack_thread.join();
    return 0;
}