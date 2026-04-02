#pragma once

#include <optional>
#include <string>
#include <vector>

// ============================================================
// Enums & plain data types
// ============================================================

enum class ReplicationMode {
    CHAIN,
    CRAQ,
    CROWN
};

// Inclusive token range used by CROWN to determine per-key head/tail role.
// start_key/end_key carry uint64 token bounds encoded as strings for wire
// compatibility with the existing proto/json shape.
// For CHAIN and CRAQ, leave head_ranges / tail_ranges empty in NodeConfig.
struct KeyRange {
    std::string start_key;   // inclusive token lower bound
    std::string end_key;     // inclusive token upper bound
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
    NodeAddress self_addr;
    ReplicationMode mode = ReplicationMode::CHAIN;

    // Immediate predecessor/successor in the chain or crown.
    std::optional<NodeAddress> predecessor;
    std::optional<NodeAddress> successor;

    // CHAIN / CRAQ role flags.
    // Ignored in CROWN mode — the strategy resolves role per-request from token ranges.
    bool is_head = false;
    bool is_tail = false;

    // CROWN mode: token ranges (on the key-hash ring) for which this node
    // acts as head or tail.
    std::vector<KeyRange> head_ranges;
    std::vector<KeyRange> tail_ranges;
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
    const NodeAddress& self_addr() const;
    ReplicationMode mode() const;
    bool is_head() const;
    bool is_tail() const;
    const std::optional<NodeAddress>& predecessor() const;
    const std::optional<NodeAddress>& successor() const;

    // CROWN-mode helpers — hash key -> uint64 token, then test token ranges.
    bool is_head_for(const std::string& key) const;
    bool is_tail_for(const std::string& key) const;

private:
    bool matches_token_ranges(const std::string& key,
                              const std::vector<KeyRange>& ranges) const;

    NodeConfig config_;
};
