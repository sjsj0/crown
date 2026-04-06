#include "craq_replication.h"

#include <google/protobuf/empty.pb.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <condition_variable>
#include <queue>
#include <vector>

#include "chain.pb.h"
#include "node/node.h"

using namespace std;

namespace {

string craq_node_label(const Node& node) {
    if (!node.id().empty()) {
        return node.id();
    }
    return node.self_addr().to_string();
}

struct PropagateTask {
    std::shared_ptr<chain::ChainNode::Stub> successor;
    chain::PropagateRequest req;
    std::string from_node;
};

class PropagateDispatcher {
public:
    explicit PropagateDispatcher(const std::string& tag)
        : tag_(tag) {
        size_t workers = std::thread::hardware_concurrency();
        if (workers == 0) workers = 4;
        workers = std::min<size_t>(workers, 8);

        workers_.reserve(workers);
        for (size_t i = 0; i < workers; ++i) {
            workers_.emplace_back([this]() { worker_loop(); });
        }
    }

    ~PropagateDispatcher() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

    void enqueue(std::shared_ptr<chain::ChainNode::Stub> successor,
                 chain::PropagateRequest req,
                 std::string from_node) {
        if (!successor) {
            cerr << tag_ << " Async PROPAGATE skipped from " << from_node
                 << ": no successor stub\n";
            return;
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push(PropagateTask{std::move(successor), std::move(req), std::move(from_node)});
        }
        cv_.notify_one();
    }

private:
    void worker_loop() {
        while (true) {
            PropagateTask task;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this]() { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop();
            }

            google::protobuf::Empty ignored;
            grpc::ClientContext ctx;
            grpc::Status status = task.successor->Propagate(&ctx, task.req, &ignored);
            if (!status.ok()) {
                cerr << tag_ << " Async PROPAGATE failed from " << task.from_node
                     << " key='" << task.req.key() << "' version=" << task.req.version()
                     << ": " << status.error_message() << "\n";
            }
        }
    }

    std::string tag_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<PropagateTask> queue_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
};

PropagateDispatcher& craq_propagate_dispatcher() {
    static PropagateDispatcher dispatcher("[CRAQ]");
    return dispatcher;
}

void forward_propagate_async(std::shared_ptr<chain::ChainNode::Stub> successor,
                             chain::PropagateRequest req,
                             const std::string& from_node) {
    craq_propagate_dispatcher().enqueue(std::move(successor), std::move(req), from_node);
}

chain::ReadResponse build_read_response(const std::string& key,
                                        const std::string& value,
                                        uint64_t version) {
    chain::ReadResponse resp;
    resp.set_key(key);
    resp.set_value(value);
    resp.set_version(version);
    return resp;
}

} // namespace

chain::WriteResponse CRAQReplication::handle_write(const chain::WriteRequest& req, Node& node) {
    if (!node.is_head()) {
        throw runtime_error("CRAQ write rejected: node is not head");
    }

    const uint64_t version = support_.assign_next_version(req.key());
    support_.record_local_write(req.key(), req.value(), version);

    cout << "[CRAQ] Head " << craq_node_label(node)
         << " accepted write key='" << req.key() << "' version=" << version
         << " (response means accepted by head; commit happens later when ACK returns)\n";

    chain::WriteResponse resp;
    resp.set_success(true);
    resp.set_version(version);

    if (node.is_tail()) {
        support_.mark_version_clean(req.key(), version);
        cout << "[CRAQ] Single-node chain committed immediately key='" << req.key()
             << "' version=" << version << "\n";

        chain::AckRequest ack;
        ack.set_key(req.key());
        ack.set_version(version);
        ack.set_client_addr(req.client_addr());
        ack.set_request_id(req.request_id());
        support_.send_client_ack(ack);

        return resp;
    }

    chain::PropagateRequest fwd;
    fwd.set_key(req.key());
    fwd.set_value(req.value());
    fwd.set_version(version);
    fwd.set_origin_id(node.id());
    fwd.set_client_addr(req.client_addr());
    fwd.set_request_id(req.request_id());

    auto succ = support_.successor_stub();
    forward_propagate_async(succ, std::move(fwd), craq_node_label(node));

    return resp;
}

