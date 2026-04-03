#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

#include <grpcpp/grpcpp.h>
#include "chain.grpc.pb.h"

#include "node/node.h"

struct LatestReplicaValue {
    bool found = false;
    std::string value;
    uint64_t version = 0;
};

// Shared helper for chain-style replication with explicit clean/dirty state.
// This helper is intentionally not concurrency-safe in this serialized prototype.
class ChainStyleReplicationSupport {
public:
    uint64_t assign_next_version(const std::string& key);

    void record_local_write(const std::string& key,
                            const std::string& value,
                            uint64_t version);

    void mark_version_clean(const std::string& key, uint64_t version);

    // Returns the latest committed (clean) value/version.
    LatestReplicaValue read_clean(const std::string& key) const;

    // Rebuild predecessor/successor channels and stubs for new topology.
    void on_config_change(const Node& node);

    std::shared_ptr<chain::ChainNode::Stub> predecessor_stub() const;
    std::shared_ptr<chain::ChainNode::Stub> successor_stub() const;

private:
    struct KeyState {
        uint64_t next_version = 0;
        uint64_t latest_seen_version = 0;
        std::string latest_seen_value;

        uint64_t clean_version = 0;
        std::string clean_value;

        std::map<uint64_t, std::string> dirty_versions;
    };

    std::unordered_map<std::string, KeyState> by_key_;

    std::shared_ptr<grpc::Channel> predecessor_channel_;
    std::shared_ptr<grpc::Channel> successor_channel_;
    std::shared_ptr<chain::ChainNode::Stub> predecessor_stub_;
    std::shared_ptr<chain::ChainNode::Stub> successor_stub_;
};
