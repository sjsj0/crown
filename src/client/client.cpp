// client.cpp — reads a JSON config file describing the chain topology and
// pushes a NodeConfig to each server node via the Configure RPC.
//
// Config file format (JSON):
//
// {
//   "mode": "craq",            // "chain" | "craq" | "crown"
//   "nodes": [
//     {
//       "id":          0,
//       "host":        "127.0.0.1",
//       "port":        50051,
//       "is_head":     true,
//       "is_tail":     false,
//       "predecessor": null,
//       "successor":   "127.0.0.1:50052"
//     },
//     {
//       "id":          1,
//       "host":        "127.0.0.1",
//       "port":        50052,
//       "is_head":     false,
//       "is_tail":     true,
//       "predecessor": "127.0.0.1:50051",
//       "successor":   null
//     }
//   ]
// }
//
// CROWN mode additionally supports "head_ranges" / "tail_ranges" arrays per node:
//   "head_ranges": [{"start": "0", "end": "6148914691236517205"}]
//
// Dependencies: nlohmann/json (header-only), grpc++, protobuf.
// Install json: https://github.com/nlohmann/json (or `vcpkg install nlohmann-json`)

#include <iostream>
#include <fstream>
#include <limits>
#include <string>
#include <stdexcept>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>
#include "chain.grpc.pb.h"

using namespace std;
using json = nlohmann::json;

// ============================================================
// Helpers — build proto messages from JSON
// ============================================================

static chain::ReplicationMode parse_mode(const string& s) {
    if (s == "chain") return chain::ReplicationMode::CHAIN;
    if (s == "craq")  return chain::ReplicationMode::CRAQ;
    if (s == "crown")  return chain::ReplicationMode::CROWN;
    throw invalid_argument("Unknown mode in config: " + s);
}

// Parse "host:port" string into a NodeAddress proto.
static chain::NodeAddress parse_addr(const string& s) {
    auto colon = s.rfind(':');
    if (colon == string::npos)
        throw invalid_argument("Expected host:port, got: " + s);
    chain::NodeAddress a;
    a.set_host(s.substr(0, colon));
    a.set_port(stoi(s.substr(colon + 1)));
    return a;
}

// Build a NodeConfig proto from a single JSON node entry + the global mode.
static chain::NodeConfig build_node_config(const json& node_json,
                                            chain::ReplicationMode mode) {
    chain::NodeConfig cfg;
    cfg.set_node_id(node_json.at("id").get<int>());
    cfg.set_mode(mode);
    cfg.set_is_head(node_json.value("is_head", false));
    cfg.set_is_tail(node_json.value("is_tail", false));

    // self address
    string host = node_json.at("host").get<string>();
    int    port = node_json.at("port").get<int>();
    chain::NodeAddress self_addr;
    self_addr.set_host(host);
    self_addr.set_port(port);
    *cfg.mutable_self_addr() = self_addr;

    // optional predecessor / successor
    if (!node_json["predecessor"].is_null())
        *cfg.mutable_predecessor() = parse_addr(node_json["predecessor"].get<string>());
    if (!node_json["successor"].is_null())
        *cfg.mutable_successor() = parse_addr(node_json["successor"].get<string>());

    // crown-mode key ranges (ignored for chain / craq)
    if (node_json.contains("head_ranges")) {
        for (const auto& r : node_json["head_ranges"]) {
            auto* kr = cfg.add_head_ranges();
            kr->set_start_key(r.at("start").get<string>());
            kr->set_end_key(r.at("end").get<string>());
        }
    }
    if (node_json.contains("tail_ranges")) {
        for (const auto& r : node_json["tail_ranges"]) {
            auto* kr = cfg.add_tail_ranges();
            kr->set_start_key(r.at("start").get<string>());
            kr->set_end_key(r.at("end").get<string>());
        }
    }

    return cfg;
}

struct TokenRange {
    uint64_t start = 0;
    uint64_t end = 0;
    string start_text;
    string end_text;
};

struct CrownNodeView {
    int id = 0;
    string endpoint;
    string predecessor;
    string successor;
    vector<TokenRange> head_ranges;
    vector<TokenRange> tail_ranges;
};

static string addr_to_string(const chain::NodeAddress& addr) {
    return addr.host() + ":" + to_string(addr.port());
}