chain::ReadResponse CRAQReplication::handle_read(const chain::ReadRequest& req, Node& node) {
    const auto local_clean = support_.read_clean(req.key());

    if (node.is_tail()) {
        if (!local_clean.found) {
            throw runtime_error("CRAQ read failed: key not found or not committed");
        }

        cout << "[CRAQ] Tail " << craq_node_label(node)
             << " serving CLEAN read key='" << req.key()
             << "' version=" << local_clean.version << "\n";
        return build_read_response(req.key(), local_clean.value, local_clean.version);
    }

    const auto local_latest = support_.read_latest_seen(req.key());
    const bool has_dirty = local_latest.found &&
                           (!local_clean.found || local_latest.version > local_clean.version);

    if (!has_dirty) {
        if (!local_clean.found) {
            throw runtime_error("CRAQ read failed: key not found");
        }

        cout << "[CRAQ] Non-tail " << craq_node_label(node)
             << " serving local CLEAN read key='" << req.key()
             << "' version=" << local_clean.version << "\n";
        return build_read_response(req.key(), local_clean.value, local_clean.version);
    }

    chain::VersionQueryRequest query;
    query.set_key(req.key());
    const chain::VersionQueryResponse tail_state = handle_version_query(query, node);
    const uint64_t latest_committed = tail_state.latest_version();

    if (latest_committed == 0) {
        throw runtime_error("CRAQ read failed: key not committed at tail");
    }

    if (local_clean.found && local_clean.version == latest_committed) {
        cout << "[CRAQ] Non-tail " << craq_node_label(node)
             << " dirty read path resolved to local CLEAN key='" << req.key()
             << "' version=" << local_clean.version << "\n";
        return build_read_response(req.key(), local_clean.value, local_clean.version);
    }

    string value;
    if (!support_.read_value_at_version(req.key(), latest_committed, value)) {
        throw runtime_error("CRAQ read failed: local node cannot materialize tail-committed version");
    }

    cout << "[CRAQ] Non-tail " << craq_node_label(node)
         << " served read using local DIRTY version matching tail key='" << req.key()
         << "' version=" << latest_committed << "\n";
    return build_read_response(req.key(), value, latest_committed);
}

void CRAQReplication::handle_propagate(const chain::PropagateRequest& req, Node& node) {
    support_.record_local_write(req.key(), req.value(), req.version());

    cout << "[CRAQ] Node " << craq_node_label(node)
         << " received DIRTY PROPAGATE key='" << req.key()
         << "' version=" << req.version() << "\n";

    if (node.is_tail()) {
        support_.mark_version_clean(req.key(), req.version());

        cout << "[CRAQ] Tail committed key='" << req.key()
             << "' version=" << req.version() << " and sending ACK to client\n";

        chain::AckRequest client_ack;
        client_ack.set_key(req.key());
        client_ack.set_version(req.version());
        client_ack.set_client_addr(req.client_addr());
        client_ack.set_request_id(req.request_id());
        support_.send_client_ack(client_ack);

        if (!node.is_head()) {
            chain::AckRequest ack;
            ack.set_key(req.key());
            ack.set_version(req.version());
            ack.set_client_addr(req.client_addr());
            ack.set_request_id(req.request_id());

            support_.enqueue_predecessor_ack(ack);
        }
        return;
    }

    auto succ = support_.successor_stub();
    chain::PropagateRequest fwd = req;
    forward_propagate_async(succ, std::move(fwd), craq_node_label(node));
}

void CRAQReplication::handle_ack(const chain::AckRequest& req, Node& node) {
    support_.mark_version_clean(req.key(), req.version());

    cout << "[CRAQ] Node " << craq_node_label(node)
         << " marked CLEAN key='" << req.key()
         << "' version=" << req.version() << "\n";

    if (node.is_head()) {
        cout << "[CRAQ] Head finalized commit request_id=" << req.request_id()
             << " key='" << req.key() << "' version=" << req.version()
             << "\n";
        return;
    }

    chain::AckRequest fwd = req;
    support_.enqueue_predecessor_ack(fwd);
}

chain::VersionQueryResponse CRAQReplication::handle_version_query(const chain::VersionQueryRequest& req, Node& node) {
    chain::VersionQueryResponse resp;
    resp.set_key(req.key());

    if (node.is_tail()) {
        const auto clean = support_.read_clean(req.key());
        resp.set_latest_version(clean.found ? clean.version : 0);
        return resp;
    }

    auto tail = support_.tail_stub();
    if (tail) {
        grpc::ClientContext ctx;
        chain::VersionQueryResponse downstream;
        grpc::Status status = tail->VersionQuery(&ctx, req, &downstream);
        if (!status.ok()) {
            throw runtime_error("CRAQ version query failed: " + status.error_message());
        }
        return downstream;
    }

    auto succ = support_.successor_stub();
    if (!succ) {
        throw runtime_error("CRAQ version query failed: missing successor or tail stub");
    }

    grpc::ClientContext ctx;
    chain::VersionQueryResponse downstream;
    grpc::Status status = succ->VersionQuery(&ctx, req, &downstream);
    if (!status.ok()) {
        throw runtime_error("CRAQ version query failed: " + status.error_message());
    }
    return downstream;
}

void CRAQReplication::on_config_change(Node& node) {
    support_.on_config_change(node);
    support_.start_ack_workers();
}
