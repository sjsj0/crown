#include <string>
#include <vector>
#include <optional>
#include <iostream>

using namespace std;

// ============================================================
// Enums & plain data types
// ============================================================

enum class ReplicationMode {
    CHAIN,
    CRAQ,
    CROWN
};

// Inclusive key range used by crown mode to determine per-request head/tail role.
// For CHAIN and CRAQ, leave head_ranges / tail_ranges empty in NodeConfig.
struct KeyRange {
    string start_key;   // inclusive
    string end_key;     // inclusive
};

struct NodeAddress {
    string host;
    int port = 0;

    bool empty() const { 
        return host.empty(); 
    }

    string to_string() const {
        return host + ":" + std::to_string(port);
    }
};

// Full configuration pushed to a node by the client at startup or on topology change.
struct NodeConfig {
    string node_id;
    NodeAddress self_addr;
    ReplicationMode mode = ReplicationMode::CHAIN;

    // Immediate predecessor/successor in the chain or crown.
    optional<NodeAddress> predecessor;
    optional<NodeAddress> successor;

    // CHAIN / CRAQ role flags.
    // Ignored in CROWN mode — the strategy resolves role per-request from key ranges.
    bool is_head = false;
    bool is_tail = false;

    // CROWN mode: key ranges for which this node acts as head or tail.
    vector<KeyRange> head_ranges;
    vector<KeyRange> tail_ranges;
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
    explicit Node(NodeConfig config): config_(std::move(config)) {}

    const NodeConfig& config() const { 
        return config_;
    }

    // Replace the config. Called by the Configure RPC handler in server.cpp.
    void update_config(NodeConfig new_config) {
        config_ = std::move(new_config);
        cout << "[Node " << config_.node_id << "] Config updated.\n";
    }

    // --------------------------------------------------------
    // Convenience accessors (shortcut into config_)
    // --------------------------------------------------------

    const string&          id()          const { return config_.node_id;  }
    const NodeAddress&     self_addr()   const { return config_.self_addr; }
    ReplicationMode        mode()        const { return config_.mode;      }
    bool                   is_head()     const { return config_.is_head;   }
    bool                   is_tail()     const { return config_.is_tail;   }
    const optional<NodeAddress>& predecessor() const { return config_.predecessor; }
    const optional<NodeAddress>& successor()   const { return config_.successor;   }

    // CROWN-mode helpers — check whether this node is head/tail for a given key.
    bool is_head_for(const string& key) const {
        for (const auto& r : config_.head_ranges)
            if (key >= r.start_key && key <= r.end_key) return true;
        return false;
    }

    bool is_tail_for(const string& key) const {
        for (const auto& r : config_.tail_ranges)
            if (key >= r.start_key && key <= r.end_key) return true;
        return false;
    }

private:
    NodeConfig config_;
};
