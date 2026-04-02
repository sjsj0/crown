#include "replication/common/chain_style_replication_support.h"

#include <algorithm>

using namespace std;

uint64_t ChainStyleReplicationSupport::assign_next_version(const std::string& key) {
    lock_guard<mutex> lock(mu_);
    KeyState& state = by_key_[key];
    state.next_version += 1;
    return state.next_version;
}

void ChainStyleReplicationSupport::apply_replica_write(const std::string& key,
                                                       const std::string& value,
                                                       uint64_t version) {
    lock_guard<mutex> lock(mu_);
    KeyState& state = by_key_[key];

    state.next_version = max(state.next_version, version);
    state.inflight[version] = value;

    if (version >= state.latest_version) {
        state.latest_version = version;
        state.latest_value = value;
    }
}

LatestReplicaValue ChainStyleReplicationSupport::read_latest(const std::string& key) const {
    lock_guard<mutex> lock(mu_);
    LatestReplicaValue out;

    const auto it = by_key_.find(key);
    if (it == by_key_.end() || it->second.committed_version == 0) {
        return out;
    }

    out.found = true;
    out.value = it->second.committed_value;
    out.version = it->second.committed_version;
    return out;
}

void ChainStyleReplicationSupport::mark_write_committed(const std::string& key, uint64_t version) {
    lock_guard<mutex> lock(mu_);
    KeyState& state = by_key_[key];

    state.next_version = max(state.next_version, version);

    if (version > state.committed_version) {
        state.committed_version = version;

        const auto committed_it = state.inflight.find(version);
        if (committed_it != state.inflight.end()) {
            state.committed_value = committed_it->second;
        } else if (version == state.latest_version) {
            state.committed_value = state.latest_value;
        }
    }

    for (auto it = state.inflight.begin(); it != state.inflight.end();) {
        if (it->first <= state.committed_version) {
            it = state.inflight.erase(it);
        } else {
            ++it;
        }
    }
}

void ChainStyleReplicationSupport::wait_for_commit(const std::string& key, uint64_t version) {
    unique_lock<mutex> lock(mu_);
    cv_.wait(lock, [&]() {
        const auto it = by_key_.find(key);
        return it != by_key_.end() && it->second.committed_version >= version;
    });
}

void ChainStyleReplicationSupport::notify_commit(const std::string& key, uint64_t version) {
    (void)key;
    (void)version;
    cv_.notify_all();
}

void ChainStyleReplicationSupport::on_config_change(const Node& node) {
    lock_guard<mutex> lock(mu_);

    predecessor_channel_.reset();
    successor_channel_.reset();
    predecessor_stub_.reset();
    successor_stub_.reset();

    if (node.predecessor().has_value()) {
        predecessor_channel_ = grpc::CreateChannel(
            node.predecessor()->to_string(),
            grpc::InsecureChannelCredentials());
        auto pred_stub = chain::ChainNode::NewStub(predecessor_channel_);
        predecessor_stub_ = std::shared_ptr<chain::ChainNode::Stub>(std::move(pred_stub));
    }

    if (node.successor().has_value()) {
        successor_channel_ = grpc::CreateChannel(
            node.successor()->to_string(),
            grpc::InsecureChannelCredentials());
        auto succ_stub = chain::ChainNode::NewStub(successor_channel_);
        successor_stub_ = std::shared_ptr<chain::ChainNode::Stub>(std::move(succ_stub));
    }
}

std::shared_ptr<chain::ChainNode::Stub> ChainStyleReplicationSupport::predecessor_stub() const {
    lock_guard<mutex> lock(mu_);
    return predecessor_stub_;
}

std::shared_ptr<chain::ChainNode::Stub> ChainStyleReplicationSupport::successor_stub() const {
    lock_guard<mutex> lock(mu_);
    return successor_stub_;
}
