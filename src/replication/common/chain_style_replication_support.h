#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
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

// Shared helper for plain chain-style replication semantics.
// This is intentionally not CRAQ-style clean/dirty tracking.
class ChainStyleReplicationSupport {
public:
    uint64_t assign_next_version(const std::string& key);

    void apply_replica_write(const std::string& key,
                             const std::string& value,
                             uint64_t version);

    // Returns the latest committed value/version for plain chain-style reads.
    LatestReplicaValue read_latest(const std::string& key) const;

    void mark_write_committed(const std::string& key, uint64_t version);

    void wait_for_commit(const std::string& key, uint64_t version);

    void notify_commit(const std::string& key, uint64_t version);

    // Rebuild predecessor/successor channels and stubs for new topology.
    void on_config_change(const Node& node);

    std::shared_ptr<chain::ChainNode::Stub> predecessor_stub() const;
    std::shared_ptr<chain::ChainNode::Stub> successor_stub() const;

private:
    struct KeyState {
        uint64_t next_version = 0;
        uint64_t latest_version = 0;
        std::string latest_value;

        uint64_t committed_version = 0;
        std::string committed_value;

        std::unordered_map<uint64_t, std::string> inflight;
    };

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_map<std::string, KeyState> by_key_;

    std::shared_ptr<grpc::Channel> predecessor_channel_;
    std::shared_ptr<grpc::Channel> successor_channel_;
    std::shared_ptr<chain::ChainNode::Stub> predecessor_stub_;
    std::shared_ptr<chain::ChainNode::Stub> successor_stub_;
};
