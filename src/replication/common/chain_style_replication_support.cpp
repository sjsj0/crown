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
    std::shared_ptr<chain::ChainNode::Stub> new_successor_stub;
    {
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
            new_successor_stub = successor_stub_;
        }

        if (node.config().tail.has_value()) {
            tail_channel_ = grpc::CreateChannel(
                node.config().tail->to_string(),
                grpc::InsecureChannelCredentials());
            auto tail_stub = chain::ChainNode::NewStub(tail_channel_);
            tail_stub_ = std::shared_ptr<chain::ChainNode::Stub>(std::move(tail_stub));
        }
    }

    // Refresh successor stubs for any queued propagate tasks (handles
    // failure-case where the old successor is dead and pending writes must
    // be redirected to the new successor).
    if (new_successor_stub) {
        lock_guard<mutex> qlk(prop_queue_mtx_);
        std::queue<PropagateTask> refreshed;
        while (!prop_queue_.empty()) {
            PropagateTask task = std::move(prop_queue_.front());
            prop_queue_.pop();
            task.successor = new_successor_stub;
            refreshed.push(std::move(task));
        }
        prop_queue_ = std::move(refreshed);
        if (!prop_queue_.empty()) {
            prop_queue_cv_.notify_all();
        }
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
    ctx.set_deadline(chrono::system_clock::now() + chrono::seconds(5));
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

    // Start propagate workers with retry support
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

    // Start retry scheduler (handles propagate retries)
    {
        lock_guard<mutex> lk(retry_queue_mtx_);
        retry_scheduler_thread_ = make_shared<thread>([this]() { retry_scheduler_loop(); });
    }

    // Start predecessor ACK worker (inter-node ACK with retry)
    pred_ack_worker_running_.store(true, memory_order_release);
    pred_ack_worker_thread_ = make_shared<thread>([this] { predecessor_ack_worker_loop(); });
}

