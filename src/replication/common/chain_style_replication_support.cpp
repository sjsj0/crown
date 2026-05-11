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

    // Start propagate workers (using existing pattern)
    {
        lock_guard<mutex> lk(prop_queue_mtx_);
        size_t workers = std::thread::hardware_concurrency();
        if (workers == 0) workers = 4;
        workers = std::min<size_t>(workers, 8);

        prop_workers_.reserve(workers);
        for (size_t i = 0; i < workers; ++i) {
            prop_workers_.emplace_back([this]() { propagate_worker_loop(); });
        }
    }

    // Start ACK workers (2 threads for client + predecessor ACKs)
    {
        lock_guard<mutex> lk(ack_queue_mtx_);
        ack_workers_.reserve(2);
        for (int i = 0; i < 2; ++i) {
            ack_workers_.emplace_back([this]() { ack_worker_loop(); });
        }
    }

    // Start retry scheduler (single thread for all retries)
    {
        lock_guard<mutex> lk(retry_queue_mtx_);
        retry_scheduler_thread_ = make_shared<thread>([this]() { retry_scheduler_loop(); });
    }

    // Legacy predecessor ACK worker
    pred_ack_worker_running_.store(true, memory_order_release);
    pred_ack_worker_thread_ = make_shared<thread>([this] { predecessor_ack_worker_loop(); });
}

void ChainStyleReplicationSupport::stop_ack_workers() {
    // Stop propagate workers
    {
        lock_guard<mutex> lk(prop_queue_mtx_);
        // Signal workers to exit by making them check an empty queue after notify
    }
    prop_queue_cv_.notify_all();
    for (auto& worker : prop_workers_) {
        if (worker.joinable()) worker.join();
    }
    prop_workers_.clear();

    // Stop ACK workers
    {
        lock_guard<mutex> lk(ack_queue_mtx_);
    }
    ack_queue_cv_.notify_all();
    for (auto& worker : ack_workers_) {
        if (worker.joinable()) worker.join();
    }
    ack_workers_.clear();

    // Stop retry scheduler
    {
        lock_guard<mutex> lk(retry_queue_mtx_);
    }
    retry_queue_cv_.notify_one();
    if (retry_scheduler_thread_ && retry_scheduler_thread_->joinable()) {
        retry_scheduler_thread_->join();
    }
    retry_scheduler_thread_.reset();

    // Legacy predecessor ACK worker
    pred_ack_worker_running_.store(false, memory_order_release);
    pred_ack_queue_cv_.notify_one();
    if (pred_ack_worker_thread_ && pred_ack_worker_thread_->joinable()) {
        pred_ack_worker_thread_->join();
    }
    pred_ack_worker_thread_.reset();
}

void ChainStyleReplicationSupport::enqueue_propagate(
    std::shared_ptr<chain::ChainNode::Stub> successor,
    chain::PropagateRequest req,
    std::string from_node) {
    if (!successor) {
        cerr << "[Support] Propagate skipped from " << from_node << ": no successor stub\n";
        return;
    }

    {
        lock_guard<mutex> lk(prop_queue_mtx_);
        prop_queue_.push(PropagateTask{std::move(successor), std::move(req), std::move(from_node), 0});
    }
    prop_queue_cv_.notify_one();
}

void ChainStyleReplicationSupport::enqueue_client_ack(const chain::AckRequest& req) {
    {
        lock_guard<mutex> lk(ack_queue_mtx_);
        ack_queue_.push(AckTask{req, false, 0});
    }
    ack_queue_cv_.notify_one();
}

void ChainStyleReplicationSupport::schedule_propagate_retry(PropagateTask task, int backoff_seconds) {
    {
        lock_guard<mutex> lk(retry_queue_mtx_);
        auto retry_time = chrono::steady_clock::now() + chrono::seconds(backoff_seconds);
        retry_queue_.push(RetryEntry{retry_time, true, task});
    }
    retry_queue_cv_.notify_one();
}

void ChainStyleReplicationSupport::schedule_ack_retry(AckTask task, int backoff_seconds) {
    {
        lock_guard<mutex> lk(retry_queue_mtx_);
        auto retry_time = chrono::steady_clock::now() + chrono::seconds(backoff_seconds);
        retry_queue_.push(RetryEntry{retry_time, false, task});
    }
    retry_queue_cv_.notify_one();
}

void ChainStyleReplicationSupport::propagate_worker_loop() {
    static constexpr int kMaxRetryAttempts = 3;
    static constexpr int kBackoffs[] = {15, 45, 90};

    while (true) {
        PropagateTask task;
        {
            unique_lock<mutex> lk(prop_queue_mtx_);
            prop_queue_cv_.wait(lk, [this] { return !prop_queue_.empty(); });
            if (prop_queue_.empty()) break;
            task = std::move(prop_queue_.front());
            prop_queue_.pop();
        }

        google::protobuf::Empty ignored;
        grpc::ClientContext ctx;
        grpc::Status status = task.successor->Propagate(&ctx, task.req, &ignored);

        if (!status.ok()) {
            cerr << "[Support] Propagate attempt " << (task.attempt + 1)
                 << " failed from " << task.from_node
                 << " key='" << task.req.key() << "' version=" << task.req.version()
                 << ": " << status.error_message() << "\n";

            if (task.attempt < kMaxRetryAttempts) {
                task.attempt += 1;
                int backoff = kBackoffs[task.attempt - 1];
                schedule_propagate_retry(task, backoff);
            } else {
                cerr << "[Support] Propagate dropped after " << (task.attempt + 1)
                     << " attempts key='" << task.req.key() << "' version=" << task.req.version() << "\n";
            }
        }
    }
}

