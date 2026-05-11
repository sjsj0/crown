#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <thread>
#include <deque>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <variant>
#include <chrono>

#include <grpcpp/grpcpp.h>
#include "chain.grpc.pb.h"

#include "node/node.h"

struct LatestReplicaValue {
    bool found = false;
    std::string value;
    uint64_t version = 0;
};

// Shared helper for chain-style replication with per-key versioned state.
// State is protected with internal locks for concurrent RPC handlers.
class ChainStyleReplicationSupport {
public:
    ~ChainStyleReplicationSupport();

    uint64_t assign_next_version(const std::string& key);

    void record_local_write(const std::string& key,
                            const std::string& value,
                            uint64_t version);

    // CHAIN/CROWN path: only keep monotonic latest seen value/version.
    // Older/equal versions are ignored for local persistence.
    void record_local_write_if_newer(const std::string& key,
                                     const std::string& value,
                                     uint64_t version);

    // CRAQ path: mark a version clean based on pending versioned state.
    void mark_version_clean(const std::string& key, uint64_t version);

    // CHAIN/CROWN path: mark committed only if version is newer than current committed.
    // Older/equal versions are ignored for local commit state.
    void mark_version_committed_if_newer(const std::string& key,
                                         const std::string& value,
                                         uint64_t version);

    // CHAIN/CROWN path: latest committed value/version.
    LatestReplicaValue read_committed(const std::string& key) const;

    // CRAQ path: latest clean (committed) value/version.
    LatestReplicaValue read_clean(const std::string& key) const;

    // CRAQ path: latest locally seen value/version (committed or pending).
    LatestReplicaValue read_latest_seen(const std::string& key) const;

    // CRAQ path: resolve a specific version to a value from local committed/pending state.
    bool read_value_at_version(const std::string& key,
                               uint64_t version,
                               std::string& value_out) const;

    // Rebuild predecessor/successor channels and stubs for new topology.
    void on_config_change(const Node& node);

    std::shared_ptr<chain::ChainNode::Stub> predecessor_stub() const;
    std::shared_ptr<chain::ChainNode::Stub> successor_stub() const;
    std::shared_ptr<chain::ChainNode::Stub> tail_stub() const;

    // Enqueue propagate for async fire-with-retry to successor (inter-node).
    void enqueue_propagate(std::shared_ptr<chain::ChainNode::Stub> successor,
                          chain::PropagateRequest req,
                          std::string from_node);

    // Send client ACK synchronously (final confirmation, needs to be fast).
    void send_client_ack(const chain::AckRequest& req);

    // Enqueue predecessor ACK for async delivery with retry (inter-node).
    void enqueue_predecessor_ack(const chain::AckRequest& req);

    // Start and stop background worker threads (propagate, ACK, retry scheduler).
    void start_ack_workers();
    void stop_ack_workers();

private:
    std::shared_ptr<chain::ChainNode::Stub> get_or_create_client_stub(const std::string& client_addr);

    // Propagate dispatcher structures and worker
    struct PropagateTask {
        std::shared_ptr<chain::ChainNode::Stub> successor;
        chain::PropagateRequest req;
        std::string from_node;
        int attempt = 0;
    };

    struct RetryEntry {
        std::chrono::steady_clock::time_point retry_after;
        PropagateTask task;

        bool operator>(const RetryEntry& o) const { return retry_after > o.retry_after; }
    };

    // Propagate workers and queue
    void propagate_worker_loop();
    void schedule_propagate_retry(PropagateTask task, int backoff_seconds);

    std::mutex prop_queue_mtx_;
    std::condition_variable prop_queue_cv_;
    std::queue<PropagateTask> prop_queue_;
    std::vector<std::thread> prop_workers_;

    // Retry scheduler
    void retry_scheduler_loop();

    std::mutex retry_queue_mtx_;
    std::priority_queue<RetryEntry, std::vector<RetryEntry>, std::greater<RetryEntry> > retry_queue_;
    std::condition_variable retry_queue_cv_;
    std::shared_ptr<std::thread> retry_scheduler_thread_;

    // Worker thread entry points (legacy for predecessor ACK, kept for compatibility)
    void predecessor_ack_worker_loop();

    struct KeyState {
        uint64_t next_version = 0;
        uint64_t latest_seen_version = 0;
        std::string latest_seen_value;

        uint64_t committed_version = 0;
        std::string committed_value;

        std::map<uint64_t, std::string> pending_versions;
    };

    std::unordered_map<std::string, KeyState> by_key_;
    mutable std::mutex state_mtx_;

    std::shared_ptr<grpc::Channel> predecessor_channel_;
    std::shared_ptr<grpc::Channel> successor_channel_;
    std::shared_ptr<grpc::Channel> tail_channel_;
    std::shared_ptr<chain::ChainNode::Stub> predecessor_stub_;
    std::shared_ptr<chain::ChainNode::Stub> successor_stub_;
    std::shared_ptr<chain::ChainNode::Stub> tail_stub_;
    mutable std::mutex stub_mtx_;

    std::unordered_map<std::string, std::shared_ptr<grpc::Channel>> client_channels_;
    std::unordered_map<std::string, std::shared_ptr<chain::ChainNode::Stub>> client_stubs_;
    mutable std::mutex client_stub_cache_mtx_;

    // Predecessor ACK worker thread management.
    std::mutex pred_ack_queue_mtx_;
    std::condition_variable pred_ack_queue_cv_;
    std::deque<chain::AckRequest> pred_ack_queue_;
    std::shared_ptr<std::thread> pred_ack_worker_thread_;
    std::atomic<bool> pred_ack_worker_running_{false};
};
