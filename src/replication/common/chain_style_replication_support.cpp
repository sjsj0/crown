#include "replication/common/chain_style_replication_support.h"

#include <algorithm>

using namespace std;

ChainStyleReplicationSupport::~ChainStyleReplicationSupport() {
    stop_ack_workers();
}

uint64_t ChainStyleReplicationSupport::assign_next_version(const std::string& key) {
    lock_guard<mutex> lk(state_mtx_);
    KeyState& state = by_key_[key];
    state.next_version += 1;
    return state.next_version;
}

void ChainStyleReplicationSupport::record_local_write(const std::string& key,
                                                      const std::string& value,
                                                      uint64_t version) {
    lock_guard<mutex> lk(state_mtx_);
    KeyState& state = by_key_[key];

    state.next_version = max(state.next_version, version);
    state.pending_versions[version] = value;

    if (version >= state.latest_seen_version) {
        state.latest_seen_version = version;
        state.latest_seen_value = value;
    }
}

void ChainStyleReplicationSupport::record_local_write_if_newer(const std::string& key,
                                                               const std::string& value,
                                                               uint64_t version) {
    lock_guard<mutex> lk(state_mtx_);
    KeyState& state = by_key_[key];

    state.next_version = max(state.next_version, version);
    if (version > state.latest_seen_version) {
        state.latest_seen_version = version;
        state.latest_seen_value = value;
    }
}

