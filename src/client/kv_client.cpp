#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>

#include "chain.grpc.pb.h"
#include "node/token_hash.h"

using namespace std;
using json = nlohmann::json;

enum class Mode {
    CHAIN,
    CRAQ,
    CROWN
};

struct TokenRange {
    uint64_t start = 0;
    uint64_t end = 0;
};

struct NodeInfo {
    int id = 0;
    string host;
    int port = 0;
    bool is_head = false;
    bool is_tail = false;
    vector<TokenRange> head_ranges;
    vector<TokenRange> tail_ranges;

    string endpoint() const {
        return host + ":" + to_string(port);
    }
};

static string mode_name(Mode mode) {
    switch (mode) {
        case Mode::CHAIN: return "chain";
        case Mode::CRAQ: return "craq";
        case Mode::CROWN: return "crown";
    }
    return "unknown";
}

static Mode parse_mode(const string& s) {
    if (s == "chain") return Mode::CHAIN;
    if (s == "craq") return Mode::CRAQ;
    if (s == "crown") return Mode::CROWN;
    throw invalid_argument("Unknown mode in config: " + s);
}

static bool parse_token_range(const json& range_json,
                              TokenRange& out,
                              string& error) {
    if (!range_json.contains("start") || !range_json.contains("end")) {
        error = "range must contain 'start' and 'end'";
        return false;
    }

    string start_text;
    string end_text;
    try {
        start_text = range_json.at("start").get<string>();
        end_text = range_json.at("end").get<string>();
    } catch (const exception& ex) {
        error = string("invalid range start/end values: ") + ex.what();
        return false;
    }

    if (!crown::token::parse_token(start_text, out.start)) {
        error = "invalid range start token: '" + start_text + "'";
        return false;
    }
    if (!crown::token::parse_token(end_text, out.end)) {
        error = "invalid range end token: '" + end_text + "'";
        return false;
    }

    return true;
}

static bool parse_nodes(const json& nodes_json,
                        Mode mode,
                        vector<NodeInfo>& nodes,
                        string& error) {
    if (!nodes_json.is_array() || nodes_json.empty()) {
        error = "'nodes' must be a non-empty array";
        return false;
    }

    nodes.clear();
    nodes.reserve(nodes_json.size());

    for (size_t i = 0; i < nodes_json.size(); ++i) {
        const auto& n = nodes_json[i];
        NodeInfo node;

        try {
            node.id = n.at("id").get<int>();
            node.host = n.at("host").get<string>();
            node.port = n.at("port").get<int>();
            node.is_head = n.value("is_head", false);
            node.is_tail = n.value("is_tail", false);
        } catch (const exception& ex) {
            error = "invalid node at index " + to_string(i) + ": " + ex.what();
            return false;
        }

        if (mode == Mode::CROWN) {
            if (n.contains("head_ranges") && n.at("head_ranges").is_array()) {
                for (const auto& r : n.at("head_ranges")) {
                    TokenRange tr;
                    if (!parse_token_range(r, tr, error)) {
                        error = "invalid head_ranges on node " + node.endpoint() + ": " + error;
                        return false;
                    }
                    node.head_ranges.push_back(tr);
                }
            }

            if (n.contains("tail_ranges") && n.at("tail_ranges").is_array()) {
                for (const auto& r : n.at("tail_ranges")) {
                    TokenRange tr;
                    if (!parse_token_range(r, tr, error)) {
                        error = "invalid tail_ranges on node " + node.endpoint() + ": " + error;
                        return false;
                    }
                    node.tail_ranges.push_back(tr);
                }
            }
        }

        nodes.push_back(std::move(node));
    }

    return true;
}

static const NodeInfo* find_global_role(const vector<NodeInfo>& nodes,
                                        bool NodeInfo::*flag,
                                        const string& role,
                                        string& error) {
    const NodeInfo* selected = nullptr;

    for (const auto& n : nodes) {
        if (n.*flag) {
            if (selected != nullptr) {
                error = "multiple nodes marked as " + role + " (" + selected->endpoint() + ", " + n.endpoint() + ")";
                return nullptr;
            }
            selected = &n;
        }
    }

    if (selected == nullptr) {
        error = "no node marked as " + role;
        return nullptr;
    }

    return selected;
}

static const NodeInfo* find_crown_owner(const vector<NodeInfo>& nodes,
                                        uint64_t token,
                                        bool use_head_ranges,
                                        const string& key,
                                        string& error) {
    const NodeInfo* selected = nullptr;

    for (const auto& n : nodes) {
        const auto& ranges = use_head_ranges ? n.head_ranges : n.tail_ranges;
        for (const auto& r : ranges) {
            if (crown::token::token_in_range(token, r.start, r.end)) {
                if (selected != nullptr) {
                    const string kind = use_head_ranges ? "head" : "tail";
                    error = "multiple CROWN " + kind + " owners for key '" + key + "' token=" + to_string(token)
                            + " (" + selected->endpoint() + ", " + n.endpoint() + ")";
                    return nullptr;
                }
                selected = &n;
            }
        }
    }

    if (selected == nullptr) {
        const string kind = use_head_ranges ? "head" : "tail";
        error = "no CROWN " + kind + " owner for key '" + key + "' token=" + to_string(token);
        return nullptr;
    }

    return selected;
}

