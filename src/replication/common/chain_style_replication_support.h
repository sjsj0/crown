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

    // Send client ACK synchronously from caller context.
    void send_client_ack(const chain::AckRequest& req);

    // Enqueue predecessor ACK for background delivery.
    void enqueue_predecessor_ack(const chain::AckRequest& req);

    // Start and stop background ACK worker thread(s).
    void start_ack_workers();
    void stop_ack_workers();

private:
    std::shared_ptr<chain::ChainNode::Stub> get_or_create_client_stub(const std::string& client_addr);

    // Worker thread entry point.
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