LatestReplicaValue ChainStyleReplicationSupport::read_clean(const std::string& key) const {
    lock_guard<mutex> lk(state_mtx_);
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

LatestReplicaValue ChainStyleReplicationSupport::read_committed(const std::string& key) const {
    lock_guard<mutex> lk(state_mtx_);
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

LatestReplicaValue ChainStyleReplicationSupport::read_latest_seen(const std::string& key) const {
    lock_guard<mutex> lk(state_mtx_);
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
    lock_guard<mutex> lk(state_mtx_);
    const auto it = by_key_.find(key);
    if (it == by_key_.end() || version == 0) {
        return false;
    }

    const KeyState& state = it->second;
    if (state.committed_version == version) {
        value_out = state.committed_value;
        return true;
    }

    const auto pending_it = state.pending_versions.find(version);
    if (pending_it == state.pending_versions.end()) {
        return false;
    }

    value_out = pending_it->second;
    return true;
}

void ChainStyleReplicationSupport::mark_version_clean(const std::string& key, uint64_t version) {
    lock_guard<mutex> lk(state_mtx_);
    KeyState& state = by_key_[key];

    state.next_version = max(state.next_version, version);

    const auto clean_it = state.pending_versions.find(version);
    if (clean_it != state.pending_versions.end() && version >= state.committed_version) {
        state.committed_version = version;
        state.committed_value = clean_it->second;
    }

    if (state.committed_version == 0) {
        return;
    }

    for (auto it = state.pending_versions.begin(); it != state.pending_versions.end();) {
        if (it->first <= state.committed_version) {
            it = state.pending_versions.erase(it);
        } else {
            ++it;
        }
    }
}

void ChainStyleReplicationSupport::mark_version_committed_if_newer(const std::string& key,
                                                                   const std::string& value,
                                                                   uint64_t version) {
    lock_guard<mutex> lk(state_mtx_);
    KeyState& state = by_key_[key];

    state.next_version = max(state.next_version, version);
    if (version > state.committed_version) {
        state.committed_version = version;
        state.committed_value = value;
    }
    if (version > state.latest_seen_version) {
        state.latest_seen_version = version;
        state.latest_seen_value = value;
    }

    for (auto it = state.pending_versions.begin(); it != state.pending_versions.end();) {
        if (it->first <= state.committed_version) {
            it = state.pending_versions.erase(it);
        } else {
            ++it;
        }
    }
}

void ChainStyleReplicationSupport::on_config_change(const Node& node) {
    lock_guard<mutex> lk(stub_mtx_);
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
    lock_guard<mutex> lk(stub_mtx_);
    return predecessor_stub_;
}

std::shared_ptr<chain::ChainNode::Stub> ChainStyleReplicationSupport::successor_stub() const {
    lock_guard<mutex> lk(stub_mtx_);
    return successor_stub_;
}

std::shared_ptr<chain::ChainNode::Stub> ChainStyleReplicationSupport::tail_stub() const {
    lock_guard<mutex> lk(stub_mtx_);
    return tail_stub_;
}

std::shared_ptr<chain::ChainNode::Stub> ChainStyleReplicationSupport::get_or_create_client_stub(
    const std::string& client_addr) {
    lock_guard<mutex> lk(client_stub_cache_mtx_);

    const auto cached = client_stubs_.find(client_addr);
    if (cached != client_stubs_.end()) {
        return cached->second;
    }

    auto channel = grpc::CreateChannel(client_addr, grpc::InsecureChannelCredentials());
    auto stub = std::shared_ptr<chain::ChainNode::Stub>(chain::ChainNode::NewStub(channel));
    client_channels_[client_addr] = channel;
    client_stubs_[client_addr] = stub;
    return stub;
}

void ChainStyleReplicationSupport::send_client_ack(const chain::AckRequest& req) {
    if (req.client_addr().empty()) {
        return;
    }

    auto stub = get_or_create_client_stub(req.client_addr());

    google::protobuf::Empty ignored;
    grpc::ClientContext ctx;
    grpc::Status status = stub->Ack(&ctx, req, &ignored);
    if (!status.ok()) {
        cerr << "[ChainStyleReplicationSupport] Client ACK failed to " << req.client_addr()
             << " request_id=" << req.request_id()
             << ": " << status.error_message() << "\n";

        // Drop cached channel/stub on failure to force reconnect on next ACK.
        lock_guard<mutex> lk(client_stub_cache_mtx_);
        client_stubs_.erase(req.client_addr());
        client_channels_.erase(req.client_addr());
    }
}

void ChainStyleReplicationSupport::enqueue_predecessor_ack(const chain::AckRequest& req) {
    {
        lock_guard<mutex> lk(pred_ack_queue_mtx_);
        pred_ack_queue_.push_back(req);
    }
    pred_ack_queue_cv_.notify_one();
}

void ChainStyleReplicationSupport::start_ack_workers() {
    stop_ack_workers();

    pred_ack_worker_running_.store(true, memory_order_release);
    pred_ack_worker_thread_ = make_shared<thread>([this] { predecessor_ack_worker_loop(); });
}

void ChainStyleReplicationSupport::stop_ack_workers() {
    pred_ack_worker_running_.store(false, memory_order_release);
    pred_ack_queue_cv_.notify_one();
    if (pred_ack_worker_thread_ && pred_ack_worker_thread_->joinable()) {
        pred_ack_worker_thread_->join();
    }
    pred_ack_worker_thread_.reset();
}

void ChainStyleReplicationSupport::predecessor_ack_worker_loop() {
    while (pred_ack_worker_running_.load(memory_order_acquire)) {
        chain::AckRequest req;
        {
            unique_lock<mutex> pred_lk(pred_ack_queue_mtx_);
            pred_ack_queue_cv_.wait(pred_lk, [this] {
                return !pred_ack_queue_.empty() || !pred_ack_worker_running_.load(memory_order_acquire);
            });

            if (!pred_ack_worker_running_.load(memory_order_acquire)) break;
            if (pred_ack_queue_.empty()) continue;

            req = pred_ack_queue_.front();
            pred_ack_queue_.pop_front();
        }

        shared_ptr<chain::ChainNode::Stub> pred = predecessor_stub();

        if (!pred) {
            cerr << "[ChainStyleReplicationSupport] Predecessor ACK skipped: no predecessor stub\n";
            continue;
        }

        google::protobuf::Empty ignored;
        grpc::ClientContext ctx;
        grpc::Status status = pred->Ack(&ctx, req, &ignored);
        if (!status.ok()) {
            cerr << "[ChainStyleReplicationSupport] Predecessor ACK failed"
                 << " key='" << req.key() << "' version=" << req.version()
                 << ": " << status.error_message() << "\n";
        }
    }
}