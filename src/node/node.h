#pragma once

#include <optional>
#include <string>

// ============================================================
// Enums & plain data types
// ============================================================

enum class ReplicationMode {
    CHAIN,
    CRAQ,
    CROWN
};

struct NodeAddress {
    std::string host;
    int port = 0;

    bool empty() const;
    std::string to_string() const;
};

// Full configuration pushed to a node by the client at startup or on topology change.
struct NodeConfig {
    std::string node_id;
    int node_index = -1;
    NodeAddress self_addr;
    ReplicationMode mode = ReplicationMode::CHAIN;

    // Immediate predecessor/successor in the chain or crown.
    std::optional<NodeAddress> predecessor;
    std::optional<NodeAddress> successor;

    // CHAIN / CRAQ role flags.
    // Ignored in CROWN mode — role is resolved via hash(key) % crown_node_count.
    bool is_head = false;
    bool is_tail = false;

    // CROWN mode: total number of nodes in the ring.
    // Key ownership is computed as hash(key) % crown_node_count.
    int crown_node_count = 0;

    // CRAQ only: direct tail endpoint for version queries.
    std::optional<NodeAddress> tail;
};

// ============================================================
// Node — identity and topology only, no data store
// ============================================================
//
// The key/value store and version tracking live in each ReplicationStrategy,
// since storage semantics differ per strategy (e.g. CRAQ needs a dirty list,
// plain chain does not).

class Node {
public:
    explicit Node(NodeConfig config);

    const NodeConfig& config() const;

    // Replace the config. Called by the Configure RPC handler in server.cpp.
    void update_config(NodeConfig new_config);

    // --------------------------------------------------------
    // Convenience accessors (shortcut into config_)
    // --------------------------------------------------------

    const std::string& id() const;
    int node_index() const;
    const NodeAddress& self_addr() const;
    ReplicationMode mode() const;
    bool is_head() const;
    bool is_tail() const;
    const std::optional<NodeAddress>& predecessor() const;
    const std::optional<NodeAddress>& successor() const;

    // CROWN-mode helpers — hash key and resolve owner via modulo.
    bool is_head_for(const std::string& key) const;
    bool is_tail_for(const std::string& key) const;

private:
    NodeConfig config_;
};
