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

int Node::node_index() const {
    return config_.node_index;
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
    if (config_.mode != ReplicationMode::CROWN) {
        return config_.is_head;
    }
    if (config_.crown_node_count <= 0 || config_.node_index < 0 ||
        config_.node_index >= config_.crown_node_count) {
        return false;
    }

    const uint64_t token = crown::token::fnv1a64(key);
    const int head_index = static_cast<int>(token % static_cast<uint64_t>(config_.crown_node_count));
    return config_.node_index == head_index;
}

bool Node::is_tail_for(const string& key) const {
    if (config_.mode != ReplicationMode::CROWN) {
        return config_.is_tail;
    }
    if (config_.crown_node_count <= 0 || config_.node_index < 0 ||
        config_.node_index >= config_.crown_node_count) {
        return false;
    }

    const uint64_t token = crown::token::fnv1a64(key);
    const int head_index = static_cast<int>(token % static_cast<uint64_t>(config_.crown_node_count));
    const int tail_index = (head_index + config_.crown_node_count - 1) % config_.crown_node_count;
    return config_.node_index == tail_index;
}