static bool parse_token_text(const string& text, uint64_t& out) {
    if (text.empty()) return false;
    try {
        size_t consumed = 0;
        int base = 10;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            base = 16;
        }
        const auto parsed = stoull(text, &consumed, base);
        if (consumed != text.size()) return false;
        out = static_cast<uint64_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

static string range_key(const TokenRange& r) {
    return to_string(r.start) + ":" + to_string(r.end);
}

static string range_text(const TokenRange& r) {
    return "[" + r.start_text + "," + r.end_text + "]";
}

static vector<pair<uint64_t, uint64_t>> to_non_wrapped_segments(const TokenRange& r) {
    if (r.start <= r.end) return {{r.start, r.end}};
    return {{0, r.end}, {r.start, numeric_limits<uint64_t>::max()}};
}

static bool ranges_overlap(const TokenRange& a, const TokenRange& b) {
    const auto a_segments = to_non_wrapped_segments(a);
    const auto b_segments = to_non_wrapped_segments(b);
    for (const auto& as : a_segments) {
        for (const auto& bs : b_segments) {
            if (as.first <= bs.second && bs.first <= as.second) {
                return true;
            }
        }
    }
    return false;
}

static bool parse_token_range(const json& range_json,
                              const string& where,
                              TokenRange& out,
                              string& error) {
    if (!range_json.contains("start") || !range_json.contains("end")) {
        error = where + " must contain 'start' and 'end'";
        return false;
    }

    string start_text;
    string end_text;
    try {
        start_text = range_json.at("start").get<string>();
        end_text = range_json.at("end").get<string>();
    } catch (const exception& ex) {
        error = where + " has invalid start/end values: " + ex.what();
        return false;
    }

    uint64_t start = 0;
    uint64_t end = 0;
    if (!parse_token_text(start_text, start)) {
        error = where + " has invalid start token: '" + start_text + "'";
        return false;
    }
    if (!parse_token_text(end_text, end)) {
        error = where + " has invalid end token: '" + end_text + "'";
        return false;
    }

    out.start = start;
    out.end = end;
    out.start_text = start_text;
    out.end_text = end_text;
    return true;
}

static bool validate_minimal_config(const json& nodes, string& error) {
    if (!nodes.is_array() || nodes.empty()) {
        error = "'nodes' must be a non-empty array";
        return false;
    }

    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        try {
            (void)n.at("id").get<int>();
            (void)n.at("host").get<string>();
            (void)n.at("port").get<int>();

            if (n.contains("predecessor") && !n.at("predecessor").is_null()) {
                (void)parse_addr(n.at("predecessor").get<string>());
            }
            if (n.contains("successor") && !n.at("successor").is_null()) {
                (void)parse_addr(n.at("successor").get<string>());
            }
        } catch (const exception& ex) {
            error = "invalid node at index " + to_string(i) + ": " + ex.what();
            return false;
        }
    }

    return true;
}

