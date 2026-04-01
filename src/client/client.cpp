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
//   "head_ranges": [{"start": "a", "end": "m"}]
//
// Dependencies: nlohmann/json (header-only), grpc++, protobuf.
// Install json: https://github.com/nlohmann/json (or `vcpkg install nlohmann-json`)

#include <iostream>
#include <fstream>
#include <string>
#include <stdexcept>
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