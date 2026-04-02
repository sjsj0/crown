#include "node/node.h"

#include <iostream>
#include <utility>

#include "node/token_hash.h"

using namespace std;

bool NodeAddress::empty() const {
    return host.empty();
}

string NodeAddress::to_string() const {
    return host + ":" + std::to_string(port);
}

Node::Node(NodeConfig config) : config_(std::move(config)) {}

const NodeConfig& Node::config() const {
    return config_;
}

void Node::update_config(NodeConfig new_config) {
    config_ = std::move(new_config);
    cout << "[Node " << config_.node_id << "] Config updated.\n";
}

const string& Node::id() const {
    return config_.node_id;
}

const NodeAddress& Node::self_addr() const {
    return config_.self_addr;
}

ReplicationMode Node::mode() const {
    return config_.mode;
}

bool Node::is_head() const {
    return config_.is_head;
}

bool Node::is_tail() const {
    return config_.is_tail;
}

const optional<NodeAddress>& Node::predecessor() const {
    return config_.predecessor;
}

const optional<NodeAddress>& Node::successor() const {
    return config_.successor;
}

bool Node::is_head_for(const string& key) const {
    return matches_token_ranges(key, config_.head_ranges);
}

bool Node::is_tail_for(const string& key) const {
    return matches_token_ranges(key, config_.tail_ranges);
}

bool Node::matches_token_ranges(const string& key,
                                const vector<KeyRange>& ranges) const {
    const uint64_t token = crown::token::fnv1a64(key);
    for (const auto& r : ranges) {
        uint64_t start = 0;
        uint64_t end = 0;
        if (!crown::token::parse_token(r.start_key, start) ||
            !crown::token::parse_token(r.end_key, end)) {
            continue;
        }
        if (crown::token::token_in_range(token, start, end)) {
            return true;
        }
    }
    return false;
}