void ChainStyleReplicationSupport::stop_ack_workers() {
    // Signal all workers to stop (must stay true until all are joined)
    workers_stopping_.store(true, memory_order_release);

    // Stop propagate workers
    prop_queue_cv_.notify_all();
    for (auto& worker : prop_workers_) {
        if (worker.joinable()) worker.join();
    }
    prop_workers_.clear();

    // Stop retry scheduler (uses workers_stopping_ in its wait predicate)
    retry_queue_cv_.notify_one();
    if (retry_scheduler_thread_ && retry_scheduler_thread_->joinable()) {
        retry_scheduler_thread_->join();
    }
    retry_scheduler_thread_.reset();

    // Stop predecessor ACK worker
    pred_ack_worker_running_.store(false, memory_order_release);
    pred_ack_queue_cv_.notify_one();
    if (pred_ack_worker_thread_ && pred_ack_worker_thread_->joinable()) {
        pred_ack_worker_thread_->join();
    }
    pred_ack_worker_thread_.reset();

    // Reset after all workers have exited
    workers_stopping_.store(false, memory_order_release);
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

void ChainStyleReplicationSupport::schedule_propagate_retry(PropagateTask task, int backoff_seconds) {
    {
        lock_guard<mutex> lk(retry_queue_mtx_);
        auto retry_time = chrono::steady_clock::now() + chrono::seconds(backoff_seconds);
        retry_queue_.push(RetryEntry{retry_time, task});
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
            prop_queue_cv_.wait(lk, [this] {
                return !prop_queue_.empty() || workers_stopping_.load(memory_order_acquire);
            });
            if (prop_queue_.empty()) break;  // empty + stopping (or spurious), exit
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

void ChainStyleReplicationSupport::retry_scheduler_loop() {
    while (true) {
        chrono::steady_clock::time_point next_deadline;
        {
            unique_lock<mutex> lk(retry_queue_mtx_);

            // Wait until we have retries or shutdown
            if (retry_queue_.empty()) {
                retry_queue_cv_.wait(lk, [this] {
                    return !retry_queue_.empty() || workers_stopping_.load(memory_order_acquire);
                });
                if (retry_queue_.empty()) break;
            }

            // Calculate wait time to next deadline
            const auto& top = retry_queue_.top();
            next_deadline = top.retry_after;
        }

        // Wait until deadline or until new retries added
        {
            unique_lock<mutex> lk(retry_queue_mtx_);
            retry_queue_cv_.wait_until(lk, next_deadline);
        }

        // Move ready retries back to propagate work queue
        vector<RetryEntry> ready;
        {
            unique_lock<mutex> lk(retry_queue_mtx_);
            auto now = chrono::steady_clock::now();

            while (!retry_queue_.empty() && retry_queue_.top().retry_after <= now) {
                ready.push_back(retry_queue_.top());
                const_cast<priority_queue<RetryEntry, vector<RetryEntry>, greater<RetryEntry> >&>(
                    retry_queue_).pop();
            }
        }

        // Enqueue ready retries back to propagate work queue
        if (!ready.empty()) {
            lock_guard<mutex> lk(prop_queue_mtx_);
            for (auto& entry : ready) {
                prop_queue_.push(entry.task);
            }
            prop_queue_cv_.notify_all();
        }
    }
}

chain::DataDump ChainStyleReplicationSupport::dump_committed_state() const {
    chain::DataDump dump;
    lock_guard<mutex> lk(state_mtx_);
    for (const auto& [key, state] : by_key_) {
        if (state.committed_version == 0) continue;
        auto* entry = dump.add_entries();
        entry->set_key(key);
        entry->set_value(state.committed_value);
        entry->set_version(state.committed_version);
    }
    return dump;
}

void ChainStyleReplicationSupport::load_from_dump(const chain::DataDump& dump) {
    lock_guard<mutex> lk(state_mtx_);
    by_key_.clear();
    for (const auto& entry : dump.entries()) {
        KeyState& state = by_key_[entry.key()];
        state.next_version = entry.version();
        state.latest_seen_version = entry.version();
        state.latest_seen_value = entry.value();
        state.committed_version = entry.version();
        state.committed_value = entry.value();
    }
}

void ChainStyleReplicationSupport::send_inflight_check(uint64_t reconfig_id, int32_t origin_node_id) {
    auto succ = successor_stub();
    if (!succ) {
        cerr << "[Support] Cannot send InflightCheck: no successor stub\n";
        return;
    }

    // Fire async with retry — InflightCheck is rare (only during reconfig) so
    // a detached thread with bounded retries is the simplest implementation.
    thread([succ, reconfig_id, origin_node_id]() {
        static constexpr int kMaxAttempts = 4;
        static constexpr int kBackoffs[] = {2, 5, 10};

        chain::InflightCheckRequest req;
        req.set_reconfig_id(reconfig_id);
        req.set_origin_node_id(origin_node_id);

        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            google::protobuf::Empty ignored;
            grpc::ClientContext ctx;
            grpc::Status status = succ->InflightCheck(&ctx, req, &ignored);
            if (status.ok()) return;

            cerr << "[Support] InflightCheck attempt " << (attempt + 1)
                 << " failed reconfig_id=" << reconfig_id
                 << " origin=" << origin_node_id
                 << ": " << status.error_message() << "\n";

            if (attempt < kMaxAttempts - 1) {
                this_thread::sleep_for(chrono::seconds(kBackoffs[attempt]));
            }
        }
        cerr << "[Support] InflightCheck dropped after " << kMaxAttempts
             << " attempts reconfig_id=" << reconfig_id << "\n";
    }).detach();
}

void ChainStyleReplicationSupport::predecessor_ack_worker_loop() {
    static constexpr int kBackoffs[] = {1, 2, 5, 10};

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

        int attempt = 0;
        while (pred_ack_worker_running_.load(memory_order_acquire)) {
            auto pred = predecessor_stub();
            if (!pred) {
                cerr << "[Support] Pred ACK waiting: no predecessor stub"
                     << " key='" << req.key() << "' version=" << req.version() << "\n";
            } else {
                google::protobuf::Empty resp;
                grpc::ClientContext ctx;
                ctx.set_deadline(chrono::system_clock::now() + chrono::seconds(5));
                grpc::Status st = pred->Ack(&ctx, req, &resp);
                if (st.ok()) {
                    cout << "[Support] Pred ACK delivered"
                         << " key='" << req.key() << "' version=" << req.version() << "\n";
                    break;
                }

                cerr << "[Support] Pred ACK attempt " << (attempt + 1)
                     << " failed key='" << req.key() << "' version=" << req.version()
                     << ": " << st.error_message() << "\n";
            }

            const int backoff = kBackoffs[min(
                attempt,
                static_cast<int>(sizeof(kBackoffs) / sizeof(kBackoffs[0])) - 1)];
            ++attempt;

            unique_lock<mutex> pred_lk(pred_ack_queue_mtx_);
            pred_ack_queue_cv_.wait_for(
                pred_lk,
                chrono::seconds(backoff),
                [this] {
                    return !pred_ack_worker_running_.load(memory_order_acquire);
                });
        }
    }
}