static bool validate_crown_topology(const json& nodes, string& error) {
    if (!nodes.is_array() || nodes.empty()) {
        error = "CROWN requires 'nodes' to be a non-empty array";
        return false;
    }

    vector<CrownNodeView> parsed;
    parsed.reserve(nodes.size());

    unordered_map<string, size_t> by_endpoint;
    unordered_set<int> seen_ids;

    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        CrownNodeView v;

        try {
            v.id = n.at("id").get<int>();
            const string host = n.at("host").get<string>();
            const int port = n.at("port").get<int>();
            v.endpoint = host + ":" + to_string(port);
        } catch (const exception& ex) {
            error = "CROWN node index " + to_string(i) + " has invalid id/host/port: " + ex.what();
            return false;
        }

        if (!seen_ids.insert(v.id).second) {
            error = "CROWN duplicate node id: " + to_string(v.id);
            return false;
        }

        if (by_endpoint.count(v.endpoint) > 0) {
            error = "CROWN duplicate node endpoint: " + v.endpoint;
            return false;
        }

        if (!n.contains("predecessor") || n.at("predecessor").is_null()) {
            error = "CROWN node " + v.endpoint + " must define non-null predecessor";
            return false;
        }
        if (!n.contains("successor") || n.at("successor").is_null()) {
            error = "CROWN node " + v.endpoint + " must define non-null successor";
            return false;
        }

        try {
            v.predecessor = addr_to_string(parse_addr(n.at("predecessor").get<string>()));
            v.successor = addr_to_string(parse_addr(n.at("successor").get<string>()));
        } catch (const exception& ex) {
            error = "CROWN node " + v.endpoint + " has invalid predecessor/successor: " + ex.what();
            return false;
        }

        if (!n.contains("head_ranges") || !n.at("head_ranges").is_array()) {
            error = "CROWN node " + v.endpoint + " must have array 'head_ranges'";
            return false;
        }
        if (!n.contains("tail_ranges") || !n.at("tail_ranges").is_array()) {
            error = "CROWN node " + v.endpoint + " must have array 'tail_ranges'";
            return false;
        }

        for (size_t r = 0; r < n.at("head_ranges").size(); ++r) {
            TokenRange tr;
            if (!parse_token_range(n.at("head_ranges")[r],
                                   "head_ranges[" + to_string(r) + "] on node " + v.endpoint,
                                   tr,
                                   error)) {
                return false;
            }
            v.head_ranges.push_back(tr);
        }

        for (size_t r = 0; r < n.at("tail_ranges").size(); ++r) {
            TokenRange tr;
            if (!parse_token_range(n.at("tail_ranges")[r],
                                   "tail_ranges[" + to_string(r) + "] on node " + v.endpoint,
                                   tr,
                                   error)) {
                return false;
            }
            v.tail_ranges.push_back(tr);
        }

        by_endpoint[v.endpoint] = parsed.size();
        parsed.push_back(std::move(v));
    }

    // Ring predecessor/successor consistency checks.
    for (const auto& n : parsed) {
        if (by_endpoint.count(n.predecessor) == 0) {
            error = "CROWN node " + n.endpoint + " predecessor " + n.predecessor + " not found in node list";
            return false;
        }
        if (by_endpoint.count(n.successor) == 0) {
            error = "CROWN node " + n.endpoint + " successor " + n.successor + " not found in node list";
            return false;
        }

        const auto& pred = parsed[by_endpoint[n.predecessor]];
        const auto& succ = parsed[by_endpoint[n.successor]];
        if (pred.successor != n.endpoint) {
            error = "CROWN ring inconsistency: predecessor " + pred.endpoint + " does not point back to " + n.endpoint;
            return false;
        }
        if (succ.predecessor != n.endpoint) {
            error = "CROWN ring inconsistency: successor " + succ.endpoint + " does not point back to " + n.endpoint;
            return false;
        }
    }

    // Ensure one closed cycle across all nodes.
    unordered_set<string> visited;
    string current = parsed.front().endpoint;
    for (size_t step = 0; step < parsed.size(); ++step) {
        if (visited.count(current) > 0) {
            error = "CROWN ring has a cycle before covering all nodes, revisited " + current;
            return false;
        }
        visited.insert(current);
        current = parsed[by_endpoint[current]].successor;
    }
    if (current != parsed.front().endpoint) {
        error = "CROWN ring does not close back to the starting node";
        return false;
    }
    if (visited.size() != parsed.size()) {
        error = "CROWN ring is disconnected; not all nodes are in one cycle";
        return false;
    }

    struct OwnedRange {
        TokenRange range;
        string owner;
    };

    vector<OwnedRange> all_heads;
    vector<OwnedRange> all_tails;
    for (const auto& n : parsed) {
        for (const auto& r : n.head_ranges) all_heads.push_back({r, n.endpoint});
        for (const auto& r : n.tail_ranges) all_tails.push_back({r, n.endpoint});
    }

    if (all_heads.empty()) {
        error = "CROWN requires at least one head token range";
        return false;
    }
    if (all_tails.empty()) {
        error = "CROWN requires at least one tail token range";
        return false;
    }

    for (size_t i = 0; i < all_heads.size(); ++i) {
        for (size_t j = i + 1; j < all_heads.size(); ++j) {
            if (ranges_overlap(all_heads[i].range, all_heads[j].range)) {
                error = "CROWN head range overlap between " + all_heads[i].owner + " " + range_text(all_heads[i].range)
                        + " and " + all_heads[j].owner + " " + range_text(all_heads[j].range);
                return false;
            }
        }
    }

    for (size_t i = 0; i < all_tails.size(); ++i) {
        for (size_t j = i + 1; j < all_tails.size(); ++j) {
            if (ranges_overlap(all_tails[i].range, all_tails[j].range)) {
                error = "CROWN tail range overlap between " + all_tails[i].owner + " " + range_text(all_tails[i].range)
                        + " and " + all_tails[j].owner + " " + range_text(all_tails[j].range);
                return false;
            }
        }
    }

    // Every head range must appear exactly once in predecessor tail ranges.
    for (const auto& n : parsed) {
        const auto pred_it = by_endpoint.find(n.predecessor);
        const auto& pred = parsed[pred_it->second];
        for (const auto& hr : n.head_ranges) {
            int matches = 0;
            for (const auto& tr : pred.tail_ranges) {
                if (hr.start == tr.start && hr.end == tr.end) {
                    ++matches;
                }
            }
            if (matches != 1) {
                error = "CROWN head range " + range_text(hr) + " on " + n.endpoint
                        + " must appear exactly once in predecessor " + pred.endpoint
                        + " tail_ranges (found " + to_string(matches) + ")";
                return false;
            }
        }
    }

    // Each token range must have exactly one head owner and one tail owner.
    unordered_map<string, int> head_counts;
    unordered_map<string, int> tail_counts;
    unordered_set<string> all_keys;

    for (const auto& o : all_heads) {
        const string key = range_key(o.range);
        ++head_counts[key];
        all_keys.insert(key);
    }
    for (const auto& o : all_tails) {
        const string key = range_key(o.range);
        ++tail_counts[key];
        all_keys.insert(key);
    }

    for (const auto& key : all_keys) {
        const int hc = head_counts.count(key) ? head_counts[key] : 0;
        const int tc = tail_counts.count(key) ? tail_counts[key] : 0;
        if (hc != 1 || tc != 1) {
            error = "CROWN token range " + key + " must have exactly one head owner and one tail owner"
                    + " (found heads=" + to_string(hc) + ", tails=" + to_string(tc) + ")";
            return false;
        }
    }

    return true;
}

