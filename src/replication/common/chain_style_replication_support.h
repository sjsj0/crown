#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <thread>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>

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

    // Returns the latest locally seen value/version (clean or dirty).
    LatestReplicaValue read_latest_seen(const std::string& key) const;

    // Resolve a specific version to a value from local clean/dirty state.
    bool read_value_at_version(const std::string& key,
                               uint64_t version,
                               std::string& value_out) const;

    // Rebuild predecessor/successor channels and stubs for new topology.
    void on_config_change(const Node& node);

    std::shared_ptr<chain::ChainNode::Stub> predecessor_stub() const;
    std::shared_ptr<chain::ChainNode::Stub> successor_stub() const;
    std::shared_ptr<chain::ChainNode::Stub> tail_stub() const;

    // Enqueue ACK messages for background delivery.
    void enqueue_client_ack(const chain::AckRequest& req);
    void enqueue_predecessor_ack(const chain::AckRequest& req);

    // Start and stop background ACK worker threads.
    void start_ack_workers();
    void stop_ack_workers();

private:
    // Worker thread entry points.
    void client_ack_worker_loop();
    void predecessor_ack_worker_loop();
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
    std::shared_ptr<grpc::Channel> tail_channel_;
    std::shared_ptr<chain::ChainNode::Stub> predecessor_stub_;
    std::shared_ptr<chain::ChainNode::Stub> successor_stub_;
    std::shared_ptr<chain::ChainNode::Stub> tail_stub_;

    // ACK worker thread management.
    std::mutex client_ack_queue_mtx_;
    std::condition_variable client_ack_queue_cv_;
    std::deque<chain::AckRequest> client_ack_queue_;
    std::shared_ptr<std::thread> client_ack_worker_thread_;
    std::atomic<bool> client_ack_worker_running_{false};

    std::mutex pred_ack_queue_mtx_;
    std::condition_variable pred_ack_queue_cv_;
    std::deque<chain::AckRequest> pred_ack_queue_;
    std::shared_ptr<std::thread> pred_ack_worker_thread_;
    std::atomic<bool> pred_ack_worker_running_{false};
};