static bool load_config(const string& config_path,
                        Mode& mode,
                        vector<NodeInfo>& nodes,
                        string& error) {
    ifstream file(config_path);
    if (!file.is_open()) {
        error = "cannot open config file: " + config_path;
        return false;
    }

    json config;
    try {
        file >> config;
    } catch (const json::exception& ex) {
        error = string("JSON parse error: ") + ex.what();
        return false;
    }

    try {
        mode = parse_mode(config.at("mode").get<string>());
    } catch (const exception& ex) {
        error = ex.what();
        return false;
    }

    if (!config.contains("nodes")) {
        error = "missing required top-level field 'nodes'";
        return false;
    }

    return parse_nodes(config.at("nodes"), mode, nodes, error);
}

static bool do_write_rpc(const NodeInfo& target,
                         const string& key,
                         const string& value,
                         uint64_t& out_version,
                         string& error) {
    auto channel = grpc::CreateChannel(target.endpoint(), grpc::InsecureChannelCredentials());
    auto stub = chain::ChainNode::NewStub(channel);

    chain::WriteRequest req;
    req.set_key(key);
    req.set_value(value);
    req.set_version(0);

    chain::WriteResponse resp;
    grpc::ClientContext ctx;

    grpc::Status status = stub->Write(&ctx, req, &resp);
    if (!status.ok()) {
        error = "Write RPC failed at " + target.endpoint() + ": " + status.error_message();
        return false;
    }
    if (!resp.success()) {
        error = "Write RPC returned success=false at " + target.endpoint();
        return false;
    }

    out_version = resp.version();
    return true;
}

static bool do_read_rpc(const NodeInfo& target,
                        const string& key,
                        string& out_value,
                        uint64_t& out_version,
                        string& error) {
    auto channel = grpc::CreateChannel(target.endpoint(), grpc::InsecureChannelCredentials());
    auto stub = chain::ChainNode::NewStub(channel);

    chain::ReadRequest req;
    req.set_key(key);

    chain::ReadResponse resp;
    grpc::ClientContext ctx;

    grpc::Status status = stub->Read(&ctx, req, &resp);
    if (!status.ok()) {
        error = "Read RPC failed at " + target.endpoint() + ": " + status.error_message();
        return false;
    }

    out_value = resp.value();
    out_version = resp.version();
    return true;
}

static void print_usage(const string& bin) {
    cerr << "Usage:\n"
         << "  " << bin << " write <config.json> <key> <value>\n"
         << "  " << bin << " read  <config.json> <key>\n";
}

int main(int argc, char** argv) {
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }

    const string op = argv[1];
    const string config_path = argv[2];
    const string key = argv[3];

    if (op == "write" && argc != 5) {
        print_usage(argv[0]);
        return 1;
    }
    if (op == "read" && argc != 4) {
        print_usage(argv[0]);
        return 1;
    }
    if (op != "write" && op != "read") {
        print_usage(argv[0]);
        return 1;
    }

    Mode mode;
    vector<NodeInfo> nodes;
    string error;
    if (!load_config(config_path, mode, nodes, error)) {
        cerr << "[kv_client] Config load failed: " << error << "\n";
        return 1;
    }

    const NodeInfo* target = nullptr;
    uint64_t token = 0;

    if (mode == Mode::CROWN) {
        token = crown::token::fnv1a64(key);
    }

    if (op == "write") {
        if (mode == Mode::CHAIN || mode == Mode::CRAQ) {
            target = find_global_role(nodes, &NodeInfo::is_head, "head", error);
        } else {
            target = find_crown_owner(nodes, token, true, key, error);
        }

        if (target == nullptr) {
            cerr << "[kv_client] Route resolution failed: " << error << "\n";
            return 1;
        }

        const string value = argv[4];
        uint64_t version = 0;
        if (!do_write_rpc(*target, key, value, version, error)) {
            cerr << "[kv_client] " << error << "\n";
            return 1;
        }

        cout << "[kv_client] mode=" << mode_name(mode)
             << " op=write"
             << " key='" << key << "'";
        if (mode == Mode::CROWN) {
            cout << " token=" << token;
        }
        cout << " target=" << target->endpoint()
             << " version=" << version << "\n";
        return 0;
    }

    // op == read
    if (mode == Mode::CHAIN || mode == Mode::CRAQ) {
        target = find_global_role(nodes, &NodeInfo::is_tail, "tail", error);
    } else {
        target = find_crown_owner(nodes, token, false, key, error);
    }

    if (target == nullptr) {
        cerr << "[kv_client] Route resolution failed: " << error << "\n";
        return 1;
    }

    string value;
    uint64_t version = 0;
    if (!do_read_rpc(*target, key, value, version, error)) {
        cerr << "[kv_client] " << error << "\n";
        return 1;
    }

    cout << "[kv_client] mode=" << mode_name(mode)
         << " op=read"
         << " key='" << key << "'";
    if (mode == Mode::CROWN) {
        cout << " token=" << token;
    }
    cout << " target=" << target->endpoint()
         << " version=" << version
         << " value='" << value << "'\n";

    return 0;
}