static bool validate_config_before_configure(const json& config,
                                             chain::ReplicationMode mode,
                                             string& error) {
    if (!config.contains("nodes")) {
        error = "missing required top-level field 'nodes'";
        return false;
    }

    const auto& nodes = config.at("nodes");
    if (!validate_minimal_config(nodes, error)) {
        return false;
    }

    if (mode == chain::ReplicationMode::CROWN) {
        return validate_crown_topology(nodes, error);
    }
    return true;
}

// ============================================================
// main
// ============================================================

int main(int argc, char** argv) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <config.json>\n";
        return 1;
    }

    // --- Load config file --------------------------------------
    ifstream file(argv[1]);
    if (!file.is_open()) {
        cerr << "Cannot open config file: " << argv[1] << "\n";
        return 1;
    }

    json config;
    try {
        file >> config;
    } catch (const json::exception& ex) {
        cerr << "JSON parse error: " << ex.what() << "\n";
        return 1;
    }

    chain::ReplicationMode mode = parse_mode(config.at("mode").get<string>());

    string validation_error;
    if (!validate_config_before_configure(config, mode, validation_error)) {
        cerr << "[Client] Config validation failed: " << validation_error << "\n";
        return 1;
    }

    const auto& nodes = config.at("nodes");

    // --- Send Configure RPC to every node ----------------------
    int failures = 0;
    for (const auto& node_json : nodes) {
        string host = node_json.at("host").get<string>();
        int    port = node_json.at("port").get<int>();
        string target = host + ":" + to_string(port);

        chain::NodeConfig cfg = build_node_config(node_json, mode);

        auto channel = grpc::CreateChannel(target,
                                           grpc::InsecureChannelCredentials());
        auto stub    = chain::ChainNode::NewStub(channel);

        google::protobuf::Empty resp;
        grpc::ClientContext ctx;

        grpc::Status status = stub->Configure(&ctx, cfg, &resp);
        if (status.ok()) {
            cout << "[Client] Configured " << cfg.node_id()
                 << " at " << target << "\n";
        } else {
            cerr << "[Client] Failed to configure " << cfg.node_id()
                 << " at " << target << ": "
                 << status.error_message() << "\n";
            ++failures;
        }
    }

    if (failures > 0) {
        cerr << "[Client] " << failures << " node(s) failed to configure.\n";
        return 1;
    }

    cout << "[Client] All nodes configured successfully.\n";
    return 0;
}
