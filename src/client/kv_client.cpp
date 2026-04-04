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

struct NodeInfo {
    int id = 0;
    string host;
    int port = 0;
    bool is_head = false;
    bool is_tail = false;

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

static bool parse_nodes(const json& nodes_json,
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

static bool build_crown_by_id(const vector<NodeInfo>& nodes,
                              vector<const NodeInfo*>& by_id,
                              string& error) {
    by_id.assign(nodes.size(), nullptr);

    for (const auto& n : nodes) {
        if (n.id < 0 || n.id >= static_cast<int>(nodes.size())) {
            error = "CROWN node id out of range [0," + to_string(nodes.size() - 1)
                  + "]: id=" + to_string(n.id);
            return false;
        }
        if (by_id[n.id] != nullptr) {
            error = "duplicate CROWN node id: " + to_string(n.id);
            return false;
        }
        by_id[n.id] = &n;
    }

    return true;
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

    return parse_nodes(config.at("nodes"), nodes, error);
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
    vector<const NodeInfo*> crown_by_id;

    if (mode == Mode::CROWN) {
        token = crown::token::fnv1a64(key);
        if (!build_crown_by_id(nodes, crown_by_id, error)) {
            cerr << "[kv_client] Route resolution failed: " << error << "\n";
            return 1;
        }
    }

    if (op == "write") {
        if (mode == Mode::CHAIN || mode == Mode::CRAQ) {
            target = find_global_role(nodes, &NodeInfo::is_head, "head", error);
        } else {
            const size_t head_index = static_cast<size_t>(token % crown_by_id.size());
            target = crown_by_id[head_index];
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
        const size_t head_index = static_cast<size_t>(token % crown_by_id.size());
        const size_t tail_index = (head_index + crown_by_id.size() - 1) % crown_by_id.size();
        target = crown_by_id[tail_index];
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