void ChainStyleReplicationSupport::ack_worker_loop() {
    static constexpr int kMaxRetryAttempts = 3;
    static constexpr int kBackoffs[] = {15, 45, 90};

    while (true) {
        AckTask task;
        {
            unique_lock<mutex> lk(ack_queue_mtx_);
            ack_queue_cv_.wait(lk, [this] { return !ack_queue_.empty(); });
            if (ack_queue_.empty()) break;
            task = std::move(ack_queue_.front());
            ack_queue_.pop();
        }

        google::protobuf::Empty ignored;
        grpc::ClientContext ctx;
        grpc::Status status;

        if (task.is_pred_ack) {
            auto pred = predecessor_stub();
            if (!pred) {
                cerr << "[Support] ACK (pred) skipped: no predecessor stub\n";
                continue;
            }
            status = pred->Ack(&ctx, task.req, &ignored);
        } else {
            auto stub = get_or_create_client_stub(task.req.client_addr());
            if (!stub) {
                cerr << "[Support] ACK (client) skipped: no client stub\n";
                continue;
            }
            status = stub->Ack(&ctx, task.req, &ignored);
        }

        if (!status.ok()) {
            const char* ack_type = task.is_pred_ack ? "pred" : "client";
            cerr << "[Support] ACK (" << ack_type << ") attempt " << (task.attempt + 1)
                 << " failed key='" << task.req.key() << "' version=" << task.req.version()
                 << ": " << status.error_message() << "\n";

            if (task.attempt < kMaxRetryAttempts) {
                task.attempt += 1;
                int backoff = kBackoffs[task.attempt - 1];
                schedule_ack_retry(task, backoff);
            } else {
                const char* ack_type_long = task.is_pred_ack ? "predecessor" : "client";
                cerr << "[Support] ACK (" << ack_type_long << ") dropped after " << (task.attempt + 1)
                     << " attempts key='" << task.req.key() << "' version=" << task.req.version() << "\n";
            }

            // On client ACK failure, drop the cached stub to force reconnect
            if (!task.is_pred_ack && task.attempt == 0) {
                lock_guard<mutex> lk(client_stub_cache_mtx_);
                client_stubs_.erase(task.req.client_addr());
                client_channels_.erase(task.req.client_addr());
            }
        }
    }
}

void ChainStyleReplicationSupport::retry_scheduler_loop() {
    while (true) {
        chrono::steady_clock::time_point next_deadline;
        {
            unique_lock<mutex> lk(retry_queue_mtx_);

            // Wait until we have retries or shutdown
            if (retry_queue_.empty()) {
                retry_queue_cv_.wait(lk, [this] { return !retry_queue_.empty(); });
                if (retry_queue_.empty()) break;
            }

            // Calculate wait time to next deadline
            const auto& top = retry_queue_.top();
            next_deadline = top.retry_after;
        }

        // Wait until deadline or until new retries added (with 1s max timeout)
        {
            unique_lock<mutex> lk(retry_queue_mtx_);
            retry_queue_cv_.wait_until(lk, next_deadline);
        }

        // Move ready retries back to work queues
        vector<RetryEntry> ready;
        {
            unique_lock<mutex> lk(retry_queue_mtx_);
            auto now = chrono::steady_clock::now();

            while (!retry_queue_.empty() && retry_queue_.top().retry_after <= now) {
                ready.push_back(std::move(const_cast<RetryEntry&>(retry_queue_.top())));
                const_cast<priority_queue<RetryEntry, vector<RetryEntry>, greater<RetryEntry> >&>(
                    retry_queue_).pop();
            }
        }

        // Enqueue ready retries back to their respective work queues
        for (auto& entry : ready) {
            if (entry.is_propagate) {
                PropagateTask* task = std::get_if<PropagateTask>(&entry.task);
                if (task) {
                    {
                        lock_guard<mutex> lk(prop_queue_mtx_);
                        prop_queue_.push(std::move(*task));
                    }
                    prop_queue_cv_.notify_one();
                }
            } else {
                AckTask* task = std::get_if<AckTask>(&entry.task);
                if (task) {
                    {
                        lock_guard<mutex> lk(ack_queue_mtx_);
                        ack_queue_.push(std::move(*task));
                    }
                    ack_queue_cv_.notify_one();
                }
            }
        }
    }
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

        enqueue_predecessor_ack(req);
    }
}