#include "replication/common/chain_style_replication_support.h"

#include <algorithm>

using namespace std;

uint64_t ChainStyleReplicationSupport::assign_next_version(const std::string& key) {
    KeyState& state = by_key_[key];
    state.next_version += 1;
    return state.next_version;
}

void ChainStyleReplicationSupport::record_local_write(const std::string& key,
                                                      const std::string& value,
                                                      uint64_t version) {
    KeyState& state = by_key_[key];

    state.next_version = max(state.next_version, version);
    state.dirty_versions[version] = value;

    if (version >= state.latest_seen_version) {
        state.latest_seen_version = version;
        state.latest_seen_value = value;
    }
}

LatestReplicaValue ChainStyleReplicationSupport::read_clean(const std::string& key) const {
    LatestReplicaValue out;

    const auto it = by_key_.find(key);
    if (it == by_key_.end() || it->second.clean_version == 0) {
        return out;
    }

    out.found = true;
    out.value = it->second.clean_value;
    out.version = it->second.clean_version;
    return out;
}

LatestReplicaValue ChainStyleReplicationSupport::read_latest_seen(const std::string& key) const {
    LatestReplicaValue out;

    const auto it = by_key_.find(key);
    if (it == by_key_.end() || it->second.latest_seen_version == 0) {
        return out;
    }

    out.found = true;
    out.value = it->second.latest_seen_value;
    out.version = it->second.latest_seen_version;
    return out;
}

bool ChainStyleReplicationSupport::read_value_at_version(const std::string& key,
                                                         uint64_t version,
                                                         std::string& value_out) const {
    const auto it = by_key_.find(key);
    if (it == by_key_.end() || version == 0) {
        return false;
    }

    const KeyState& state = it->second;
    if (state.clean_version == version) {
        value_out = state.clean_value;
        return true;
    }

    const auto dirty_it = state.dirty_versions.find(version);
    if (dirty_it == state.dirty_versions.end()) {
        return false;
    }

    value_out = dirty_it->second;
    return true;
}

void ChainStyleReplicationSupport::mark_version_clean(const std::string& key, uint64_t version) {
    KeyState& state = by_key_[key];

    state.next_version = max(state.next_version, version);

    const auto clean_it = state.dirty_versions.find(version);
    if (clean_it != state.dirty_versions.end() && version >= state.clean_version) {
        state.clean_version = version;
        state.clean_value = clean_it->second;
    }

    if (state.clean_version == 0) {
        return;
    }

    for (auto it = state.dirty_versions.begin(); it != state.dirty_versions.end();) {
        if (it->first <= state.clean_version) {
            it = state.dirty_versions.erase(it);
        } else {
            ++it;
        }
    }
}

void ChainStyleReplicationSupport::on_config_change(const Node& node) {
    predecessor_channel_.reset();
    successor_channel_.reset();
    tail_channel_.reset();
    predecessor_stub_.reset();
    successor_stub_.reset();
    tail_stub_.reset();

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

    if (node.config().tail.has_value()) {
        tail_channel_ = grpc::CreateChannel(
            node.config().tail->to_string(),
            grpc::InsecureChannelCredentials());
        auto tail_stub = chain::ChainNode::NewStub(tail_channel_);
        tail_stub_ = std::shared_ptr<chain::ChainNode::Stub>(std::move(tail_stub));
    }
}

std::shared_ptr<chain::ChainNode::Stub> ChainStyleReplicationSupport::predecessor_stub() const {
    return predecessor_stub_;
}

std::shared_ptr<chain::ChainNode::Stub> ChainStyleReplicationSupport::successor_stub() const {
    return successor_stub_;
}

std::shared_ptr<chain::ChainNode::Stub> ChainStyleReplicationSupport::tail_stub() const {
    return tail_stub_;
}
